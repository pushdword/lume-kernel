#ifndef _WIN32
#define _XOPEN_SOURCE 700
#endif
#define MODULE_WORK_PARALLEL
#define main module_services_serial
#include "module_services_test.c"
#undef main
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <sched.h>
#endif

struct work_case
{
    struct lume_work work;
    unsigned calls;
    bool resubmit;
};

static unsigned ready_threads;
static _Thread_local bool scheduler_owned;
#ifdef _WIN32
static CRITICAL_SECTION scheduler_lock;
#else
static pthread_mutex_t scheduler_lock = PTHREAD_MUTEX_INITIALIZER;
#endif

void lume_worker_scheduler(void (*run)(void *), void *context)
{
    bool entered = !scheduler_owned;
    if (entered)
    {
#ifdef _WIN32
        EnterCriticalSection(&scheduler_lock);
#else
        assert(!pthread_mutex_lock(&scheduler_lock));
#endif
        scheduler_owned = true;
    }
    run(context);
    if (entered)
    {
        scheduler_owned = false;
#ifdef _WIN32
        LeaveCriticalSection(&scheduler_lock);
#else
        assert(!pthread_mutex_unlock(&scheduler_lock));
#endif
    }
}

struct drain_request
{
    unsigned domain, done;
};

static void drain_callback(void *context)
{
    struct drain_request *request = context;
    assert(scheduler_owned);
    request->done = lume_work_drain_on(request->domain, 1);
}

static unsigned drain_serialized(unsigned domain)
{
    struct drain_request request = {.domain = domain};
    lume_worker_scheduler(drain_callback, &request);
    return request.done;
}

static void parallel_callback(void *context)
{
    struct work_case *test = context;
    assert(__atomic_load_n(&references, __ATOMIC_ACQUIRE));
    ++test->calls;
    if (test->resubmit)
    {
        test->resubmit = false;
        assert(!lume_module_work_submit(&test->work));
    }
}

static void host_yield(void)
{
#ifdef _WIN32
    Sleep(0);
#else
    sched_yield();
#endif
}

#ifdef _WIN32
static DWORD WINAPI parallel_thread(void *context)
#else
static void *parallel_thread(void *context)
#endif
{
    struct work_case *test = context;
    __atomic_fetch_add(&ready_threads, 1, __ATOMIC_RELEASE);
    while (__atomic_load_n(&ready_threads, __ATOMIC_ACQUIRE) < 4)
        host_yield();
    unsigned expected = 0;
    for (unsigned round = 0; round < 2000; ++round)
    {
        test->resubmit = (round % 7) == 0;
        assert(!lume_module_work_submit(&test->work));
        if (round % 2)
        {
            assert(lume_module_work_cancel(&test->work));
            assert(!drain_serialized(test->work.domain));
        }
        else
        {
            assert(drain_serialized(test->work.domain) == 1);
            ++expected;
            if (!(round % 7))
            {
                assert(drain_serialized(test->work.domain) == 1);
                ++expected;
            }
        }
        assert(!lume_work_queued(&test->work));
        if (!(round % 16))
            host_yield();
    }
    assert(test->calls == expected);
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

int main(void)
{
#ifdef _WIN32
    InitializeCriticalSection(&scheduler_lock);
#endif
    assert(!module_services_serial());
    assert(!references);
    struct work_case cases[4] = {0};
#ifdef _WIN32
    HANDLE threads[4];
#else
    pthread_t threads[4];
#endif
    for (unsigned i = 0; i < 4; ++i)
    {
        cases[i].work = (struct lume_work){
            .run = parallel_callback, .context = cases + i, .domain = LUME_WORK_STORAGE + i};
#ifdef _WIN32
        threads[i] = CreateThread(NULL, 0, parallel_thread, cases + i, 0, NULL);
        assert(threads[i]);
#else
        assert(!pthread_create(threads + i, NULL, parallel_thread, cases + i));
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
    assert(!__atomic_load_n(&references, __ATOMIC_ACQUIRE) && !lume_work_pending());
    puts("MODULE_WORK_PARALLEL_PASS threads=4 submissions=8000 cancel,resubmit references=0");
    return 0;
}
