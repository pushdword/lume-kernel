#include <stdbool.h>
#include <stdint.h>
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
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

#define LUME_CPU_LIMIT 4u
static _Thread_local unsigned cpu_index;
static uint64_t ticks, wait_hooks[4], acquired_hooks[4], released_hooks[4];
static unsigned value, inside;

static unsigned lume_cpu_index(void)
{
    return cpu_index;
}

static uint64_t lume_counter_read(void)
{
    return __atomic_add_fetch(&ticks, 1, __ATOMIC_RELAXED);
}

static void relax(void)
{
#ifdef _WIN32
    SwitchToThread();
#else
    sched_yield();
#endif
}

static void kernel_lock_wait(void)
{
    ++wait_hooks[cpu_index];
    relax();
}

static void kernel_lock_acquired(void)
{
    ++acquired_hooks[cpu_index];
}

static void kernel_lock_released(void)
{
    ++released_hooks[cpu_index];
}

static jmp_buf rejected;
static const char *failure;
static bool expected_failure;

_Noreturn void lume_fail(const char *message)
{
    assert(expected_failure);
    failure = message;
    longjmp(rejected, 1);
}

#include "smp_lock.h"

static THREAD_RESULT contender(void *argument)
{
    cpu_index = (unsigned)(uintptr_t)argument;
    for (unsigned i = 0; i < 2000; ++i)
    {
        lume_smp_lock();
        assert(lume_smp_owned() && !__atomic_fetch_add(&inside, 1, __ATOMIC_RELAXED));
        ++value;
        assert(__atomic_fetch_sub(&inside, 1, __ATOMIC_RELAXED) == 1);
        lume_smp_unlock();
    }
    return 0;
}

int main(void)
{
    assert(!lume_smp_owned() && !lume_smp_contended());
    assert(!lume_smp_lock_turn() && !lume_smp_wait_ticks() && !lume_smp_wait_total() &&
           !lume_smp_wait_start(0));
    for (uint64_t turn = 1; turn <= 32; ++turn)
    {
        lume_smp_lock();
        assert(lume_smp_owned() && !lume_smp_contended() && lume_smp_lock_turn() == turn);
        expected_failure = true;
        if (!setjmp(rejected))
        {
            lume_smp_lock();
            assert(false);
        }
        expected_failure = false;
        assert(!strcmp(failure, "recursive kernel lock"));
        assert(lume_smp_owned() && lume_smp_lock_turn() == turn);
        lume_smp_unlock();
        assert(!lume_smp_owned() && !lume_smp_contended() && lume_smp_lock_turn() == turn);
        expected_failure = true;
        if (!setjmp(rejected))
        {
            lume_smp_unlock();
            assert(false);
        }
        expected_failure = false;
        assert(!strcmp(failure, "kernel lock ownership"));
        assert(!lume_smp_owned() && lume_smp_lock_turn() == turn);
    }
    kernel_lock.next = kernel_lock.serving = UINT32_MAX - 1;
    lume_smp_lock();
    test_thread threads[3];
    for (unsigned i = 0; i < 3; ++i)
    {
#ifdef _WIN32
        threads[i] = CreateThread(NULL, 0, contender, (void *)(uintptr_t)(i + 1), 0, NULL);
        assert(threads[i]);
#else
        assert(!pthread_create(&threads[i], NULL, contender, (void *)(uintptr_t)(i + 1)));
#endif
        while (!lume_smp_wait_start(i + 1))
            relax();
        assert(lume_smp_contended());
    }
    lume_smp_unlock();
    for (unsigned i = 0; i < 3; ++i)
    {
#ifdef _WIN32
        assert(WaitForSingleObject(threads[i], 20000) == WAIT_OBJECT_0);
        DWORD status;
        assert(GetExitCodeThread(threads[i], &status) && !status);
        assert(CloseHandle(threads[i]));
#else
        assert(!pthread_join(threads[i], NULL));
#endif
        unsigned cpu = i + 1;
        assert(wait_hooks[cpu] && acquired_hooks[cpu] == 2000 && released_hooks[cpu] == 2000);
        assert(lock_cpus[cpu].contentions && lock_cpus[cpu].max_wait_ticks);
        assert(!lock_cpus[cpu].held && !lume_smp_wait_start(cpu));
    }
    assert(value == 6000 && !inside && kernel_lock.next == kernel_lock.serving);
    assert(kernel_lock.serving == 5999 && acquired_hooks[0] == 33 && released_hooks[0] == 33);
    puts("SMP_LOCK_PASS recursion ownership wrap wait-service cpus=4 acquisitions=6033");
    return 0;
}
