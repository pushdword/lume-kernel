/* The BCM2712 SDHCI driver polls PIO transfers and issues single-sector commands. It does not
 * use DMA or SD Express. Writes start disabled. After volume validation, writes are limited to
 * that Lume volume and cannot address the MBR. */
#include "lume/early.h"
#include "lume/mmio.h"
#include "lume/counter.h"
#include "lume/sd.h"
#include "lume/watchdog.h"
#include "lume/storage.h"
#define HOST UINT64_C(0x1000fff000)
static uint64_t frequency, capacity, write_first, write_count;
static uint64_t boot_first, boot_count;
static uint32_t rca;
static uint64_t reads, writes, errors, rejected;
static bool ready;

static uint32_t rd(unsigned reg)
{
    return lume_mmio_read32_relaxed(HOST + reg);
}

static void wr(unsigned reg, uint32_t value)
{
    lume_mmio_write32_sync(HOST + reg, value);
}

static void delay(unsigned us)
{
    lume_counter_delay_us(frequency, us);
}

static bool wait_register(unsigned reg, uint32_t mask, uint32_t wanted, unsigned ms)
{
    uint64_t start = lume_counter_read();
    do
    {
        if ((rd(reg) & mask) == wanted)
            return true;
    } while (lume_counter_read() - start < frequency * ms / 1000);
    return false;
}

static bool event(uint32_t mask, unsigned ms)
{
    uint64_t start = lume_counter_read();
    uint32_t status;
    do
    {
        status = rd(0x30);
        if (status & 0xffff8000u)
            break;
        if (status & mask)
        {
            wr(0x30, mask);
            return true;
        }
    } while (lume_counter_read() - start < frequency * ms / 1000);
    lume_value("SD_ERROR_INTERRUPT", status);
    lume_value("SD_ERROR_PRESENT", rd(0x24));
    ++errors;
    ready = false;
    return false;
}

#define RESPONSE (2u << 16)
#define CHECKS ((1u << 19) | (1u << 20))

static bool command(unsigned index, uint32_t argument, uint32_t flags, uint32_t *response)
{
    uint32_t inhibit = 1u | ((flags & ((1u << 21) | (1u << 16))) ? 2u : 0u);
    if (!wait_register(0x24, inhibit, 0, 1000))
    {
        lume_value("SD_INHIBIT_CMD", index);
        ++errors;
        ready = false;
        return false;
    }
    wr(0x30, 0xffffffffu);
    wr(8, argument);
    wr(12, (index << 24) | flags);
    if (!event(1, 1000))
    {
        lume_value("SD_ERROR_CMD", index);
        return false;
    }
    if (response)
        *response = rd(0x10);
    if ((flags & (3u << 16)) == (3u << 16) && !event(2, 2000))
        return false;
    return true;
}

static bool app(unsigned index, uint32_t argument, uint32_t flags, uint32_t *response)
{
    uint32_t status;
    return command(55, rca, RESPONSE | CHECKS, &status) && (status & 32) &&
           command(index, argument, flags, response);
}

static bool clock_rate(unsigned divider)
{
    uint32_t setting = (12u << 16) | ((divider & 255) << 8) | ((divider >> 8) << 6) | 1u;
    wr(0x2c, 0);
    wr(0x2c, setting);
    if (!wait_register(0x2c, 2, 2, 1000))
        return false;
    wr(0x2c, setting | 4);
    delay(2000);
    return true;
}

bool lume_sd_start(uint64_t hz)
{
    ready = false;
    capacity = write_first = write_count = 0;
    boot_first = boot_count = 0;
    rca = 0;
    reads = writes = errors = rejected = 0;
    frequency = hz;
    if (!hz || hz > 1000000000)
        return false;
    lume_value("SD_HOST", HOST);
    lume_value("SD_VERSION", rd(0xfc));
    uint32_t capabilities = rd(0x40);
    lume_value("SD_CAPABILITIES", capabilities);
    /* DT emmc2-clock and the SDHCI base-clock capability both specify 200 MHz.
     * CNTFRQ (54 MHz) is only the deadline counter, not the SD clock source. */
    if (((capabilities >> 8) & 255) != 200)
        return false;
    lume_value("SD_BASE_HZ", 200000000);
    wr(0x2c, 1u << 24);
    if (!wait_register(0x2c, 7u << 24, 0, 1000))
        return false;
    wr(0x28, 0x0f00);
    delay(2000); /* VDD1 3.3V, 1-bit, PIO */
    wr(0x3c, 0);
    wr(0x38, 0);
    wr(0x34, 0xffff017f);
    wr(0x30, 0xffffffffu);
    /* Firmware leaves the base clock and SD pad mux enabled. */
    if (!clock_rate(250) || !command(0, 0, 0, 0))
        return false; /* 400 kHz identification */
    lume_value("SD_IDENTIFICATION_HZ", 400000);
    uint32_t response;
    if (!command(8, 0x1aa, RESPONSE | CHECKS, &response) || (response & 0xfff) != 0x1aa)
        return false;
    uint64_t start = lume_counter_read();
    do
    {
        if (!app(41, 0x40ff8000, RESPONSE, &response))
            return false;
        if (response & 0x80000000u)
            break;
        delay(10000);
    } while (lume_counter_read() - start < frequency * 3);
    if ((response & 0xc0000000u) != 0xc0000000u)
        return false; /* SDHC/SDXC only */
    lume_value("SD_OCR", response);
    if (!command(2, 0, (1u << 16) | (1u << 19), 0) || !command(3, 0, RESPONSE | CHECKS, &response))
        return false;
    rca = response & 0xffff0000u;
    if (!rca)
        return false;
    lume_value("SD_RCA", rca);
    if (!command(9, rca, (1u << 16) | (1u << 19), 0))
        return false;
    uint32_t csd0 = rd(0x10), csd1 = rd(0x14), csd2 = rd(0x18), csd3 = rd(0x1c);
    lume_value("SD_CSD0", csd0);
    lume_value("SD_CSD1", csd1);
    lume_value("SD_CSD2", csd2);
    lume_value("SD_CSD3", csd3);
    /* SDHCI 136-bit responses omit the low CRC byte: CSD bit N is N-8. */
    if (((csd3 >> 22) & 3) != 1)
        return false;
    uint32_t size = (csd1 >> 8) & 0x3fffffu;
    capacity = ((uint64_t)size + 1) * 1024;
    if (!command(7, rca, (3u << 16) | CHECKS, &response) || (response & 0xfdffe008u))
        return false;
    if (!clock_rate(4))
        return false; /* 25 MHz standard speed, no CMD6 high-speed switch */
    lume_value("SD_BUS_HZ", 25000000);
    ready = true;
    lume_value("SD_SECTORS", capacity);
    lume_puts("LUME NATIVE SD PIO READY\n");
    return true;
}

uint64_t lume_sd_sectors(void)
{
    return ready ? capacity : 0;
}

bool lume_sd_allow_volume(uint64_t first, uint64_t count)
{
    if (!ready || !first || !count || first >= capacity || count > capacity - first || write_count)
        return false;
    unsigned char mbr[512];
    struct lume_volume allowed;
    if (!lume_sd_read(0, mbr) || !lume_volume_select(mbr, capacity, &allowed) ||
        first != allowed.first || count != allowed.count)
        return false;
    write_first = first;
    write_count = count;
    return true;
}

bool lume_sd_allow_boot_volume(uint64_t first, uint64_t count)
{
    unsigned char mbr[512];
    struct lume_volume allowed;
    if (!ready || !write_count || !lume_sd_read(0, mbr) ||
        !lume_volume_boot_select(mbr, capacity, &allowed) || first != allowed.first ||
        count != allowed.count)
        return false;
    boot_first = first;
    boot_count = count;
    return true;
}

static bool transfer(uint64_t sector, void *data, bool write)
{
    if (!ready || sector >= capacity || sector > UINT32_MAX)
        return false;
    bool data_allowed = write_count && sector >= write_first && sector - write_first < write_count;
    bool boot_allowed = boot_count && sector >= boot_first && sector - boot_first < boot_count;
    if (write && !data_allowed && !boot_allowed)
    {
        ++rejected;
        return false;
    }
    wr(4, (1u << 16) | 512u);
    uint32_t response;
    uint32_t flags = RESPONSE | CHECKS | (1u << 21) | (write ? 0u : 16u);
    if (!command(write ? 24 : 17, (uint32_t)sector, flags, &response))
        return false;
    if (response & 0xfdffe008u)
    {
        lume_value("SD_CARD_ERROR", response);
        ++errors;
        ready = false;
        return false;
    }
    if (!event(write ? 16u : 32u, 1000))
        return false;
    unsigned char *p = data;
    for (unsigned i = 0; i < 512; i += 4)
    {
        if (write)
            wr(0x20, (uint32_t)p[i] | (uint32_t)p[i + 1] << 8 | (uint32_t)p[i + 2] << 16 |
                         (uint32_t)p[i + 3] << 24);
        else
        {
            uint32_t v = rd(0x20);
            for (unsigned j = 0; j < 4; ++j)
                p[i + j] = (unsigned char)(v >> (j * 8));
        }
    }
    if (!event(2, 2000))
        return false;
    if (write)
    {
        uint64_t start = lume_counter_read();
        do
        {
            if (!command(13, rca, RESPONSE | CHECKS, &response))
                return false;
            if (response & 0xfdffe008u)
            {
                ++errors;
                ready = false;
                return false;
            }
            if ((response & 0x1f00) == 0x900)
            {
                ++writes;
                return true;
            }
        } while (lume_counter_read() - start < frequency * 2);
        ++errors;
        ready = false;
        return false;
    }
    ++reads;
    return true;
}

bool lume_sd_read(uint64_t sector, void *data)
{
    bool complete = transfer(sector, data, false);
    if (complete)
    {
        lume_watchdog_progress();
        lume_watchdog_poll();
    }
    return complete;
}

bool lume_sd_write(uint64_t sector, const void *data)
{
    bool complete = transfer(sector, (void *)data, true);
    if (complete)
    {
        lume_watchdog_progress();
        lume_watchdog_poll();
    }
    return complete;
}

void lume_sd_stats(void)
{
    lume_value("SD_READ_SECTORS", reads);
    lume_value("SD_WRITTEN_SECTORS", writes);
    lume_value("SD_ERRORS", errors);
    lume_value("SD_REJECTED_WRITES", rejected);
}
