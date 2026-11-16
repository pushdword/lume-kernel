#define main vfs_fixture_main
#include "vfs_test.c"
#undef main
#include <stdlib.h>
#ifdef _WIN32
#include <windows.h>
static CRITICAL_SECTION scheduler;
#else
#include <pthread.h>
static pthread_mutex_t scheduler = PTHREAD_MUTEX_INITIALIZER;
#endif

static struct lume_files concurrent_files;
static int concurrent_fd;
static unsigned stopped, readers_ready, borrowed, destroyed;

struct payload
{
    uint64_t marker, identity;
};

static void scheduler_enter(void)
{
#ifdef _WIN32
    EnterCriticalSection(&scheduler);
#else
    assert(!pthread_mutex_lock(&scheduler));
#endif
}

static void scheduler_leave(void)
{
#ifdef _WIN32
    LeaveCriticalSection(&scheduler);
#else
    assert(!pthread_mutex_unlock(&scheduler));
#endif
}

void lume_worker_scheduler(void (*run)(void *), void *context)
{
    scheduler_enter();
    run(context);
    scheduler_leave();
}

static void close_payload(void *context)
{
    struct payload *payload = context;
    assert(payload->marker == UINT64_C(0x4f50454e46494c45));
    payload->marker = 0;
    ++destroyed;
    free(payload);
}

static const struct lume_file_ops payload_ops = {.kind = LUME_NODE_CHAR, .close = close_payload};

#ifdef _WIN32
static DWORD WINAPI borrow_thread(void *unused)
#else
static void *borrow_thread(void *unused)
#endif
{
    (void)unused;
    __atomic_fetch_add(&readers_ready, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&stopped, __ATOMIC_ACQUIRE))
    {
        void *context = NULL;
        int object =
            lume_vfs_borrow(&concurrent_files, (unsigned)concurrent_fd, &payload_ops, &context);
        if (object < 0)
        {
            assert(object == -LUME_EBADF);
            continue;
        }
        struct payload *payload = context;
        uint64_t identity = payload->identity;
        assert(payload->marker == UINT64_C(0x4f50454e46494c45));
        for (unsigned i = 0; i < 32; ++i)
            __atomic_signal_fence(__ATOMIC_SEQ_CST);
        assert(payload->identity == identity && payload->marker == UINT64_C(0x4f50454e46494c45));
        lume_vfs_drop_independent(object);
        __atomic_fetch_add(&borrowed, 1, __ATOMIC_RELAXED);
    }
    return 0;
}

int main(void)
{
#ifdef _WIN32
    InitializeCriticalSection(&scheduler);
    HANDLE threads[4];
#else
    pthread_t threads[4];
#endif
    lume_vfs_init(NULL, 0);
    assert(lume_files_init(&concurrent_files));
    concurrent_fd = 3;
    struct payload *probe = malloc(sizeof(*probe));
    assert(probe);
    *probe = (struct payload){UINT64_C(0x4f50454e46494c45), UINT64_MAX};
    assert(lume_vfs_attach(&concurrent_files, &payload_ops, probe, 0) == 3);
    void *borrowed_context = NULL;
    int lease = lume_vfs_borrow(&concurrent_files, 3, &payload_ops, &borrowed_context);
    assert(lease >= 0 && borrowed_context == probe);
    int64_t result;
    assert(!lume_vfs_metadata_close(&concurrent_files, 3, 1, &result));
    assert(lume_vfs_dup2(&concurrent_files, 3, 4) == 4);
    assert(lume_vfs_metadata_close(&concurrent_files, 3, 1, &result) && !result);
    assert(!lume_vfs_close(&concurrent_files, 4) && !destroyed);
    assert(probe->identity == UINT64_MAX);
    lume_vfs_drop_independent(lease);
    assert(destroyed == 1);
    destroyed = 0;
    for (unsigned i = 0; i < 4; ++i)
    {
#ifdef _WIN32
        threads[i] = CreateThread(NULL, 0, borrow_thread, NULL, 0, NULL);
        assert(threads[i]);
#else
        assert(!pthread_create(threads + i, NULL, borrow_thread, NULL));
#endif
    }
    while (__atomic_load_n(&readers_ready, __ATOMIC_ACQUIRE) != 4)
    {
    }
    for (unsigned i = 0; i < 6000; ++i)
    {
        struct payload *payload = malloc(sizeof(*payload));
        assert(payload);
        *payload = (struct payload){UINT64_C(0x4f50454e46494c45), i};
        scheduler_enter();
        assert(lume_vfs_attach(&concurrent_files, &payload_ops, payload, 0) == concurrent_fd);
        scheduler_leave();
        for (unsigned n = 0; n < 32; ++n)
            __atomic_signal_fence(__ATOMIC_SEQ_CST);
        scheduler_enter();
        assert(!lume_vfs_close(&concurrent_files, (unsigned)concurrent_fd));
        scheduler_leave();
    }
    __atomic_store_n(&stopped, 1, __ATOMIC_RELEASE);
    for (unsigned i = 0; i < 4; ++i)
    {
#ifdef _WIN32
        assert(WaitForSingleObject(threads[i], 10000) == WAIT_OBJECT_0);
        CloseHandle(threads[i]);
#else
        assert(!pthread_join(threads[i], NULL));
#endif
    }
    assert(borrowed && destroyed == 6000);
    lume_files_release(&concurrent_files);
    printf("FD_PARALLEL_PASS threads=4 reuse=6000 borrowed=%u destroyed=6000\n", borrowed);
    return 0;
}
