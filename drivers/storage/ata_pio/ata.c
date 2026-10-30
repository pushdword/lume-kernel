#include "lume/q35.h"
#include "lume/counter.h"
#include "lume/cpu.h"
#include "lume/platform_driver.h"
#include "lume/storage.h"
#include "lume/sd.h"
#include "lume/worker.h"
#define DATA 0x1f0
#define CONTROL 0x3f6
static uint64_t capacity, allowed_first, allowed_count;
static uint64_t reads, writes, failures;
static bool failed;
static bool flush_supported;

static void delay(void)
{
    for (unsigned i = 0; i < 4; ++i)
        (void)lume_in8(CONTROL);
}

static bool wait_status(bool data)
{
    uint64_t start = lume_counter_read_relaxed();
    for (unsigned i = 0; i < 10000000; ++i)
    {
        uint8_t status = lume_in8(CONTROL);
        if (!status || status == 0xff)
            break;
        if (!(status & 0x80))
        {
            if (status & 0x21)
                break;
            if (data ? (status & 8) != 0 : (status & 8) == 0)
                return true;
        }
        if (lume_counter_read_relaxed() - start >= 2000000)
            break;
        lume_cpu_relax();
    }
    failed = true;
    ++failures;
    return false;
}

static bool select_sector(uint64_t sector, uint8_t command)
{
    if (failed || sector >= capacity || sector >= UINT64_C(0x10000000) || !wait_status(false))
        return false;
    lume_out8(DATA + 6, (uint8_t)(0xe0 | (sector >> 24)));
    delay();
    lume_out8(DATA + 2, 1);
    lume_out8(DATA + 3, (uint8_t)sector);
    lume_out8(DATA + 4, (uint8_t)(sector >> 8));
    lume_out8(DATA + 5, (uint8_t)(sector >> 16));
    lume_out8(DATA + 7, command);
    delay();
    return wait_status(true);
}

static bool read_sector(uint64_t sector, void *buffer)
{
    if (!buffer || !select_sector(sector, 0x20))
        return false;
    unsigned char *out = buffer;
    for (unsigned i = 0; i < 256; ++i)
    {
        uint16_t value = lume_in16(DATA);
        out[i * 2] = (unsigned char)value;
        out[i * 2 + 1] = (unsigned char)(value >> 8);
    }
    delay();
    if (!wait_status(false))
        return false;
    ++reads;
    return true;
}

static bool write_sector(uint64_t sector, const void *buffer)
{
    if (!buffer || !allowed_count || sector < allowed_first ||
        sector - allowed_first >= allowed_count || !select_sector(sector, 0x30))
        return false;
    const unsigned char *in = buffer;
    for (unsigned i = 0; i < 256; ++i)
        lume_out16(DATA, (uint16_t)in[i * 2] | (uint16_t)in[i * 2 + 1] << 8);
    delay();
    if (!wait_status(false))
        return false;
    if (flush_supported)
    {
        lume_out8(DATA + 7, 0xe7);
        delay();
        if (!wait_status(false))
            return false;
    }
    ++writes;
    return true;
}

static bool read_blocks(uint64_t sector, unsigned count, void *buffer)
{
    if (!buffer || !count || count > LUME_SD_MAX_SECTORS || sector >= capacity ||
        count > capacity - sector)
        return false;
    for (unsigned i = 0; i < count; ++i)
    {
        if (!read_sector(sector + i, (unsigned char *)buffer + (size_t)i * 512))
            return false;
        lume_worker_checkpoint(512);
    }
    return true;
}

static bool write_blocks(uint64_t sector, unsigned count, const void *buffer)
{
    if (!buffer || !count || count > LUME_SD_MAX_SECTORS || sector >= capacity ||
        count > capacity - sector || !allowed_count || sector < allowed_first ||
        sector - allowed_first >= allowed_count || count > allowed_count - (sector - allowed_first))
        return false;
    for (unsigned i = 0; i < count; ++i)
    {
        if (!write_sector(sector + i, (const unsigned char *)buffer + (size_t)i * 512))
            return false;
        lume_worker_checkpoint(512);
    }
    return true;
}

static bool start(uint64_t frequency)
{
    (void)frequency;
    return capacity && !failed;
}

static bool configure(uint64_t base, uint64_t size, uint32_t irq)
{
    (void)base;
    (void)size;
    (void)irq;
    return false;
}

static bool stop(void)
{
    return !failed && wait_status(false);
}

static uint64_t sectors(void)
{
    return capacity;
}

static bool allow(uint64_t first, uint64_t count)
{
    if (failed || allowed_count || first != LUME_PC_DATA_FIRST || first >= capacity || !count ||
        count != capacity - first)
        return false;
    allowed_first = first;
    allowed_count = count;
    return true;
}

static bool allow_boot(uint64_t first, uint64_t count)
{
    (void)first;
    (void)count;
    return false;
}

static void stats(void)
{
    lume_value("ATA_PIO_SECTORS", capacity);
    lume_value("ATA_PIO_READS", reads);
    lume_value("ATA_PIO_WRITES", writes);
    lume_value("ATA_PIO_FAILURES", failures);
}

int lume_pc_ide_register(void)
{
    lume_out8(CONTROL, 2);
    lume_out8(DATA + 6, 0xa0);
    delay();
    lume_out8(DATA + 2, 0);
    lume_out8(DATA + 3, 0);
    lume_out8(DATA + 4, 0);
    lume_out8(DATA + 5, 0);
    lume_out8(DATA + 7, 0xec);
    delay();
    if (!wait_status(true) || lume_in8(DATA + 4) || lume_in8(DATA + 5))
        return -5;
    uint16_t identify[256];
    for (unsigned i = 0; i < 256; ++i)
        identify[i] = lume_in16(DATA);
    delay();
    if (!wait_status(false) || !(identify[49] & 0x200))
        return -5;
    flush_supported = (identify[83] & 0xc000) == 0x4000 && (identify[83] & (1u << 12));
    if (!flush_supported && (identify[82] & (1u << 5)))
        return -5;
    capacity = (uint64_t)identify[60] | (uint64_t)identify[61] << 16;
    if (!capacity || capacity > UINT64_C(0x10000000))
        return -5;
    static const struct lume_platform_sd_ops operations = {
        start, configure,  stop,  read_sector, write_sector, sectors,
        allow, allow_boot, stats, read_blocks, write_blocks};
    int result = lume_platform_driver_register(LUME_PLATFORM_SD, &operations, sizeof(operations));
    if (!result)
        stats();
    return result;
}
