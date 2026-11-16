#define main hardware_fixture_main
#include "hardware_test.c"
#undef main
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

static uint32_t concurrent_id;
static unsigned ready, stopped, attempts, admitted;

#ifdef _WIN32
static DWORD WINAPI reference_thread(void *unused)
#else
static void *reference_thread(void *unused)
#endif
{
    (void)unused;
    __atomic_fetch_add(&ready, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&stopped, __ATOMIC_ACQUIRE))
    {
        int result = lume_hardware_reference(concurrent_id);
        __atomic_fetch_add(&attempts, 1, __ATOMIC_RELAXED);
        assert(!result || result == -LUME_EBUSY);
        if (!result)
        {
            __atomic_fetch_add(&admitted, 1, __ATOMIC_RELAXED);
            assert(!lume_hardware_unreference(concurrent_id));
        }
    }
    return 0;
}

int main(void)
{
    hardware_fixture_main();
    struct lume_hardware_description description = {.origin = LUME_HARDWARE_VIRTUAL,
                                                    .state = LUME_HARDWARE_READY,
                                                    .name = "reference",
                                                    .path = "/reference"};
    assert(!lume_hardware_add(&description, &concurrent_id));
    assert(!lume_hardware_reference(concurrent_id));
    assert(!lume_hardware_state(concurrent_id, LUME_HARDWARE_ABSENT, -LUME_ENOENT));
    assert(lume_hardware_reference(concurrent_id) == -LUME_EBUSY);
    assert(lume_hardware_remove(concurrent_id) == -LUME_EBUSY);
    assert(!lume_hardware_unreference(concurrent_id));
    assert(!lume_hardware_state(concurrent_id, LUME_HARDWARE_READY, 0));
#ifdef _WIN32
    HANDLE threads[4];
    for (unsigned i = 0; i < 4; ++i)
    {
        threads[i] = CreateThread(NULL, 0, reference_thread, NULL, 0, NULL);
        assert(threads[i]);
    }
#else
    pthread_t threads[4];
    for (unsigned i = 0; i < 4; ++i)
        assert(!pthread_create(threads + i, NULL, reference_thread, NULL));
#endif
    while (__atomic_load_n(&ready, __ATOMIC_ACQUIRE) != 4)
    {
    }
    for (unsigned i = 0; i < 12000; ++i)
    {
        assert(!lume_hardware_state(concurrent_id, LUME_HARDWARE_ABSENT, -LUME_ENOENT));
        assert(!lume_hardware_state(concurrent_id, LUME_HARDWARE_READY, 0));
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
    struct lume_hardware_info info;
    assert(!lume_hardware_get(concurrent_id, &info) && !info.references);
    assert(attempts && admitted);
    uint32_t previous = concurrent_id;
    assert(!lume_hardware_remove(previous));
    assert(!lume_hardware_add(&description, &concurrent_id) && concurrent_id != previous);
    assert(lume_hardware_reference(previous) == -LUME_ENOENT);
    assert(!lume_hardware_reference(concurrent_id));
    assert(!lume_hardware_unreference(concurrent_id));
    assert(!lume_hardware_remove(concurrent_id));
    printf("HARDWARE_PARALLEL_PASS threads=4 attempts=%u admitted=%u transitions=24000 refs=0\n",
           attempts, admitted);
    return 0;
}
