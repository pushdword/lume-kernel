#include "lume/cpu.h"
#include "lume/board.h"
#include "lume/early.h"
#include "lume/mmio.h"
#include "lume/wpa.h"
#define RNG LUME_BOARD_RNG200_BASE

static uint32_t rd(unsigned off)
{
    return lume_mmio_read32(RNG + off);
}

static void wr(unsigned off, uint32_t v)
{
    lume_mmio_write32_relaxed(RNG + off, v);
    lume_dma_write_complete();
}

bool lume_entropy(void *out, size_t bytes)
{
    if (!out || !bytes || bytes > 64)
        return false;
    if (!(rd(0) & 0x1fff))
    {
        wr(0x10, 0x40000);
        wr(0x24, 2u << 8);
        wr(0, (3u << 13) | 0x1fff);
    }
    uint32_t start = lume_clock_millis();
    size_t used = 0;
    unsigned char *p = out;
    while (used < bytes && (uint32_t)(lume_clock_millis() - start) < 1000)
    {
        if (rd(0x18) & 0x80000020u)
        {
            lume_secret_clear(out, bytes);
            return false;
        }
        if (rd(0x0c) <= 16 || !(rd(0x24) & 255))
            continue;
        uint32_t word = rd(0x20);
        for (unsigned i = 0; i < 4 && used < bytes; ++i)
            p[used++] = (unsigned char)(word >> (i * 8));
    }
    if (used != bytes)
    {
        lume_secret_clear(out, bytes);
        return false;
    }
    return true;
}
