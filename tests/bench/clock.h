/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef LUME_BENCH_CLOCK_H
#define LUME_BENCH_CLOCK_H
#include <stdint.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static uint64_t bench_clock(void)
{
    LARGE_INTEGER count, frequency;
    QueryPerformanceCounter(&count);
    QueryPerformanceFrequency(&frequency);
    return (uint64_t)(count.QuadPart / frequency.QuadPart) * UINT64_C(1000000000) +
           (uint64_t)(count.QuadPart % frequency.QuadPart) * UINT64_C(1000000000) /
               (uint64_t)frequency.QuadPart;
}
#else
#include <time.h>

static uint64_t bench_clock(void)
{
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value))
        return 0;
    return (uint64_t)value.tv_sec * UINT64_C(1000000000) + (uint64_t)value.tv_nsec;
}
#endif
#endif
