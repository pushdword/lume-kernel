#include "api.h"
#include <fd.h>
#include <stdint.h>
#include <stdio.h>

static _Alignas(16) unsigned char source[4112], destination[4112];

static struct result
{
    unsigned offset, bytes, rounds, ms, check;
} results[4];

static unsigned now(void)
{
    return (unsigned)lume_syscall(LUME_INFO, LUME_UPTIME, 0, 0);
}

int main(void)
{
    for (unsigned i = 0; i < sizeof source; ++i)
        source[i] = (unsigned char)(i * 29u + (i >> 3));
    for (unsigned sample = 0; sample < 3; ++sample)
    {
        unsigned offset = sample == 2 ? 3 : sample, bytes = 4096u - offset;
        struct lume_fd_request request = {LUME_FD_VERSION, sizeof request, -1, -1, 0, 0, 0, 0};
        if (lume_syscall(LUME_FD, LUME_FD_PIPE, (uint32_t)(uintptr_t)&request, sizeof request) < 0)
            return 1;
        unsigned start = now(), check = 0;
        for (unsigned round = 0; round < 128; ++round)
        {
            if (app_write(request.other, source + offset, bytes) != (int)bytes ||
                app_read(request.fd, destination + offset, bytes) != (int)bytes)
                return 2;
            check += destination[offset + (round * 31u) % bytes];
        }
        unsigned elapsed = now() - start;
        for (unsigned i = 0; i < bytes; ++i)
            if (destination[offset + i] != source[offset + i])
                return 3;
        if (app_close(request.fd) < 0 || app_close(request.other) < 0)
            return 4;
        results[sample] = (struct result){offset, bytes, 128, elapsed, check};
    }
    unsigned old = (unsigned)lume_syscall(LUME_BRK, 0, 0, 0);
    unsigned first = (old + 4095u) & ~4095u, end = first + 1024u * 1024u;
    unsigned start = now(), check = 0;
    if ((int)old < 0 || (unsigned)lume_syscall(LUME_BRK, end, 0, 0) != end)
        return 5;
    for (uintptr_t address = first; address < end; address += 4096)
    {
        volatile unsigned char *page = (volatile unsigned char *)address;
        if (page[1] || page[4095])
            return 6;
        page[0] = 0xa5;
        check += page[0];
    }
    results[3] = (struct result){0, 4096, 256, now() - start, check};
    if ((unsigned)lume_syscall(LUME_BRK, old, 0, 0) != old)
        return 7;
    for (unsigned i = 0; i < 4; ++i)
        printf("R32_MEMORY_BENCH kind=%s offset=%u bytes=%u rounds=%u ms=%u check=%u\n",
               i == 3 ? "zero" : "pipe", results[i].offset, results[i].bytes, results[i].rounds,
               results[i].ms, results[i].check);
    puts("R32_MEMORY_BENCH_PASS");
    return 0;
}
