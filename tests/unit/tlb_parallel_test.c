#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#include <windows.h>
static LARGE_INTEGER frequency;
#else
#include <pthread.h>
#include <time.h>
#endif

#define LUME_CPU_LIMIT 4u
static _Thread_local unsigned cpu;
static _Thread_local bool independent;
static unsigned requests, completed, notifications[LUME_CPU_LIMIT], reloads[LUME_CPU_LIMIT];

static unsigned lume_cpu_index(void)
{
    return cpu;
}

static unsigned lume_cpu_count(void)
{
    return LUME_CPU_LIMIT;
}

static uint64_t lume_counter_frequency(void)
{
#ifdef _WIN32
    return (uint64_t)frequency.QuadPart;
#else
    return UINT64_C(1000000000);
#endif
}

static uint64_t lume_counter_read(void)
{
#ifdef _WIN32
    LARGE_INTEGER now;
    assert(QueryPerformanceCounter(&now));
    return (uint64_t)now.QuadPart;
#else
    struct timespec now;
    assert(!clock_gettime(CLOCK_MONOTONIC, &now));
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + now.tv_nsec;
#endif
}

static void lume_cpu_relax_ordered(void)
{
    __atomic_signal_fence(__ATOMIC_SEQ_CST);
}

static _Noreturn void lume_fail(const char *message)
{
    fprintf(stderr, "TLB_PARALLEL_FAIL %s\n", message);
    abort();
}

static void tlb_reload(void)
{
    assert(independent);
    ++reloads[cpu];
}

static void tlb_notify(unsigned target)
{
    assert(independent && target != cpu && target < LUME_CPU_LIMIT);
    __atomic_fetch_add(notifications + target, 1, __ATOMIC_RELEASE);
}

static void lume_task_tlb_sync(void (*run)(void *), void *context)
{
    assert(!independent);
    independent = true;
    run(context);
    independent = false;
}

#include "../../arch/amd64/tlb_sync.h"

#ifdef _WIN32
static DWORD WINAPI request_thread(void *context)
#else
static void *request_thread(void *context)
#endif
{
    cpu = (unsigned)(uintptr_t)context;
    for (unsigned i = 0; i < 800; ++i)
    {
        lume_smp_tlb_flush();
        __atomic_fetch_add(&requests, 1, __ATOMIC_RELAXED);
    }
    __atomic_fetch_or(&completed, 1u << cpu, __ATOMIC_RELEASE);
    independent = true;
    while (__atomic_load_n(&completed, __ATOMIC_ACQUIRE) != 15)
        lume_smp_tlb_service();
    independent = false;
    return 0;
}

int main(void)
{
#ifdef _WIN32
    HANDLE threads[4];
    assert(QueryPerformanceFrequency(&frequency));
#else
    pthread_t threads[4];
#endif
    for (unsigned i = 0; i < 4; ++i)
    {
#ifdef _WIN32
        threads[i] = CreateThread(NULL, 0, request_thread, (void *)(uintptr_t)i, 0, NULL);
        assert(threads[i]);
#else
        assert(!pthread_create(threads + i, NULL, request_thread, (void *)(uintptr_t)i));
#endif
    }
    for (unsigned i = 0; i < 4; ++i)
    {
#ifdef _WIN32
        assert(WaitForSingleObject(threads[i], 10000) == WAIT_OBJECT_0);
        CloseHandle(threads[i]);
#else
        assert(!pthread_join(threads[i], NULL));
#endif
    }
    assert(requests == 3200 && tlb_generation == 3200);
    unsigned total_notifications = 0;
    for (unsigned i = 0; i < 4; ++i)
    {
        assert(tlb_ack[i] == tlb_generation && reloads[i] >= 3200);
        total_notifications += notifications[i];
    }
    assert(total_notifications == 9600);
    puts("TLB_PARALLEL_PASS cpus=4 requests=3200 notifications=9600 acknowledgements=12800");
    return 0;
}
