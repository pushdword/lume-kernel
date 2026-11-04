#include "lume/board.h"
/* BCM2712 D0 Wi-Fi SDHCI, distinct from the SD card controller. */
#include "lume/early.h"
#include "lume/mmio.h"
#include "lume/counter.h"
#include "lume/sdio.h"
#include "lume/cpu.h"
#include "lume/worker.h"
#include <limits.h>
#define HOST LUME_BOARD_WIFI_SDHCI_BASE
#define PINCTRL LUME_BOARD_BCM2712_PINCTRL_BASE
#define GPIO LUME_BOARD_BCM2712_GPIO_BASE
static uint64_t frequency, commands, errors;
static bool active;
static unsigned reservation_owner, reservation_depth;

bool lume_wifi_claim(void)
{
    unsigned owner = lume_worker_current();
    if ((reservation_depth && reservation_owner != owner) || reservation_depth == UINT_MAX)
        return false;
    reservation_owner = owner;
    ++reservation_depth;
    return true;
}

void lume_wifi_release(void)
{
    if (!reservation_depth || reservation_owner != lume_worker_current())
        lume_fail("SDIO reservation owner");
    --reservation_depth;
}

static uint32_t rd(unsigned r)
{
    return lume_mmio_read32_relaxed(HOST + r);
}

static void wr(unsigned r, uint32_t v)
{
    lume_mmio_write32_sync(HOST + r, v);
}

static void delay(unsigned us)
{
    if (!us)
        return;
    uint64_t deadline = lume_counter_read() + frequency * us / 1000000;
    if (us < 1000 || !reservation_depth || reservation_owner != lume_worker_current() ||
        !lume_worker_pause(deadline))
        lume_counter_delay_us(frequency, us);
}

static void wait_slice(uint64_t start, uint64_t deadline)
{
    uint64_t now = lume_counter_read();
    if ((int64_t)(now - deadline) >= 0)
        return;
    if (now - start < frequency / 40000)
    {
        lume_cpu_relax_ordered();
        return;
    }
    uint64_t next = now + frequency / 1000;
    if ((int64_t)(next - deadline) > 0)
        next = deadline;
    if (!lume_worker_pause(next))
        lume_cpu_relax_ordered();
}

static bool wait_register(unsigned r, uint32_t mask, uint32_t wanted, unsigned ms)
{
    uint64_t start = lume_counter_read();
    uint64_t deadline = start + frequency * ms / 1000;
    do
    {
        if ((rd(r) & mask) == wanted)
            return true;
        wait_slice(start, deadline);
    } while ((int64_t)(lume_counter_read() - deadline) < 0);
    ++errors;
    return false;
}

static bool event(uint32_t mask, unsigned ms)
{
    uint64_t start = lume_counter_read();
    uint64_t deadline = start + frequency * ms / 1000;
    uint32_t status = 0;
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
        wait_slice(start, deadline);
    } while ((int64_t)(lume_counter_read() - deadline) < 0);
    lume_value("WIFI_ERROR_INTERRUPT", status);
    lume_value("WIFI_ERROR_PRESENT", rd(0x24));
    ++errors;
    return false;
}

static bool command_owned(unsigned index, uint32_t argument, uint32_t flags, uint32_t *response)
{
    uint32_t inhibit = 1u | ((flags & ((1u << 21) | (1u << 16))) ? 2u : 0u);
    if (!wait_register(0x24, inhibit, 0, 500))
        return false;
    wr(0x30, 0xffffffffu);
    wr(8, argument);
    wr(12, (index << 24) | flags);
    ++commands;
    if (!event(1, 500))
    {
        lume_value("WIFI_ERROR_CMD", index);
        return false;
    }
    if (response)
        *response = rd(0x10);
    return (flags & (3u << 16)) != (3u << 16) || event(2, 1000);
}

static bool command(unsigned index, uint32_t argument, uint32_t flags, uint32_t *response)
{
    if (!lume_wifi_claim())
        return false;
    bool result = command_owned(index, argument, flags, response);
    lume_wifi_release();
    return result;
}

static bool transfer53(uint32_t argument, unsigned char *data, size_t bytes, bool write)
{
    unsigned fn = (argument >> 28) & 7, count = argument & 511, block_bytes;
    bool blocks = (argument & (1u << 27)) != 0;
    if (!data || !bytes || bytes > 4096 || (fn != 1 && fn != 2) || (bool)(argument >> 31) != write)
        return false;
    if (blocks)
    {
        block_bytes = fn == 1 ? 64 : 512;
        if (!count || bytes != (size_t)count * block_bytes)
            return false;
    }
    else
    {
        block_bytes = count ? count : 512;
        count = 1;
        if (bytes != block_bytes || block_bytes > (fn == 1 ? 64u : 512u))
            return false;
    }
    wr(4, (count << 16) | block_bytes);
    uint32_t response;
    uint32_t flags =
        LUME_SDIO_RESPONSE | LUME_SDIO_CHECKS | (1u << 21) | (write ? 0 : 16) | (blocks ? 34 : 0);
    if (!command_owned(53, argument, flags, &response) || (response & 0xcf00))
        return false;
    for (unsigned block = 0; block < count; ++block)
    {
        if (!event(write ? 16 : 32, 1000))
            return false;
        for (unsigned i = 0; i < block_bytes; i += 4)
        {
            if (write)
            {
                uint32_t v = 0;
                for (unsigned j = 0; j < 4 && i + j < block_bytes; ++j)
                    v |= (uint32_t)data[i + j] << (j * 8);
                wr(0x20, v);
            }
            else
            {
                uint32_t v = rd(0x20);
                for (unsigned j = 0; j < 4 && i + j < block_bytes; ++j)
                    data[i + j] = (unsigned char)(v >> (j * 8));
            }
        }
        data += block_bytes;
    }
    return event(2, 1000);
}

static bool read53(uint32_t arg, unsigned char *data, size_t bytes)
{
    if (!lume_wifi_claim())
        return false;
    bool result = transfer53(arg, data, bytes, false);
    lume_wifi_release();
    return result;
}

static bool write53(uint32_t arg, const unsigned char *data, size_t bytes)
{
    if (!lume_wifi_claim())
        return false;
    bool result = transfer53(arg, (unsigned char *)data, bytes, true);
    lume_wifi_release();
    return result;
}
static const struct lume_sdio_bus bus = {command, read53, delay, write53};

const struct lume_sdio_bus *lume_wifi_bus(void)
{
    return active ? &bus : 0;
}

static bool clock_rate(unsigned divider)
{
    if (!divider || divider > 1023)
        return false;
    uint32_t setting = (14u << 16) | ((divider & 255) << 8) | ((divider >> 8) << 6) | 1u;
    wr(0x2c, 0);
    wr(0x2c, setting);
    if (!wait_register(0x2c, 2, 2, 500))
        return false;
    wr(0x2c, setting | 4);
    delay(2000);
    return true;
}

static bool transfer_clock_owned(void)
{
    if (!active || !clock_rate(4))
        return false;
    lume_value("WIFI_BUS_HZ", 25000000);
    return true;
}

static bool probe_owned(uint64_t hz, bool d0)
{
    frequency = hz;
    commands = errors = 0;
    active = false;
    if (!hz || hz > 1000000000 || !d0)
        return false; /* Other steppings have another pad map. */
    lume_value("WIFI_HOST", HOST);
    lume_value("WIFI_VERSION", rd(0xfc));
    uint32_t capabilities = rd(0x40);
    lume_value("WIFI_CAPABILITIES", capabilities);
    unsigned base_mhz = (capabilities >> 8) & 255;
    if (base_mhz != 200)
        return false;
    lume_value("WIFI_BASE_HZ", (uint64_t)base_mhz * 1000000);
    /* D0 GPIO28 (WL_ON) and SD2 pins30..35. Preserve BT_ON and all other pads. */
    uint32_t mux = lume_mmio_read32_relaxed(PINCTRL + 8);
    lume_mmio_write32_sync(PINCTRL + 8,
                           (mux & ~0xff0f0000u) | 0x11000000u); /* GPIO28 mux0, CLK/CMD mux1 */
    mux = lume_mmio_read32_relaxed(PINCTRL + 12);
    lume_mmio_write32_sync(PINCTRL + 12, (mux & ~0xffffu) | 0x1111);
    for (unsigned pin = 30; pin <= 35; ++pin)
    {
        unsigned offset = pin < 33 ? 20 : 24, shift = ((pin - 18) % 15) * 2;
        uint32_t v = lume_mmio_read32_relaxed(PINCTRL + offset);
        lume_mmio_write32_sync(PINCTRL + offset,
                               (v & ~(3u << shift)) | ((pin == 30 ? 0u : 2u) << shift));
    }
    lume_mmio_write32_sync(GPIO + 4, lume_mmio_read32_relaxed(GPIO + 4) | (1u << 28));
    lume_mmio_write32_sync(GPIO + 8, lume_mmio_read32_relaxed(GPIO + 8) & ~(1u << 28));
    active = true;
    delay(150000);
    wr(0x2c, 1u << 24);
    if (!wait_register(0x2c, 7u << 24, 0, 500))
        return false;
    wr(0x28, 0x0f00);
    delay(2000); /* 3.3V, 1-bit, no DMA, no UHS voltage switch. */
    wr(0x3c, 0);
    wr(0x38, 0);
    wr(0x34, 0xffff017f);
    wr(0x30, 0xffffffffu);
    if (!clock_rate(250))
        return false; /* 200 MHz / (2*250) = 400 kHz */
    lume_value("WIFI_BUS_HZ", 400000);
    struct lume_sdio_identity id;
    bool ok = lume_sdio_identify(&bus, &id);
    lume_value("WIFI_OCR", id.ocr);
    lume_value("WIFI_RCA", id.rca);
    lume_value("WIFI_FUNCTIONS", id.functions);
    lume_value("WIFI_CCCR", id.cccr);
    lume_value("WIFI_SDIO_VERSION", id.version);
    lume_value("WIFI_VENDOR", id.vendor);
    lume_value("WIFI_DEVICE", id.device);
    lume_value("WIFI_CLOCK_CSR", id.clock_csr);
    lume_value("WIFI_SIGNATURE", id.signature);
    lume_value("WIFI_COMMANDS", commands);
    lume_value("WIFI_ERRORS", errors);
    lume_puts(ok ? "LUME CHECK WIFI SDIO BACKPLANE PASS\n" : "LUME WIFI SDIO PROBE INCOMPLETE\n");
    return ok;
}

static void stop_owned(void)
{
    if (!active)
        return;
    /* No DMA was enabled. Stop SD clock, hold WLAN in reset for normal reboot. */
    wr(0x38, 0);
    wr(0x34, 0);
    wr(0x2c, 0);
    lume_mmio_write32_sync(GPIO + 4, lume_mmio_read32_relaxed(GPIO + 4) & ~(1u << 28));
    active = false;
    lume_value("WIFI_TOTAL_COMMANDS", commands);
    lume_value("WIFI_TOTAL_ERRORS", errors);
    lume_puts("LUME WIFI SDIO STOPPED\n");
}

bool lume_wifi_transfer_clock(void)
{
    if (!lume_wifi_claim())
        return false;
    bool result = transfer_clock_owned();
    lume_wifi_release();
    return result;
}

bool lume_wifi_probe(uint64_t hz, bool d0)
{
    if (!lume_wifi_claim())
        return false;
    bool result = probe_owned(hz, d0);
    lume_wifi_release();
    return result;
}

void lume_wifi_stop(void)
{
    if (!lume_wifi_claim())
        return;
    stop_owned();
    lume_wifi_release();
}
