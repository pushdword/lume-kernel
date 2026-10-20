#include "lume/wifi.h"
#include "lume/sdio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks, calls, fail_at;
#define T(x)                                                                                       \
    do                                                                                             \
    {                                                                                              \
        ++checks;                                                                                  \
        if (!(x))                                                                                  \
        {                                                                                          \
            fprintf(stderr, "WIFI FAIL line %d: %s\n", __LINE__, #x);                              \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
static uint32_t window;
static unsigned char memory[0x20000];

static bool command(unsigned index, uint32_t arg, uint32_t flags, uint32_t *response)
{
    if (++calls == fail_at)
        return false;
    T(index == 52 && flags == (LUME_SDIO_RESPONSE | LUME_SDIO_CHECKS));
    T((arg >> 28) == 9);
    unsigned address = (arg >> 9) & 0x1ffff;
    T(address >= 0x1000a && address <= 0x1000c);
    unsigned shift = (address - 0x1000a + 1) * 8;
    window = (window & ~(255u << shift)) | ((arg & 255) << shift);
    *response = 0;
    return true;
}

static bool transfer(uint32_t arg, unsigned char *p, size_t bytes, bool write)
{
    if (++calls == fail_at)
        return false;
    T(((arg >> 28) & 7) == 1 && (bool)(arg >> 31) == write && (arg & (1u << 26)));
    unsigned n = arg & 511;
    T(bytes == ((arg & (1u << 27)) ? n * 64 : n ? n : 512));
    T((arg & (1u << 27)) || bytes <= 64);
    unsigned address = (arg >> 9) & 0x1ffff;
    T(address & 0x8000);
    uint32_t absolute = window + (address & 0x7fff);
    T(absolute >= 0x18000000);
    size_t offset = absolute - 0x18000000;
    T(offset < sizeof(memory) && bytes <= sizeof(memory) - offset);
    if (write)
        memcpy(memory + offset, p, bytes);
    else
        memcpy(p, memory + offset, bytes);
    return true;
}

static bool read53(uint32_t a, unsigned char *p, size_t n)
{
    return transfer(a, p, n, false);
}

static bool write53(uint32_t a, const unsigned char *p, size_t n)
{
    return transfer(a, (unsigned char *)p, n, true);
}
static const struct lume_sdio_bus bus = {.command = command, .read53 = read53, .write53 = write53};

static void put32(unsigned char *p, uint32_t n)
{
    for (unsigned i = 0; i < 4; ++i)
        p[i] = (unsigned char)(n >> (8 * i));
}

int main(int argc, char **argv)
{
    unsigned char buffer[2048], other[2048], saved[2048];
    T(lume_wifi_crc32(0, "123456789", 9) == 0xcbf43926);
    T(lume_wifi_crc32(lume_wifi_crc32(0, "1234", 4), "56789", 5) == 0xcbf43926);
    for (unsigned i = 0; i < sizeof(buffer); ++i)
        buffer[i] = (unsigned char)(i * 37);
    T(lume_sdio_memory(&bus, true, 0x18007f00, buffer, sizeof(buffer)));
    unsigned operations = calls;
    T(lume_sdio_memory(&bus, false, 0x18007f00, other, sizeof(other)) &&
      !memcmp(buffer, other, sizeof(buffer)));
    for (unsigned n = 1; n <= operations; ++n)
    {
        calls = 0;
        fail_at = n;
        T(!lume_sdio_memory(&bus, true, 0x18007f00, buffer, sizeof(buffer)));
        T(calls == n);
    }
    fail_at = 0;
    calls = 0;
    T(!lume_sdio_memory(&bus, true, 0x18000001, buffer, 4));
    T(!lume_sdio_memory(&bus, true, 0xfffffffc, buffer, 8));
    T(calls == 0);
    T(lume_sdio_memory(&bus, true, 0x18007f2c, buffer, 208));
    T(lume_sdio_memory(&bus, false, 0x18007f2c, other, 208) && !memcmp(buffer, other, 208));
    T(lume_sdio_backplane_write(&bus, 0x18000000, 0xaabbccdd));
    uint32_t word;
    T(lume_sdio_backplane_read(&bus, 0x18000000, &word) && word == 0xaabbccdd);
    const char text[] = "# comment\r\n a=1 \r\nb=2 # trailing\n\n";
    size_t n =
        lume_wifi_nvram((const unsigned char *)text, sizeof(text) - 1, buffer, sizeof(buffer));
    T(n == 12 && !memcmp(buffer, "a=1\0b=2\0\0\0\0\0", 12));
    for (size_t cap = 0; cap < n; ++cap)
        T(!lume_wifi_nvram((const unsigned char *)text, sizeof(text) - 1, buffer, cap));
    T(!lume_wifi_nvram((const unsigned char *)"bad\n", 4, buffer, sizeof(buffer)));
    T(!lume_wifi_nvram((const unsigned char *)"a=\0x\n", 5, buffer, sizeof(buffer)));
    uint32_t erom[] = {0x80001,    0x01000001, 0x18000005, 0x83e01,    0x02004001, 0x18002005,
                       0x181020c5, 0x81201,    0x03004001, 0x18003005, 0x181030c5, 0x82901,
                       0x04004001, 0x18004005, 0x181040c5, 15};
    for (unsigned i = 0; i < sizeof(erom) / 4; ++i)
        put32(buffer + 4 * i, erom[i]);
    n = sizeof(erom);
    memcpy(saved, buffer, n);
    struct lume_wifi_cores c;
    T(lume_wifi_erom(buffer, n, &c) && c.arm == 0x18002000 && c.arm_wrap == 0x18102000 &&
      c.sdio_revision == 4);
    for (size_t cut = 0; cut < n; ++cut)
        T(!lume_wifi_erom(buffer, cut, &c));
    put32(buffer + 2 * 4, 0x20000005);
    T(!lume_wifi_erom(buffer, n, &c));
    memcpy(buffer, saved, n);
    put32(buffer + 6 * 4, 0x18102005);
    T(!lume_wifi_erom(buffer, n, &c));
    memcpy(buffer, saved, n);
    put32(buffer + n - 4, 0);
    T(!lume_wifi_erom(buffer, n, &c));
    n = lume_wifi_control_request(buffer, sizeof(buffer), 255, 0x2345, 262, false, "ver", 4, 32);
    T(n == 60 && buffer[4] == 255 && buffer[7] == 12 && buffer[22] == 0x45 && buffer[23] == 0x23);
    struct lume_wifi_frame h;
    T(lume_wifi_frame_header(buffer, 12, &h) && h.length == 60 && h.offset == 12);
    size_t got;
    int32_t status;
    T(lume_wifi_control_reply(buffer, n, 0x2345, 262, other, sizeof(other), &got, &status) == 1 &&
      got == 32);
    memcpy(saved, buffer, n);
    for (size_t cut = 0; cut < 60; ++cut)
        T(lume_wifi_control_reply(buffer, cut, 0x2345, 262, other, sizeof(other), &got, &status) ==
          -1);
    buffer[2] ^= 1;
    T(lume_wifi_control_reply(buffer, n, 0x2345, 262, other, sizeof(other), &got, &status) == -1);
    memcpy(buffer, saved, n);
    buffer[7] = 61;
    T(!lume_wifi_frame_header(buffer, n, &h));
    memcpy(buffer, saved, n);
    T(lume_wifi_control_reply(buffer, n, 0x2346, 262, other, sizeof(other), &got, &status) == 0);
    T(lume_wifi_control_reply(buffer, n, 0x2345, 263, other, sizeof(other), &got, &status) == -1);
    T(lume_wifi_control_reply(buffer, n, 0x2345, 262, other, 31, &got, &status) == -1);
    buffer[5] = 1;
    T(lume_wifi_control_reply(buffer, n, 0x2345, 262, other, sizeof(other), &got, &status) == 0);
    memcpy(buffer, saved, n);
    buffer[20] = 1;
    put32(buffer + 24, (uint32_t)-23);
    T(lume_wifi_control_reply(buffer, n, 0x2345, 262, other, sizeof(other), &got, &status) == -2 &&
      status == -23);
    T(!lume_wifi_control_request(buffer, 30, 0, 1, 262, false, "ver", 4, 32));
    for (unsigned sequence = 0; sequence < 256; ++sequence)
        for (unsigned delta = 0; delta < 256; ++delta)
        {
            unsigned char credit = (unsigned char)(sequence + delta);
            T(lume_wifi_tx_allowed((unsigned char)sequence, credit, 0, false) ==
              (delta >= 1 && delta <= 64));
            T(!lume_wifi_tx_allowed((unsigned char)sequence, credit, 1, false));
            T(!lume_wifi_tx_allowed((unsigned char)sequence, credit, 128, false));
            T(!lume_wifi_tx_allowed((unsigned char)sequence, credit, 0, true));
        }
    memset(buffer, 0, 12);
    buffer[0] = 12;
    buffer[2] = 243;
    buffer[3] = 255;
    buffer[7] = 12;
    buffer[9] = 4;
    T(lume_wifi_frame_header(buffer, 12, &h));
    T(lume_wifi_control_reply(buffer, 12, 1, 0, other, sizeof(other), &got, &status) == 0 &&
      got == 0);
    T(argc == 2);
    FILE *fixture = fopen(argv[1], "rb");
    T(fixture != NULL);
    T(fread(buffer, 1, 512, fixture) == 512 && fgetc(fixture) == EOF);
    fclose(fixture);
    T(lume_wifi_erom(buffer, 512, &c));
    T(c.common == 0x18000000 && c.arm == 0x18002000 && c.arm_wrap == 0x18102000 &&
      c.d11_wrap == 0x18101000 && c.sdio == 0x18004000 && c.arm_revision == 9 &&
      c.sdio_revision == 21);
    printf("PASS_WIFI_FIRMWARE_FORMATS checks=%u memory_failure_positions=%u\n", checks,
           operations);
}
