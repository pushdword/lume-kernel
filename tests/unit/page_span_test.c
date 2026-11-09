#define _GNU_SOURCE
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#include "../../src/mm/pages.c"

#define REGION_BYTES (4u * 1024u * 1024u)
static unsigned char *region;
static uint64_t region_page;
static unsigned checkpoints, search_checkpoints, zero_checkpoints, computations, cases;
static uint64_t expected_first;
static unsigned expected_count;
static size_t zeroed;
static size_t computed_bytes;
static unsigned *search_budget;
static uint64_t collision;
static bool drain_free;

static bool occupied(uint64_t page)
{
    return (used[page / 64] & (UINT64_C(1) << (page & 63))) != 0;
}

static bool expected_reserved(void)
{
    if (!expected_count)
        return false;
    for (unsigned i = 0; i < expected_count; ++i)
        if (!occupied(expected_first + i))
            return false;
    return true;
}

void lume_worker_checkpoint(unsigned bytes)
{
    assert(bytes <= LUME_WORKER_SLICE_BYTES);
    ++checkpoints;
    if (expected_reserved())
    {
        assert(computed_bytes ? !bytes : bytes != 0);
        size_t completed = computed_bytes ? computed_bytes : bytes;
        assert(zeroed + completed <= (size_t)expected_count * 4096);
        zeroed += completed;
        computed_bytes = 0;
        const unsigned char *data = (void *)(uintptr_t)(expected_first * 4096);
        for (size_t i = 0; i < (size_t)expected_count * 4096; ++i)
            assert(data[i] == (i < zeroed ? 0 : 0xa5));
        ++zero_checkpoints;
        return;
    }
    assert(bytes == LUME_WORKER_SLICE_BYTES);
    if (search_budget)
        assert(!*search_budget);
    ++search_checkpoints;
    if (collision)
    {
        uint64_t retry;
        assert(reserve_span(collision, 1, &retry));
        collision = 0;
    }
    if (drain_free)
    {
        memset(used, 0xff, sizeof(used));
        free_count = 0;
        drain_free = false;
    }
}

void lume_worker_compute(void (*run)(void *), void *context)
{
    const struct page_zero *request = context;
    assert(run == zero_pages && expected_reserved());
    assert(request->bytes == LUME_WORKER_SLICE_BYTES);
    assert((uintptr_t)request->data == expected_first * 4096 + zeroed);
    assert(!computed_bytes);
    ++computations;
    run(context);
    computed_bytes = request->bytes;
}

void lume_value(const char *name, uint64_t value)
{
    (void)name;
    (void)value;
}

static void reset(void)
{
    memset(used, 0xff, sizeof(used));
    memset(managed, 0, sizeof(managed));
    memset(region, 0xa5, REGION_BYTES);
    free_count = total_count = cursor = 0;
    span_cursor = 1;
    checkpoints = search_checkpoints = zero_checkpoints = computations = 0;
    expected_first = expected_count = zeroed = computed_bytes = 0;
    search_budget = NULL;
    collision = 0;
    drain_free = false;
}

static void release_run(uint64_t first, unsigned count)
{
    assert(first && count && first + count <= MAX_PAGES);
    for (unsigned i = 0; i < count; ++i)
    {
        uint64_t page = first + i, mask = UINT64_C(1) << (page & 63);
        assert(occupied(page));
        managed[page / 64] |= mask;
        used[page / 64] &= ~mask;
        ++free_count;
    }
}

static uint64_t allocate(unsigned count, uint64_t first)
{
    uint64_t before = free_count;
    expected_first = first;
    expected_count = count;
    zeroed = 0;
    unsigned computes_before = computations, zeros_before = zero_checkpoints;
    assert(first >= region_page && first + count <= region_page + REGION_BYTES / 4096);
    unsigned char *data = (void *)(uintptr_t)(first * 4096);
    bool guard_before = first > region_page;
    bool guard_after = first + count < region_page + REGION_BYTES / 4096;
    unsigned char before_byte = guard_before ? data[-1] : 0;
    unsigned char after_byte = guard_after ? data[(size_t)count * 4096] : 0;
    memset(data, 0xa5, (size_t)count * 4096);
    uint64_t page = lume_pages_alloc_span(count);
    assert(page == first * 4096 && free_count == before - count);
    assert(zeroed == (size_t)count * 4096 && expected_reserved());
    assert(!guard_before || data[-1] == before_byte);
    assert(!guard_after || data[(size_t)count * 4096] == after_byte);
    assert(computations - computes_before == (size_t)count * 4096 / LUME_WORKER_SLICE_BYTES);
    assert(zero_checkpoints - zeros_before ==
           ((size_t)count * 4096 + LUME_WORKER_SLICE_BYTES - 1) / LUME_WORKER_SLICE_BYTES);
    expected_count = 0;
    ++cases;
    return page;
}

static void masks_and_search(void)
{
    for (unsigned low = 0; low < 64; ++low)
        for (unsigned high = low + 1; high <= 64; ++high)
        {
            uint64_t mask = 0;
            for (unsigned i = low; i < high; ++i)
                mask |= UINT64_C(1) << i;
            assert(span_mask(7, 448 + low, 448 + high) == mask);
            ++cases;
        }
    uint32_t random = 0x9b71634d;
    for (unsigned test = 0; test < 320; ++test)
    {
        reset();
        uint64_t begin = 1 + test % 63, end = begin + 192;
        unsigned count = 1 + test % 96;
        for (uint64_t p = begin; p < end; ++p)
        {
            random ^= random << 13;
            random ^= random >> 17;
            random ^= random << 5;
            if ((random & 31) < 29)
                release_run(p, 1);
        }
        uint64_t expected = 0;
        for (uint64_t p = begin; p + count <= end && !expected; ++p)
        {
            unsigned n = 0;
            while (n < count && !occupied(p + n))
                ++n;
            if (n == count)
                expected = p;
        }
        uint64_t before = free_count;
        unsigned budget = 0;
        assert(search_span(begin, end, count, &budget) == expected);
        assert(free_count == before - (expected ? count : 0));
        assert(!checkpoints);
        if (expected)
            for (unsigned i = 0; i < count; ++i)
                assert(occupied(expected + i));
        ++cases;
    }
}

static void cursor_and_wrap(void)
{
    reset();
    release_run(region_page + 10, 20);
    span_cursor = region_page + 10;
    allocate(5, region_page + 10);
    assert(span_cursor == region_page + 15);
    allocate(5, region_page + 15);
    assert(span_cursor == region_page + 20);

    reset();
    release_run(region_page + 10, 8);
    release_run(region_page + 200, 10);
    span_cursor = region_page + 195;
    allocate(8, region_page + 200);
    allocate(8, region_page + 10);
    assert(span_cursor == region_page + 18 && search_checkpoints);

    reset();
    release_run(region_page + 100, 10);
    span_cursor = region_page + 105;
    allocate(10, region_page + 100);
    assert(span_cursor == region_page + 110);

    reset();
    release_run(1, 4);
    release_run(MAX_PAGES - 4, 4);
    span_cursor = MAX_PAGES - 4;
    assert(!lume_pages_alloc_span(8) && free_count == 8);
    assert(span_cursor == MAX_PAGES - 4);
    ++cases;
}

static void checkpoints_and_collision(void)
{
    reset();
    for (uint64_t page = 64; page < 64 + 700 * 64; page += 2)
        release_run(page, 1);
    unsigned budget = 0;
    search_budget = &budget;
    uint64_t before = free_count;
    assert(!search_span(64, 64 + 700 * 64, 2, &budget));
    assert(search_checkpoints == 2 && budget == 188 && free_count == before);
    ++cases;

    reset();
    release_run(64, 16500);
    budget = 0;
    search_budget = &budget;
    collision = 74;
    assert(search_span(64, 64 + 16500, 16385, &budget) == 75);
    assert(!collision && occupied(74) && free_count == 16500 - 1 - 16385);
    for (uint64_t page = 64; page < 74; ++page)
        assert(!occupied(page));
    for (uint64_t page = 75; page < 75 + 16385; ++page)
        assert(occupied(page));
    assert(search_checkpoints >= 2 && span_cursor == 75 + 16385);
    ++cases;

    reset();
    release_run(64, 16500);
    budget = 0;
    search_budget = &budget;
    drain_free = true;
    assert(!search_span(64, 64 + 16500, 16385, &budget));
    assert(!free_count && search_checkpoints == 1 && !budget);
    ++cases;

    reset();
    release_run(63, 130);
    uint64_t retry;
    assert(reserve_span(100, 1, &retry));
    before = free_count;
    uint64_t previous[] = {used[0], used[1], used[2], used[3]};
    assert(!reserve_span(63, 130, &retry) && retry == 101);
    assert(free_count == before && !memcmp(previous, used, sizeof(previous)));
    ++cases;
}

static void zero_free_and_failures(void)
{
    static const unsigned sizes[] = {1, 15, 16, 17, 63, 64, 65, 129, 257};
    for (unsigned n = 0; n < sizeof(sizes) / sizeof(sizes[0]); ++n)
    {
        reset();
        unsigned count = sizes[n];
        release_run(region_page + 63, count);
        span_cursor = region_page + 63;
        uint64_t first = allocate(count, region_page + 63);
        assert(!free_count);
        for (unsigned i = 0; i < count; ++i)
            assert(lume_page_free(first + (uint64_t)i * 4096));
        assert(free_count == count && !lume_page_free(first));
    }
    reset();
    release_run(region_page, 3);
    cursor = region_page;
    uint64_t one = lume_page_alloc(region_page * 4096 + 1);
    assert(one == (region_page + 1) * 4096 && free_count == 2);
    const unsigned char *data = (void *)(uintptr_t)one;
    for (unsigned i = 0; i < 4096; ++i)
        assert(!data[i]);
    assert(lume_page_free(one) && free_count == 3);
    assert(!lume_page_free(one) && !lume_page_free(one + 1));
    assert(!lume_page_free(0) && !lume_page_free(PHYSICAL_LIMIT));
    assert(!lume_page_free((region_page + 4) * 4096));
    assert(!lume_page_alloc(PHYSICAL_LIMIT));
    assert(!lume_pages_alloc_span(0) && !lume_pages_alloc_span(MAX_SPAN_PAGES + 1));
    assert(!lume_pages_alloc_span(4) && free_count == 3 && !checkpoints);
    ++cases;
}

static void map_region(void)
{
    for (uintptr_t candidate = 0x10000000; candidate < 0x38000000 && !region;
         candidate += 0x1000000)
    {
#ifdef _WIN32
        region =
            VirtualAlloc((void *)candidate, REGION_BYTES, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
        void *memory = mmap((void *)candidate, REGION_BYTES, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (memory != MAP_FAILED)
        {
            if ((uintptr_t)memory + REGION_BYTES <= PHYSICAL_LIMIT)
                region = memory;
            else
                assert(!munmap(memory, REGION_BYTES));
        }
#endif
    }
    assert(region && !((uintptr_t)region & 4095));
    assert((uintptr_t)region + REGION_BYTES <= PHYSICAL_LIMIT);
    region_page = (uintptr_t)region / 4096;
}

int main(void)
{
    map_region();
    masks_and_search();
    cursor_and_wrap();
    checkpoints_and_collision();
    zero_free_and_failures();
#ifdef _WIN32
    assert(VirtualFree(region, 0, MEM_RELEASE));
#else
    assert(!munmap(region, REGION_BYTES));
#endif
    printf(
        "PASS_PAGE_SPAN %u cases cover word search, cursor wrap, collision, checkpoints and zeroing\n",
        cases);
    return 0;
}
