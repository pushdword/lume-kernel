#include "lume/wifi_protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks;
#define CHECK(x)                                                                                   \
    do                                                                                             \
    {                                                                                              \
        ++checks;                                                                                  \
        if (!(x))                                                                                  \
        {                                                                                          \
            fprintf(stderr, "wireless network check %u line %u\n", checks, __LINE__);              \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

static void le32(unsigned char *p, unsigned n)
{
    for (unsigned i = 0; i < 4; ++i)
        p[i] = (unsigned char)(n >> (8 * i));
}

static void be32(unsigned char *p, unsigned n)
{
    for (unsigned i = 0; i < 4; ++i)
        p[3 - i] = (unsigned char)(n >> (8 * i));
}

int main(void)
{
    unsigned char event[256] = {0};
    event[12] = 0x88;
    event[13] = 0x6c;
    event[14] = 0x80;
    event[15] = 1;
    event[20] = 0x10;
    event[21] = 0x18;
    event[23] = 1;
    event[25] = 2;
    be32(event + 28, 69);
    be32(event + 32, 8);
    be32(event + 44, 12);
    struct lume_wifi_event e;
    for (size_t n = 0; n < 84; ++n)
        CHECK(!lume_wifi_event_parse(event, n, &e));
    CHECK(lume_wifi_event_parse(event, 84, &e) && e.type == 69 && e.status == 8 && e.bytes == 12 &&
          e.data == event + 72);
    const unsigned offsets[] = {12, 13, 14, 15, 18, 19, 20, 21, 22, 23, 24, 25, 70, 71};
    for (unsigned i = 0; i < sizeof(offsets) / sizeof(offsets[0]); ++i)
    {
        event[offsets[i]] ^= 1;
        CHECK(!lume_wifi_event_parse(event, sizeof(event), &e));
        event[offsets[i]] ^= 1;
    }
    be32(event + 44, UINT32_MAX);
    CHECK(!lume_wifi_event_parse(event, sizeof(event), &e));
    unsigned char bss[160] = {0};
    le32(bss, 109);
    le32(bss + 4, 132);
    bss[18] = 4;
    memcpy(bss + 19, "test", 4);
    bss[16] = 16;
    bss[72] = 36;
    bss[78] = 0xd4;
    bss[79] = 0xff;
    bss[88] = 36;
    bss[116] = 128;
    le32(bss + 120, 4);
    bss[128] = 48;
    bss[129] = 2;
    bss[130] = 1;
    struct lume_access_point ap;
    size_t used;
    for (size_t n = 0; n < 132; ++n)
        CHECK(!lume_wifi_bss(bss, n, &ap, &used));
    CHECK(lume_wifi_bss(bss, 132, &ap, &used) && used == 132 && !strcmp(ap.ssid, "test") &&
          ap.rssi == -44 && ap.channel == 36 && ap.security == 3);
    bss[18] = 33;
    CHECK(!lume_wifi_bss(bss, 132, &ap, &used));
    bss[18] = 4;
    bss[129] = 5;
    CHECK(!lume_wifi_bss(bss, 132, &ap, &used));
    bss[129] = 2;
    bss[116] = 127;
    le32(bss + 120, UINT32_MAX);
    CHECK(!lume_wifi_bss(bss, 132, &ap, &used));
    unsigned char ethernet[1514], wire[1536];
    for (unsigned i = 0; i < sizeof(ethernet); ++i)
        ethernet[i] = (unsigned char)i;
    for (size_t n = 14; n <= 1514; ++n)
    {
        size_t count = lume_wifi_data_frame(wire, sizeof(wire), 255, ethernet, n);
        CHECK(count == ((n + 21) & ~(size_t)3));
        CHECK((wire[0] | (unsigned)wire[1] << 8) == n + 18 && wire[4] == 255 && wire[5] == 2 &&
              wire[7] == 14 && wire[14] == 0x20 && wire[12] == 0 && wire[13] == 0);
        CHECK(!memcmp(wire + 18, ethernet, n));
        CHECK(!lume_wifi_data_frame(wire, count - 1, 1, ethernet, n));
    }
    CHECK(!lume_wifi_data_frame(wire, sizeof(wire), 0, ethernet, 13));
    CHECK(!lume_wifi_data_frame(wire, sizeof(wire), 0, ethernet, 1515));
    printf("PASS WIFI NETWORK DECODERS %u checks\n", checks);
    return 0;
}
