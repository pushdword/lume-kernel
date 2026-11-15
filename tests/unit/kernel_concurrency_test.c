#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
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

#include "../../src/mm/pages.c"
#include "../../src/core/work.c"

static _Thread_local bool irq_masked;
static unsigned owners[4096];
static unsigned done, callbacks, in_callback[LUME_WORK_DOMAINS];
static unsigned callback_entered, callback_release;

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

static void relax(void)
{
#ifdef _WIN32
    SwitchToThread();
#else
    sched_yield();
#endif
}

static void wait_for(unsigned *value, unsigned expected)
{
    time_t deadline = time(NULL) + 20;
    while (__atomic_load_n(value, __ATOMIC_ACQUIRE) != expected)
    {
        assert(time(NULL) < deadline);
        relax();
    }
}

static test_thread start_thread(THREAD_RESULT (*run)(void *), unsigned id)
{
    test_thread thread;
#ifdef _WIN32
    thread = CreateThread(NULL, 0, run, (void *)(uintptr_t)id, 0, NULL);
    assert(thread);
#else
    assert(!pthread_create(&thread, NULL, run, (void *)(uintptr_t)id));
#endif
    return thread;
}

static void finish_thread(test_thread thread)
{
#ifdef _WIN32
    assert(WaitForSingleObject(thread, 20000) == WAIT_OBJECT_0);
    assert(CloseHandle(thread));
#else
    assert(!pthread_join(thread, NULL));
#endif
}

void lume_worker_checkpoint(unsigned bytes)
{
    assert(!irq_masked && bytes <= LUME_WORKER_SLICE_BYTES);
    relax();
}

void lume_worker_compute(void (*run)(void *), void *context)
{
    assert(!irq_masked);
    run(context);
}

void lume_value(const char *name, uint64_t value)
{
    (void)name;
    (void)value;
}

static THREAD_RESULT allocate_thread(void *context)
{
    unsigned id = (unsigned)(uintptr_t)context;
    for (unsigned iteration = 0; iteration < 1000; ++iteration)
    {
        unsigned count = iteration % 3 == 0 ? 65 : 1;
        uint64_t address = count == 1 ? lume_page_alloc_uninitialized(4096)
                                      : lume_pages_alloc_span_uninitialized(count);
        unsigned first = (unsigned)(address / 4096);
        assert(first && first + count <= 4096);
        for (unsigned i = 0; i < count; ++i)
            assert(!__atomic_exchange_n(&owners[first + i], id, __ATOMIC_ACQ_REL));
        assert(lume_page_pin(address));
        assert(lume_page_free(address));
        assert(!lume_page_free(address));
        for (unsigned i = 1; i < count; ++i)
        {
            assert(__atomic_exchange_n(&owners[first + i], 0, __ATOMIC_ACQ_REL) == id);
            assert(lume_page_free(address + i * 4096));
        }
        assert(__atomic_exchange_n(&owners[first], 0, __ATOMIC_ACQ_REL) == id);
        lume_page_unpin(address);
        assert(!irq_masked);
    }
    return 0;
}

struct queued_item
{
    struct lume_work work;
    unsigned sequence, completed;
};
static struct queued_item items[4];

static void callback(void *context)
{
    struct queued_item *item = context;
    unsigned domain = item->work.domain;
    assert(!irq_masked);
    assert(!__atomic_fetch_add(&in_callback[domain], 1, __ATOMIC_ACQ_REL));
    unsigned sequence = __atomic_load_n(&item->sequence, __ATOMIC_ACQUIRE);
    assert(sequence == __atomic_load_n(&item->completed, __ATOMIC_RELAXED) + 1);
    __atomic_fetch_add(&callbacks, 1, __ATOMIC_RELAXED);
    assert(__atomic_fetch_sub(&in_callback[domain], 1, __ATOMIC_ACQ_REL) == 1);
    __atomic_store_n(&item->completed, sequence, __ATOMIC_RELEASE);
}

static void held_callback(void *context)
{
    (void)context;
    assert(!irq_masked);
    __atomic_store_n(&callback_entered, 1, __ATOMIC_RELEASE);
    wait_for(&callback_release, 1);
}

static THREAD_RESULT produce(void *context)
{
    struct queued_item *item = &items[(uintptr_t)context];
    for (unsigned sequence = 1; sequence <= 1000; ++sequence)
    {
        __atomic_store_n(&item->sequence, sequence, __ATOMIC_RELEASE);
        assert(!lume_work_submit(&item->work));
        wait_for(&item->completed, sequence);
    }
    return 0;
}

static THREAD_RESULT consume(void *context)
{
    unsigned domain = (unsigned)(uintptr_t)context;
    while (!__atomic_load_n(&done, __ATOMIC_ACQUIRE))
    {
        if (lume_work_pending_on(domain))
            lume_work_drain_on(domain, 4);
        relax();
    }
    return 0;
}

int main(void)
{
    memset(used, 0xff, sizeof(used));
    for (unsigned page = 1; page < 4096; ++page)
    {
        uint64_t mask = UINT64_C(1) << (page & 63);
        managed[page / 64] |= mask;
        used[page / 64] &= ~mask;
    }
    free_count = total_count = 4095;
    span_cursor = 1;
    pins_start();
    test_thread threads[4], consumers[4];
    for (unsigned i = 0; i < 4; ++i)
        threads[i] = start_thread(allocate_thread, i + 1);
    for (unsigned i = 0; i < 4; ++i)
        finish_thread(threads[i]);
    assert(lume_pages_free() == lume_pages_total());
    for (unsigned i = 0; i < 4096; ++i)
        assert(!owners[i]);

    for (unsigned i = 0; i < 4; ++i)
    {
        items[i].work =
            (struct lume_work){.run = callback, .context = &items[i], .domain = 1 + i % 2};
        consumers[i] = start_thread(consume, 1 + i % 2);
        threads[i] = start_thread(produce, i);
    }
    for (unsigned i = 0; i < 4; ++i)
        finish_thread(threads[i]);
    assert(__atomic_load_n(&callbacks, __ATOMIC_ACQUIRE) == 4000);
    struct lume_work held = {.run = held_callback, .domain = 1};
    assert(!lume_work_submit(&held));
    wait_for(&callback_entered, 1);
    assert(!lume_work_cancel(&held));
    /* A second domain must finish while the first callback remains blocked. */
    __atomic_store_n(&items[1].sequence, 1001, __ATOMIC_RELEASE);
    assert(!lume_work_submit(&items[1].work));
    wait_for(&items[1].completed, 1001);
    __atomic_store_n(&callback_release, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&done, 1, __ATOMIC_RELEASE);
    for (unsigned i = 0; i < 4; ++i)
        finish_thread(consumers[i]);
    assert(!lume_work_pending());
    puts("PASS_KERNEL_CONCURRENCY allocator=4000 spans=1336 callbacks=4001 domains=2 consumers=4");
    return 0;
}
