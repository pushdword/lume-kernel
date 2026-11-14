#define main poll_wait_contract_main
#include "poll_wait_test.c"
#undef main
#include <stdlib.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif

static uint64_t stamp(void)
{
#ifdef _WIN32
    LARGE_INTEGER value;
    QueryPerformanceCounter(&value);
    return (uint64_t)value.QuadPart;
#else
    struct timespec value;
    assert(!clock_gettime(CLOCK_MONOTONIC, &value));
    return (uint64_t)value.tv_sec * UINT64_C(1000000000) + value.tv_nsec;
#endif
}

int main(int argc, char **argv)
{
    unsigned count = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 10) : 1;
    unsigned timeout = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 10) : 0;
    unsigned iterations = 500000;
    assert(count <= LUME_FD_LIMIT);
#ifdef _WIN32
    LARGE_INTEGER frequency;
    assert(QueryPerformanceFrequency(&frequency));
    uint64_t hz = (uint64_t)frequency.QuadPart;
#else
    uint64_t hz = UINT64_C(1000000000);
#endif
    reset();
    struct lume_pollfd records[LUME_FD_LIMIT];
    for (unsigned i = 0; i < count; ++i)
        records[i] = (struct lume_pollfd){.fd = 3, .events = LUME_POLLIN};
    struct lume_poll_request request = {.version = LUME_POLL_VERSION,
                                        .bytes = sizeof(request),
                                        .buffer = (uintptr_t)records,
                                        .count = count,
                                        .timeout_ms = timeout};
    readiness = timeout ? LUME_POLLIN : 0;
    struct lume_context *(*volatile invoke)(struct lume_context *) = poll_call;
    uint64_t sum = 0;
    for (unsigned i = 0; i < 2000; ++i)
    {
        struct lume_context frame = frame_for(&request);
        assert(invoke(&frame) == &frame);
    }
    uint64_t begin = stamp();
    for (unsigned i = 0; i < iterations; ++i)
    {
        struct lume_context frame = frame_for(&request);
        assert(invoke(&frame) == &frame);
        sum += frame.x[0];
    }
    uint64_t elapsed = stamp() - begin;
    assert(sum == (timeout ? (uint64_t)count * iterations : 0));
    printf("{\"count\":%u,\"timeout\":%u,\"iterations\":%u,\"ns_per_call\":%.3f}\n", count, timeout,
           iterations, (double)elapsed * 1e9 / (double)hz / iterations);
    return 0;
}
