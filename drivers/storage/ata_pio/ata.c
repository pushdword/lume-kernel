#include "lume/q35.h"
#include "lume/counter.h"
#include "lume/cpu.h"
#include "lume/platform_driver.h"
#include "lume/storage.h"
#include "lume/sd.h"
#include "lume/worker.h"
#include "lume/abi.h"
#include "lume/irq.h"
#include "lume/irq_async.h"
#define DATA 0x1f0
#define CONTROL 0x3f6
static uint64_t capacity, allowed_first, allowed_count;
static uint64_t reads, writes, failures;
static bool failed;
static bool flush_supported;
static bool dirty;
static unsigned multiple_sectors = 1;
static volatile uint32_t interrupt_pending;
static bool interrupt_mode;
/* Controller ownership covers PIO, worker suspension and the final cache flush. */
static bool operating;

static void interrupt(unsigned irq, void *context)
{
    (void)irq;
    (void)context;
    (void)lume_in8(DATA + 7);
    __atomic_store_n(&interrupt_pending, 1, __ATOMIC_RELEASE);
}

static void delay(void)
{
    for (unsigned i = 0; i < 4; ++i)
        (void)lume_in8(CONTROL);
}

static bool wait_status(bool data)
{
    uint64_t start = lume_counter_read_relaxed();
    uint64_t frequency = lume_counter_frequency();
    uint64_t timeout = frequency ? frequency * 2 : 2000000;
    uint64_t deadline = start + timeout;
    for (unsigned i = 0; i < 10000000; ++i)
    {
        if (interrupt_mode)
            __atomic_store_n(&interrupt_pending, 0, __ATOMIC_RELEASE);
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
        uint64_t now = lume_counter_read_relaxed();
        if ((int64_t)(now - deadline) >= 0)
            break;
        if (frequency && now - start >= frequency / 40000)
        {
            uint64_t next = now + frequency / 1000 + (frequency % 1000 != 0);
            if ((int64_t)(next - deadline) > 0)
                next = deadline;
            bool suspended = interrupt_mode ? lume_worker_wait_bits(&interrupt_pending, 1, next)
                                            : lume_worker_pause(next);
            if (suspended)
                continue;
        }
        lume_cpu_relax();
    }
    failed = true;
    ++failures;
    return false;
}

static bool select_sectors(uint64_t sector, unsigned count, uint8_t command)
{
    if (failed || !count || count > LUME_SD_MAX_SECTORS || sector >= capacity ||
        count > capacity - sector || sector >= UINT64_C(0x10000000) ||
        count > UINT64_C(0x10000000) - sector || !wait_status(false))
        return false;
    lume_out8(DATA + 6, (uint8_t)(0xe0 | (sector >> 24)));
    delay();
    lume_out8(DATA + 2, (uint8_t)count);
    lume_out8(DATA + 3, (uint8_t)sector);
    lume_out8(DATA + 4, (uint8_t)(sector >> 8));
    lume_out8(DATA + 5, (uint8_t)(sector >> 16));
    lume_out8(DATA + 7, command);
    delay();
    return wait_status(true);
}

static bool read_blocks_owned(uint64_t sector, unsigned count, void *buffer)
{
    if (!buffer || !select_sectors(sector, count, multiple_sectors > 1 ? 0xc4 : 0x20))
        return false;
    unsigned char *out = buffer;
    for (unsigned block = 0; block < count;)
    {
        unsigned part = count - block;
        if (part > multiple_sectors)
            part = multiple_sectors;
        for (unsigned sector = 0; sector < part; ++sector)
        {
            lume_ins16(DATA, out, 256);
            out += 512;
            lume_worker_checkpoint(512);
        }
        block += part;
        delay();
        if (!wait_status(block < count))
            return false;
        reads += part;
    }
    return true;
}

static bool write_blocks_owned(uint64_t sector, unsigned count, const void *buffer)
{
    if (!buffer || !count || count > LUME_SD_MAX_SECTORS || !allowed_count ||
        sector < allowed_first || sector - allowed_first >= allowed_count ||
        count > allowed_count - (sector - allowed_first) ||
        !select_sectors(sector, count, multiple_sectors > 1 ? 0xc5 : 0x30))
        return false;
    const unsigned char *in = buffer;
    dirty = true;
    for (unsigned block = 0; block < count;)
    {
        unsigned part = count - block;
        if (part > multiple_sectors)
            part = multiple_sectors;
        for (unsigned sector = 0; sector < part; ++sector)
        {
            lume_outs16(DATA, in, 256);
            in += 512;
            lume_worker_checkpoint(512);
        }
        block += part;
        delay();
        if (!wait_status(block < count))
            return false;
    }
    writes += count;
    return true;
}

static bool flush_owned(void)
{
    if (failed)
        return false;
    if (!dirty)
        return true;
    if (flush_supported)
    {
        if (!wait_status(false))
            return false;
        lume_out8(DATA + 7, 0xe7);
        delay();
        if (!wait_status(false))
            return false;
    }
    dirty = false;
    return true;
}

static bool read_blocks(uint64_t sector, unsigned count, void *buffer)
{
    if (operating)
        return false;
    operating = true;
    bool result = read_blocks_owned(sector, count, buffer);
    operating = false;
    return result;
}

static bool write_blocks(uint64_t sector, unsigned count, const void *buffer)
{
    if (operating)
        return false;
    operating = true;
    bool result = write_blocks_owned(sector, count, buffer) && flush_owned();
    operating = false;
    return result;
}

static bool write_deferred(uint64_t sector, unsigned count, const void *buffer)
{
    if (operating)
        return false;
    operating = true;
    bool result = write_blocks_owned(sector, count, buffer);
    operating = false;
    return result;
}

static bool sync_device(void)
{
    if (operating)
        return false;
    operating = true;
    bool result = flush_owned();
    operating = false;
    return result;
}

static bool read_sector(uint64_t sector, void *buffer)
{
    return read_blocks(sector, 1, buffer);
}

static bool write_sector(uint64_t sector, const void *buffer)
{
    return write_blocks(sector, 1, buffer);
}

static bool start(uint64_t frequency)
{
    (void)frequency;
    return capacity && !failed && !operating;
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
    if (operating)
        return false;
    operating = true;
    bool result = flush_owned() && wait_status(false);
    operating = false;
    return result;
}

static uint64_t sectors(void)
{
    return capacity;
}

static bool allow(uint64_t first, uint64_t count)
{
    if (operating || failed || allowed_count || first != LUME_PC_DATA_FIRST || first >= capacity ||
        !count || count != capacity - first)
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
    lume_value("ATA_PIO_MULTIPLE_SECTORS", multiple_sectors);
    lume_value("ATA_PIO_IRQ", interrupt_mode);
}

static int register_owned(void)
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
    lume_ins16(DATA, identify, 256);
    delay();
    if (!wait_status(false) || !(identify[49] & 0x200))
        return -5;
    flush_supported = (identify[83] & 0xc000) == 0x4000 && (identify[83] & (1u << 12));
    if (!flush_supported && (identify[82] & (1u << 5)))
        return -5;
    capacity = (uint64_t)identify[60] | (uint64_t)identify[61] << 16;
    if (!capacity || capacity > UINT64_C(0x10000000))
        return -5;
    multiple_sectors = 1;
    unsigned maximum = identify[47] & 0xff;
    unsigned current = identify[59] & 0xff;
    if ((identify[47] >> 8) == 0x80 && (identify[59] & 0x100) && maximum && current &&
        !(maximum & (maximum - 1)) && !(current & (current - 1)) && current <= maximum &&
        current <= LUME_SD_MAX_SECTORS)
        multiple_sectors = current;
    static const struct lume_platform_sd_ops operations = {
        start,      configure, stop,        read_sector,  write_sector,   sectors,    allow,
        allow_boot, stats,     read_blocks, write_blocks, write_deferred, sync_device};
    int result = lume_platform_driver_register(LUME_PLATFORM_SD, &operations, sizeof(operations));
    if (!result)
    {
        if (!interrupt_mode && lume_irq_available() &&
            !lume_irq_register_async(46, LUME_IRQ_EDGE, interrupt, NULL))
        {
            if (!lume_irq_enable(46, true))
                interrupt_mode = true;
            else
                (void)lume_irq_unregister(46, NULL);
        }
        lume_out8(CONTROL, interrupt_mode ? 0 : 2);
        stats();
    }
    return result;
}

int lume_pc_ide_register(void)
{
    if (operating)
        return -LUME_EBUSY;
    operating = true;
    int result = register_owned();
    operating = false;
    return result;
}
