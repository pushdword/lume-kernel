#include "lume/ticket_lock.h"
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
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

#define THREADS 4u
#define ITERATIONS 10000u

static struct lume_ticket_lock lock;
static unsigned enter, reserved[THREADS], go[THREADS];
static uint32_t tickets[THREADS], expected;
static unsigned value, checksum;

static void relax(void)
{
#ifdef _WIN32
    SwitchToThread();
#else
    sched_yield();
#endif
}

static void wait_flag(unsigned *flag)
{
    while (!__atomic_load_n(flag, __ATOMIC_ACQUIRE))
        relax();
}

static void publish_flag(unsigned *flag)
{
    __atomic_store_n(flag, 1, __ATOMIC_RELEASE);
}

static void critical(uint32_t ticket)
{
    assert(!__atomic_fetch_add(&enter, 1, __ATOMIC_RELAXED));
    assert(lume_ticket_ready(&lock, ticket));
    assert(expected == ticket && checksum == (value ^ 0x51a7u));
    ++value;
    checksum = value ^ 0x51a7u;
    ++expected;
    assert(__atomic_fetch_sub(&enter, 1, __ATOMIC_RELAXED) == 1);
}

static void acquire(uint32_t ticket)
{
    while (!lume_ticket_ready(&lock, ticket))
        relax();
}

static THREAD_RESULT fifo_thread(void *argument)
{
    unsigned id = (unsigned)(uintptr_t)argument;
    wait_flag(&go[id]);
    uint32_t ticket = lume_ticket_reserve(&lock);
    tickets[id] = ticket;
    publish_flag(&reserved[id]);
    acquire(ticket);
    critical(ticket);
    lume_ticket_release(&lock, ticket);
    return 0;
}

static THREAD_RESULT stress_thread(void *argument)
{
    unsigned id = (unsigned)(uintptr_t)argument;
    publish_flag(&reserved[id]);
    wait_flag(&go[0]);
    for (unsigned i = 0; i < ITERATIONS; ++i)
    {
        uint32_t ticket = lume_ticket_reserve(&lock);
        acquire(ticket);
        critical(ticket);
        lume_ticket_release(&lock, ticket);
    }
    return 0;
}

static test_thread start_thread(THREAD_RESULT (*function)(void *), unsigned id)
{
    test_thread thread;
#ifdef _WIN32
    thread = CreateThread(NULL, 0, function, (void *)(uintptr_t)id, 0, NULL);
    assert(thread);
#else
    assert(!pthread_create(&thread, NULL, function, (void *)(uintptr_t)id));
#endif
    return thread;
}

static void finish_thread(test_thread thread)
{
#ifdef _WIN32
    assert(WaitForSingleObject(thread, 20000) == WAIT_OBJECT_0);
    DWORD status;
    assert(GetExitCodeThread(thread, &status) && !status);
    assert(CloseHandle(thread));
#else
    assert(!pthread_join(thread, NULL));
#endif
}

static void reset(uint32_t seed)
{
    lock.next = lock.serving = expected = seed;
    enter = value = 0;
    checksum = 0x51a7u;
    for (unsigned i = 0; i < THREADS; ++i)
        reserved[i] = go[i] = tickets[i] = 0;
}

static void fifo(uint32_t seed)
{
    reset(seed);
    uint32_t owner = lume_ticket_reserve(&lock);
    assert(owner == seed && lume_ticket_ready(&lock, owner));
    assert(!lume_ticket_contended(&lock));
    test_thread threads[THREADS];
    for (unsigned i = 0; i < THREADS; ++i)
    {
        threads[i] = start_thread(fifo_thread, i);
        publish_flag(&go[i]);
        wait_flag(&reserved[i]);
        assert(tickets[i] == (uint32_t)(seed + i + 1));
        assert(!lume_ticket_ready(&lock, tickets[i]));
        assert(lume_ticket_contended(&lock));
    }
    critical(owner);
    lume_ticket_release(&lock, owner);
    for (unsigned i = 0; i < THREADS; ++i)
        finish_thread(threads[i]);
    assert(value == THREADS + 1 && expected == (uint32_t)(seed + THREADS + 1));
    assert(lock.next == lock.serving && !lume_ticket_contended(&lock) && !enter);
}

static void stress(uint32_t seed)
{
    reset(seed);
    test_thread threads[THREADS];
    for (unsigned i = 0; i < THREADS; ++i)
        threads[i] = start_thread(stress_thread, i);
    for (unsigned i = 0; i < THREADS; ++i)
        wait_flag(&reserved[i]);
    publish_flag(&go[0]);
    for (unsigned i = 0; i < THREADS; ++i)
        finish_thread(threads[i]);
    assert(value == THREADS * ITERATIONS && checksum == (value ^ 0x51a7u));
    assert(expected == (uint32_t)(seed + THREADS * ITERATIONS));
    assert(lock.next == lock.serving && !enter && !lume_ticket_contended(&lock));
}

int main(void)
{
    _Static_assert(offsetof(struct lume_ticket_lock, serving) == 64, "separate ticket cache lines");
    _Static_assert(sizeof(struct lume_ticket_lock) == 128, "ticket lock size");
    _Static_assert(_Alignof(struct lume_ticket_lock) == 64, "ticket lock alignment");
    fifo(0);
    fifo(UINT32_MAX - 2);
    stress(0);
    stress(UINT32_MAX - 1000);
    puts("TICKET_LOCK_PASS fifo=2 stress=80000 wrap publication ownership");
    return 0;
}
