#include "lume/kernel_memory.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
typedef HANDLE test_thread;
#define THREAD_RESULT DWORD WINAPI
#else
#include <pthread.h>
#include <sched.h>
typedef pthread_t test_thread;
#define THREAD_RESULT void *
#endif

static _Alignas(4096) unsigned char memory[2048][4096];
static unsigned used[2048], active, peak, pool_lock;
static _Thread_local bool irq_masked;

static void relax(void)
{
#ifdef _WIN32
    SwitchToThread();
#else
    sched_yield();
#endif
}

uint64_t lume_irq_save(void)
{
    bool previous = irq_masked;
    irq_masked = true;
    return previous;
}

void lume_irq_restore(uint64_t flags)
{
    assert(irq_masked && flags <= 1);
    irq_masked = flags != 0;
}

_Noreturn void lume_fail(const char *why)
{
    fprintf(stderr, "%s\n", why);
    abort();
}

static void pool_enter(void)
{
    assert(!irq_masked);
    while (__atomic_exchange_n(&pool_lock, 1, __ATOMIC_ACQUIRE))
        relax();
}

static void pool_leave(void)
{
    __atomic_store_n(&pool_lock, 0, __ATOMIC_RELEASE);
}

uint64_t lume_pages_alloc_span(unsigned count)
{
    pool_enter();
    for (unsigned first = 0; count && first + count <= 2048; ++first)
    {
        unsigned i = 0;
        while (i < count && !used[first + i])
            ++i;
        if (i != count)
        {
            first += i;
            continue;
        }
        for (i = 0; i < count; ++i)
            used[first + i] = 1;
        active += count;
        if (active > peak)
            peak = active;
        pool_leave();
        memset(memory[first], 0, count * 4096u);
        return (uintptr_t)memory[first];
    }
    pool_leave();
    return 0;
}

uint64_t lume_page_alloc(uint64_t minimum)
{
    assert(!minimum);
    return lume_pages_alloc_span(1);
}

bool lume_page_free(uint64_t address)
{
    pool_enter();
    uintptr_t offset = (uintptr_t)address - (uintptr_t)memory;
    assert(offset < sizeof(memory) && !(offset & 4095) && used[offset / 4096]);
    used[offset / 4096] = 0;
    --active;
    pool_leave();
    return true;
}

static const size_t sizes[] = {1,   16,  17,   48,   49,   112,  113,  240,  241,
                               496, 497, 1008, 1009, 2032, 2033, 4080, 4081, 65536};

static THREAD_RESULT exercise(void *context)
{
    unsigned id = (unsigned)(uintptr_t)context;
    void *objects[24] = {0};
    size_t lengths[24] = {0};
    for (unsigned iteration = 0; iteration < 4000; ++iteration)
    {
        unsigned slot = iteration % 24;
        unsigned char pattern = (unsigned char)(id * 29u + slot + 1u);
        if (objects[slot])
        {
            for (size_t i = 0; i < lengths[slot]; ++i)
                assert(((unsigned char *)objects[slot])[i] == pattern);
            lume_kernel_free(objects[slot]);
        }
        lengths[slot] = sizes[(iteration * 13u + id) % (sizeof(sizes) / sizeof(*sizes))];
        objects[slot] = lume_kernel_allocate(lengths[slot]);
        assert(objects[slot] && !((uintptr_t)objects[slot] & 15u) && !irq_masked);
        for (size_t i = 0; i < lengths[slot]; ++i)
            assert(!((unsigned char *)objects[slot])[i]);
        memset(objects[slot], pattern, lengths[slot]);
        assert(lume_kernel_allocation_pages(objects[slot]) ==
               (lengths[slot] + 16u + 4095u) / 4096u);
    }
    for (unsigned i = 0; i < 24; ++i)
        lume_kernel_free(objects[i]);
    return 0;
}

int main(void)
{
    assert(!lume_kernel_allocate(0) && !lume_kernel_allocate(16u * 1024u * 1024u + 1u));
    assert(!lume_kernel_allocation_pages(NULL));
    lume_kernel_free(NULL);
    void *private = lume_kernel_allocate_private(48);
    assert(private && ((uintptr_t)private & 4095u) == 16 && active == 1);
    assert(lume_kernel_allocation_pages(private) == 1);
    lume_kernel_free(private);
    assert(!active);
    void *objects[1024];
    for (unsigned i = 0; i < 1024; ++i)
    {
        objects[i] = lume_kernel_allocate(48);
        assert(objects[i]);
    }
    assert(active == 17);
    for (unsigned i = 0; i < 1024; ++i)
        lume_kernel_free(objects[i]);
    assert(active == 1);
    test_thread threads[4];
    for (unsigned i = 0; i < 4; ++i)
    {
#ifdef _WIN32
        threads[i] = CreateThread(NULL, 0, exercise, (void *)(uintptr_t)i, 0, NULL);
        assert(threads[i]);
#else
        assert(!pthread_create(&threads[i], NULL, exercise, (void *)(uintptr_t)i));
#endif
    }
    for (unsigned i = 0; i < 4; ++i)
    {
#ifdef _WIN32
        assert(WaitForSingleObject(threads[i], 30000) == WAIT_OBJECT_0);
        assert(CloseHandle(threads[i]));
#else
        assert(!pthread_join(threads[i], NULL));
#endif
    }
    assert(active == 7 && lume_kernel_cache_pages() == 7 && !irq_masked);
    printf(
        "KERNEL_MEMORY_PASS small_pages=17 previous_pages=1024 threads=4 operations=16000 retained_pages=%u peak=%u\n",
        active, peak);
    return 0;
}
