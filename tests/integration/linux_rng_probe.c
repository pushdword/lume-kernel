/* SPDX-License-Identifier: GPL-2.0-only */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "lume/abi.h"
#if defined(LUME_LINUX_DMA_REBIND)
#include "lume/hardware_abi.h"
#endif

static int64_t call(uint64_t number, uint64_t a, uint64_t b, uint64_t c)
{
#if defined(__x86_64__)
    register uint64_t r8 __asm__("r8") = number;
    __asm__ volatile("int $0x80" : "+D"(a) : "S"(b), "d"(c), "r"(r8) : "memory", "cc");
    return (int64_t)a;
#else
    const uint64_t request[4] = {number, a, b, c};
    uint32_t lo = (uintptr_t)request, hi;
    __asm__ volatile("int $0x80" : "+a"(lo), "=d"(hi) : : "memory", "cc");
    return (int64_t)((uint64_t)hi << 32 | lo);
#endif
}

static void message(const char *text)
{
    size_t bytes = 0;
    while (text[bytes])
        ++bytes;
    (void)call(LUME_SYS_WRITE, 1, (uintptr_t)text, bytes);
}

static bool check_reads(void)
{
    static unsigned char buffer[258], previous[256];
    static const unsigned lengths[] = {1, 3, 31, 32, 63, 64, 129, 256};
    unsigned different = 0;
    for (unsigned trial = 0; trial < 128; ++trial)
    {
        unsigned bytes = lengths[trial % (sizeof(lengths) / sizeof(lengths[0]))];
        for (unsigned i = 0; i < sizeof(buffer); ++i)
            buffer[i] = 0xa5;
        if (call(LUME_SYS_RANDOM, (uintptr_t)(buffer + 1), bytes, 0) != bytes ||
            buffer[0] != 0xa5 || buffer[bytes + 1] != 0xa5)
            return false;
        for (unsigned i = 0; i < bytes; ++i)
        {
            different |= buffer[i + 1] ^ previous[i];
            previous[i] = buffer[i + 1];
        }
    }
    return different && call(LUME_SYS_RANDOM, (uintptr_t)buffer, 257, 0) == -LUME_EINVAL;
}

#if defined(LUME_LINUX_DMA_REBIND)
static bool same_name(const char *first, const char *second)
{
    while (*first && *first == *second)
    {
        ++first;
        ++second;
    }
    return *first == *second;
}

static int64_t hardware_call(struct lume_hardware_request *request, unsigned operation)
{
    request->operation = operation;
    return call(LUME_SYS_HARDWARE, (uintptr_t)request, sizeof(*request), 0);
}

static bool rebind(void)
{
    static struct lume_hardware_request request = {.version = LUME_HARDWARE_VERSION};
    const char *driver = NULL;
    for (; !hardware_call(&request, LUME_HARDWARE_LIST); ++request.index)
    {
        if (request.info.state != LUME_HARDWARE_READY)
            continue;
        if (same_name(request.info.driver, "q35-virtio-rng"))
            driver = "q35-virtio-rng";
        else if (same_name(request.info.driver, "linux-virtio-rng"))
            driver = "linux-virtio-rng";
        if (driver)
            break;
    }
    if (!driver)
        return false;
    request.id = request.info.id;
    request.index = 0;
    if (hardware_call(&request, LUME_HARDWARE_UNBIND))
        return false;
    unsigned i = 0;
    do
        request.driver[i] = driver[i];
    while (driver[i++]);
    return !hardware_call(&request, LUME_HARDWARE_BIND);
}
#endif

__attribute__((section(".text.start"), noreturn)) void _start(void)
{
    if (!check_reads())
        goto failed;
#if defined(LUME_LINUX_DMA_REBIND)
    if (!rebind() || !check_reads())
        goto failed;
    message("LINUX_RNG_DMA_REBIND_PASS\n");
    message("LINUX_RNG_GUEST_PASS reads=256 sizes=1,3,31,32,63,64,129,256 guards=PASS\n");
#else
    message("LINUX_RNG_GUEST_PASS reads=128 sizes=1,3,31,32,63,64,129,256 guards=PASS\n");
#endif
    (void)call(LUME_SYS_SLEEP, 20, 0, 0);
    (void)call(LUME_SYS_EXIT, 0, 0, 0);
    for (;;)
    {
    }
failed:
    message("LINUX_RNG_GUEST_FAIL\n");
    (void)call(LUME_SYS_EXIT, 1, 0, 0);
    for (;;)
    {
    }
}
