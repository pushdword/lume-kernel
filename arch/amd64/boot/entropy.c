/* SPDX-License-Identifier: BSD-3-Clause */
#include "lume/cpu.h"
#include "lume/platform_driver.h"
#include <string.h>

static bool entropy(void *out, size_t bytes)
{
    if (!out || !bytes || bytes > 256)
        return false;
    unsigned char *next = out;
    for (size_t left = bytes; left;)
    {
        uint64_t word;
        bool ready = false;
        for (unsigned retry = 0; retry < 10; ++retry)
        {
            __asm__ volatile("rdrand %0" : "=r"(word), "=@ccc"(ready));
            if (ready)
                break;
            lume_cpu_relax();
        }
        if (!ready)
        {
            memset(out, 0, bytes);
            return false;
        }
        size_t count = left < sizeof(word) ? left : sizeof(word);
        memcpy(next, &word, count);
        next += count;
        left -= count;
    }
    return true;
}

bool lume_amd64_entropy_start(void)
{
    uint32_t signature, features, ebx, edx;
    __asm__ volatile("cpuid"
                     : "=a"(signature), "=b"(ebx), "=c"(features), "=d"(edx)
                     : "0"(1), "2"(0));
    if (!(features & (1u << 30)))
        return false;
    static const struct lume_platform_entropy_ops operations = {entropy};
    uint64_t sample[2];
    if (!entropy(sample, sizeof(sample)) || sample[0] == sample[1])
        return false;
    return !lume_platform_driver_register(LUME_PLATFORM_ENTROPY, &operations, sizeof(operations));
}
