#include "lume/early.h"
#include "lume/worker.h"
#if defined(LUME_ARCH_I686)
#define PHYSICAL_LIMIT UINT64_C(0x80000000)
#define IMAGE_BLOCK UINT64_C(0x400000)
#else
#define PHYSICAL_LIMIT UINT64_C(0x200000000)
#define IMAGE_BLOCK UINT64_C(0x200000)
#endif
#define MAX_PAGES (PHYSICAL_LIMIT / 4096)
#if defined(__aarch64__) || defined(LUME_ARCH_AARCH64) || defined(LUME_ARCH_AMD64)
#define MAX_SPAN_PAGES 262144u
#else
#define MAX_SPAN_PAGES 32768u
#endif
_Static_assert(MAX_SPAN_PAGES <= SIZE_MAX / 4096u, "page span exceeds addressable memory");
static uint64_t used[MAX_PAGES / 64], managed[MAX_PAGES / 64], free_count;
static uint64_t cursor;
static uint64_t span_cursor;
static uint64_t total_count;
static uint64_t region_cursor[16], region_pages;
static unsigned region_count, next_region;

static bool overlaps(uint64_t a, uint64_t size, uint64_t b, uint64_t bytes)
{
    return a < b + bytes && b < a + size;
}

void lume_pages_start(const struct lume_fdt *fdt, uint64_t dtb, uint64_t image_start,
                      uint64_t image_limit)
{
    /* The image's split 2 MiB window has unmapped slack and a stack guard.
     * Reserve the whole window, rather than handing those holes to alloc(). */
    uint64_t image_end = (image_limit + IMAGE_BLOCK - 1) & ~(IMAGE_BLOCK - 1);
    free_count = 0;
    cursor = 0;
    span_cursor = 1;
    for (unsigned i = 0; i < MAX_PAGES / 64; ++i)
    {
        used[i] = UINT64_MAX;
        managed[i] = 0;
    }
    for (unsigned r = 0; r < fdt->ram_count; ++r)
    {
        if (fdt->ram[r].base >= PHYSICAL_LIMIT)
            continue;
        uint64_t start = (fdt->ram[r].base + 0x1fffff) & ~UINT64_C(0x1fffff);
        uint64_t end = (fdt->ram[r].base + fdt->ram[r].size) & ~UINT64_C(0x1fffff);
        if (start < 0x200000)
            start = 0x200000;
        if (end > PHYSICAL_LIMIT)
            end = PHYSICAL_LIMIT;
        for (uint64_t p = start; p < end; p += 4096)
        {
            bool reserve =
                overlaps(p, 4096, LUME_USER_BASE, LUME_USER_LIMIT - LUME_USER_BASE) ||
                overlaps(p, 4096, image_start, image_end - image_start) ||
                overlaps(p, 4096, dtb, fdt->size) ||
                (fdt->initrd_end > fdt->initrd_start &&
                 overlaps(p, 4096, fdt->initrd_start, fdt->initrd_end - fdt->initrd_start));
            for (unsigned n = 0; n < fdt->reserved_count; ++n)
                if (overlaps(p, 4096, fdt->reserved[n].base, fdt->reserved[n].size))
                    reserve = true;
            uint64_t index = p / 4096, mask = UINT64_C(1) << (index & 63);
            if (!reserve && !(managed[index / 64] & mask))
            {
                managed[index / 64] |= mask;
                used[index / 64] &= ~mask;
                ++free_count;
            }
        }
    }
    total_count = free_count;
    region_count = 0;
    next_region = 0;
    for (unsigned i = 0; i < 16; ++i)
        region_cursor[i] = 0;
    if (fdt->pi5 && fdt->memory_regions > 1 && fdt->memory_regions <= 16 &&
        !(fdt->memory_regions & (fdt->memory_regions - 1)))
    {
        uint64_t extent = 0;
        for (unsigned r = 0; r < fdt->ram_count; ++r)
        {
            uint64_t end = fdt->ram[r].base + fdt->ram[r].size;
            if (end > PHYSICAL_LIMIT)
                end = PHYSICAL_LIMIT;
            if (end > extent)
                extent = end;
        }
        uint64_t aligned = 4096;
        while (aligned < extent)
            aligned *= 2;
        region_pages = aligned / 4096 / fdt->memory_regions;
        if (region_pages >= 512)
            region_count = fdt->memory_regions;
    }
}

uint64_t lume_pages_total(void)
{
    return total_count;
}

struct page_zero
{
    volatile uint64_t *data;
    size_t bytes;
};

static void zero_pages(void *context)
{
    const struct page_zero *request = context;
    for (size_t i = 0; i < request->bytes / sizeof(*request->data); ++i)
        request->data[i] = 0;
}

static uint64_t span_mask(uint64_t word, uint64_t first, uint64_t end)
{
    uint64_t base = word * 64;
    unsigned low = first > base ? (unsigned)(first - base) : 0;
    unsigned high = end - base < 64 ? (unsigned)(end - base) : 64;
    uint64_t mask = UINT64_MAX << low;
    return high < 64 ? mask & ((UINT64_C(1) << high) - 1) : mask;
}

static bool reserve_span(uint64_t first, unsigned count, uint64_t *retry)
{
    uint64_t end = first + count;
    for (uint64_t word = first / 64; word <= (end - 1) / 64; ++word)
    {
        uint64_t occupied = used[word] & span_mask(word, first, end);
        if (occupied)
        {
            *retry = word * 64 + (unsigned)__builtin_ctzll(occupied) + 1;
            return false;
        }
    }
    /* Revalidation and reservation cannot suspend. Search checkpoints retain only
     * numeric candidates, so another allocator may change a run before this point. */
    for (uint64_t word = first / 64; word <= (end - 1) / 64; ++word)
        used[word] |= span_mask(word, first, end);
    free_count -= count;
    span_cursor = end < MAX_PAGES ? end : 1;
    return true;
}

static uint64_t search_span(uint64_t first, uint64_t end, unsigned count, unsigned *budget)
{
    uint64_t run = 0;
    uint64_t word = first / 64;
    while (word <= (end - 1) / 64)
    {
        uint64_t available = ~used[word] & span_mask(word, first, end);
        unsigned prefix = available == UINT64_MAX ? 64 : (unsigned)__builtin_ctzll(~available);
        uint64_t candidate = 0;
        if (run && run + prefix >= count)
            candidate = word * 64 - run;
        else if (count <= 64)
        {
            uint64_t starts = available;
            for (unsigned length = 1; length < count && starts;)
            {
                unsigned step = count - length < length ? count - length : length;
                starts &= starts >> step;
                length += step;
            }
            if (starts)
                candidate = word * 64 + (unsigned)__builtin_ctzll(starts);
        }
        if (candidate)
        {
            uint64_t retry;
            if (reserve_span(candidate, count, &retry))
                return candidate;
            first = retry;
            word = first / 64;
            run = 0;
        }
        else
        {
            run = available == UINT64_MAX ? run + 64 : (unsigned)__builtin_clzll(~available);
            ++word;
        }
        if (++*budget == 256)
        {
            *budget = 0;
            lume_worker_checkpoint(LUME_WORKER_SLICE_BYTES);
            if (free_count < count)
                return 0;
        }
    }
    return 0;
}

uint64_t lume_pages_alloc_span_uninitialized(unsigned count)
{
    if (!count || count > MAX_SPAN_PAGES || count > MAX_PAGES || free_count < count)
        return 0;
    unsigned budget = 0;
    uint64_t start = span_cursor && span_cursor < MAX_PAGES ? span_cursor : 1;
    uint64_t first = search_span(start, MAX_PAGES, count, &budget);
    if (!first && start > 1)
    {
        /* The second interval includes the tail needed by runs crossing the cursor.
         * A physical span never wraps from the final page back to page one. */
        uint64_t end = start + count - 1;
        if (end > MAX_PAGES)
            end = MAX_PAGES;
        first = search_span(1, end, count, &budget);
    }
    if (!first)
        return 0;
    return first * 4096;
}

uint64_t lume_pages_alloc_span(unsigned count)
{
    uint64_t physical = lume_pages_alloc_span_uninitialized(count);
    if (!physical)
        return 0;
    /* Reserved pages remain unpublished until their complete span is initialized. */
    size_t bytes = (size_t)count * 4096;
    for (size_t offset = 0; offset < bytes;)
    {
        size_t length =
            bytes - offset < LUME_WORKER_SLICE_BYTES ? bytes - offset : LUME_WORKER_SLICE_BYTES;
        struct page_zero request = {
            .data = (void *)(uintptr_t)(physical + offset),
            .bytes = length,
        };
        if (length == LUME_WORKER_SLICE_BYTES)
        {
            lume_worker_compute(zero_pages, &request);
            lume_worker_checkpoint(0);
        }
        else
        {
            zero_pages(&request);
            lume_worker_checkpoint((unsigned)length);
        }
        offset += length;
    }
    return physical;
}

static uint64_t find_page(uint64_t first, uint64_t end)
{
    for (uint64_t word = first / 64; first < end && word <= (end - 1) / 64; ++word)
    {
        uint64_t available = ~used[word] & span_mask(word, first, end);
        if (available)
            return word * 64 + (unsigned)__builtin_ctzll(available);
    }
    return 0;
}

uint64_t lume_page_alloc_uninitialized(uint64_t minimum)
{
    if (!free_count || minimum >= PHYSICAL_LIMIT)
        return 0;
    uint64_t first = (minimum + 4095) / 4096;
    if (!first)
        first = 1;
    uint64_t p = 0;
    if (region_count)
    {
        /* Firmware regions select SDRAM banks. Rotate single-page allocations
         * across banks while keeping DMA spans physically contiguous. */
        for (unsigned n = 0; n < region_count; ++n)
        {
            unsigned region = (next_region + n) % region_count;
            uint64_t low = region * region_pages, end = low + region_pages;
            if (end > MAX_PAGES)
                end = MAX_PAGES;
            if (low < first)
                low = first;
            if (low >= end)
                continue;
            uint64_t start = region_cursor[region];
            if (start < low || start >= end)
                start = low;
            p = find_page(start, end);
            if (!p && start > low)
                p = find_page(low, start);
            if (p)
            {
                region_cursor[region] = p + 1;
                next_region = (region + 1) % region_count;
                break;
            }
        }
    }
    else
    {
        uint64_t start = cursor >= first && cursor < MAX_PAGES ? cursor : first;
        p = find_page(start, MAX_PAGES);
        if (!p && start > first)
            p = find_page(first, start);
    }
    if (!p)
        return 0;
    used[p / 64] |= UINT64_C(1) << (p & 63);
    --free_count;
    cursor = p + 1;
    return p * 4096;
}

uint64_t lume_page_alloc(uint64_t minimum)
{
    uint64_t physical = lume_page_alloc_uninitialized(minimum);
    if (physical)
    {
        volatile uint64_t *data = (void *)(uintptr_t)physical;
        for (unsigned i = 0; i < 512; ++i)
            data[i] = 0;
    }
    return physical;
}

bool lume_page_free(uint64_t page)
{
    if (!page || page >= PHYSICAL_LIMIT || (page & 4095))
        return false;
    uint64_t p = page / 4096, mask = UINT64_C(1) << (p & 63);
    if (!(managed[p / 64] & mask) || !(used[p / 64] & mask))
        return false;
    used[p / 64] &= ~mask;
    ++free_count;
    return true;
}

uint64_t lume_pages_free(void)
{
    return free_count;
}

bool lume_pages_check(void)
{
    uint64_t before = free_count, pages[129];
    if (before < 129)
        return false;
    for (unsigned n = 0; n < 129; ++n)
    {
        pages[n] = lume_page_alloc(n == 128 ? UINT64_C(0x100000000) : 0);
        if (!pages[n])
            return false;
        volatile uint64_t *p = (void *)(uintptr_t)pages[n];
        if (p[0] || p[511])
            return false;
        p[0] = pages[n] ^ UINT64_C(0x4c554d45);
        p[511] = ~p[0];
    }
    lume_value("PAGE_TEST_ABOVE_4G", pages[128]);
    for (unsigned n = 0; n < 129; ++n)
    {
        volatile uint64_t *p = (void *)(uintptr_t)pages[n];
        if (p[0] != (pages[n] ^ UINT64_C(0x4c554d45)) || p[511] != ~p[0])
            return false;
    }
    for (unsigned n = 129; n; --n)
        if (!lume_page_free(pages[n - 1]))
            return false;
    return free_count == before && !lume_page_free(pages[0]) && !lume_page_free(0x200000);
}
