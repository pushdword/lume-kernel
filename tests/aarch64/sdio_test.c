#include "lume/sdio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks, calls, fail_at, fail_r5, reads53, delays;
static bool ocr_stuck, function_stuck, alp_stuck, corrupt_chip;
static unsigned char regs[2][0x20000];
#define CHECK(x)                                                                                   \
    do                                                                                             \
    {                                                                                              \
        ++checks;                                                                                  \
        if (!(x))                                                                                  \
        {                                                                                          \
            fprintf(stderr, "SDIO FAIL line %d: %s\n", __LINE__, #x);                              \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

static bool cmd(unsigned index, uint32_t argument, uint32_t flags, uint32_t *response)
{
    ++calls;
    if (calls == fail_at)
        return false;
    if (index == 0)
    {
        CHECK(!argument && !flags);
        return true;
    }
    CHECK(response != NULL);
    *response = 0;
    if (index == 5)
    {
        CHECK(flags == LUME_SDIO_RESPONSE && (argument == 0 || argument == 0x300000));
        *response = 0x30ff8000u | ((argument && !ocr_stuck) ? 0x80000000u : 0);
        return true;
    }
    if (index == 3)
    {
        CHECK(!argument);
        *response = 0x00010000;
        return true;
    }
    if (index == 7)
    {
        CHECK(argument == 0x10000);
        return true;
    }
    CHECK(index == 52 && flags == (LUME_SDIO_RESPONSE | LUME_SDIO_CHECKS));
    unsigned fn = (argument >> 28) & 7, addr = (argument >> 9) & 0x1ffff;
    CHECK(fn < 2 && !(argument & (1u << 27)) && !(argument & (1u << 8)));
    if (calls == fail_r5)
    {
        *response = 0x800;
        return true;
    }
    if (argument >> 31)
    {
        regs[fn][addr] = (unsigned char)argument;
        if (!fn && addr == 2 && !function_stuck)
            regs[0][3] = regs[0][2];
        if (fn == 1 && addr == 0x1000e && !alp_stuck)
            regs[1][addr] |= 0x40;
    }
    else
        *response = regs[fn][addr];
    return true;
}

static bool read53(uint32_t argument, unsigned char *data, size_t n)
{
    ++calls;
    ++reads53;
    if (calls == fail_at)
        return false;
    CHECK(n == 4 && argument == 0x15000004);
    CHECK(regs[1][0x1000a] == 0 && regs[1][0x1000b] == 0 && regs[1][0x1000c] == 0x18);
    const unsigned char signature[] = {0x45, 0x43, 0x26, 0x15};
    memcpy(data, signature, 4);
    if (corrupt_chip)
        data[0] = 0;
    return true;
}

static void delay_us(unsigned n)
{
    CHECK(n == 1000 || n == 10000);
    ++delays;
}
static const struct lume_sdio_bus bus = {.command = cmd, .read53 = read53, .delay_us = delay_us};

static void reset(void)
{
    memset(regs, 0, sizeof(regs));
    calls = fail_at = fail_r5 = reads53 = delays = 0;
    ocr_stuck = function_stuck = alp_stuck = corrupt_chip = false;
    regs[0][0] = 0x32;
    regs[0][1] = 3;
    regs[0][8] = 2;
    regs[0][10] = 0x10;
    const unsigned char cis[] = {0, 0x15, 3, 'a', 'b', 0, 0x20, 4, 0xd0, 2, 0x45, 0x43, 0xff};
    memcpy(regs[0] + 0x1000, cis, sizeof(cis));
    regs[0][0x300] = 2;
}

int main(void)
{
    struct lume_sdio_identity id;
    unsigned char byte = 0;
    reset();
    CHECK(lume_sdio_identify(&bus, &id));
    unsigned successful_calls = calls;
    CHECK(id.vendor == 0x02d0 && id.device == 0x4345 && id.signature == 0x15264345);
    CHECK(id.functions == 3 && id.interface[2] == 2 && id.rca == 0x10000 && id.clock_csr == 0x68);
    CHECK(reads53 == 1 && !delays && regs[0][0x110] == 64);
    /* Each transport failure must abort, including the three window writes. */
    for (unsigned n = 1; n <= successful_calls; ++n)
    {
        reset();
        fail_at = n;
        CHECK(!lume_sdio_identify(&bus, &id));
        CHECK(calls == n);
    }
    for (unsigned n = 6; n < successful_calls; ++n)
    {
        reset();
        fail_r5 = n;
        CHECK(!lume_sdio_identify(&bus, &id));
        CHECK(!reads53);
    }
    const unsigned bad_manufacturer[][2] = {{6, 0x21}, {7, 3}, {8, 0}, {10, 0}};
    for (unsigned n = 0; n < 4; ++n)
    {
        reset();
        regs[0][0x1000 + bad_manufacturer[n][0]] = (unsigned char)bad_manufacturer[n][1];
        CHECK(!lume_sdio_identify(&bus, &id));
        CHECK(!regs[0][2] && !reads53);
    }
    reset();
    regs[0][9] = 0xff;
    regs[0][10] = 0xff;
    regs[0][11] = 1;
    regs[0][0x1ffff] = 0x20;
    CHECK(!lume_sdio_identify(&bus, &id));
    reset();
    memset(regs[0] + 0x1000, 0, 1024);
    CHECK(!lume_sdio_identify(&bus, &id));
    CHECK(calls < 100);
    reset();
    regs[0][0x1007] = 0xff;
    CHECK(!lume_sdio_identify(&bus, &id));
    reset();
    ocr_stuck = true;
    CHECK(!lume_sdio_identify(&bus, &id));
    CHECK(delays == 200);
    reset();
    function_stuck = true;
    CHECK(!lume_sdio_identify(&bus, &id));
    CHECK(delays == 100 && !reads53);
    reset();
    alp_stuck = true;
    CHECK(!lume_sdio_identify(&bus, &id));
    CHECK(delays == 100 && !reads53);
    reset();
    corrupt_chip = true;
    CHECK(!lume_sdio_identify(&bus, &id));
    CHECK(reads53 == 1);
    reset();
    CHECK(!lume_sdio_direct(&bus, false, 8, 0, &byte));
    CHECK(!lume_sdio_direct(&bus, true, 0, 0x20000, &byte));
    CHECK(!lume_sdio_direct(&bus, false, 0, 0, NULL));
    CHECK(calls == 0);
    uint32_t value;
    CHECK(!lume_sdio_backplane_read(&bus, 0x18000001, &value));
    CHECK(!lume_sdio_identify(NULL, &id));
    CHECK(calls == 0);
    printf("PASS_SDIO_PROTOCOL checks=%u failure_positions=%u\n", checks, successful_calls);
}
