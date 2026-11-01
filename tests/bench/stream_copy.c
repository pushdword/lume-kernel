/* SPDX-License-Identifier: BSD-3-Clause */
#define _POSIX_C_SOURCE 200809L
#define main stream_contract_main
#include "../unit/stream_test.c"
#undef main
#include "clock.h"
#include <stdlib.h>

extern int lume_stream_nowait(unsigned, unsigned, struct lume_stream_request *);

int main(int argc, char **argv)
{
    unsigned iterations = argc == 2 ? (unsigned)strtoul(argv[1], NULL, 10) : 20000;
    if (!iterations || iterations > 100000)
        return 2;
    stream_contract_main();
    lume_stream_release(2);
    notifications = 0;
    struct lume_stream_request request = {
        .version = LUME_IO_VERSION, .port = 8095, .address = {172, 30, 1, 163}};
    assert(!lume_stream_call(2, LUME_STREAM_CONNECT, &request));
    unsigned char data[6000];
    for (unsigned i = 0; i < sizeof(data); ++i)
        data[i] = (unsigned char)(i * 13u + 7u);
    struct pbuf packet = {.payload = data, .len = sizeof(data), .tot_len = sizeof(data)};
    uint64_t start = bench_clock();
    unsigned checksum = 0;
    for (unsigned i = 0; i < iterations; ++i)
    {
        notifications = 0;
        assert(pcb.recv(pcb.callback_arg, &pcb, &packet, ERR_OK) == ERR_OK);
        request.count = 4096;
        assert(lume_stream_nowait(2, LUME_STREAM_RECEIVE, &request) == 4096);
        checksum += request.data[i % 4096];
        if (!i || i + 1 == iterations)
            assert(!memcmp(request.data, data, 4096));
        request.count = 4096;
        assert(lume_stream_nowait(2, LUME_STREAM_RECEIVE, &request) == sizeof(data) - 4096);
        checksum += request.data[i % (sizeof(data) - 4096)];
        if (!i || i + 1 == iterations)
            assert(!memcmp(request.data, data + 4096, sizeof(data) - 4096));
    }
    uint64_t elapsed = bench_clock() - start;
    printf("{\"bytes\":%llu,\"nanoseconds\":%llu,\"checksum\":%u}\n",
           (unsigned long long)iterations * sizeof(data), (unsigned long long)elapsed, checksum);
    return 0;
}
