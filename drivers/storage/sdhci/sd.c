#include "lume/cpu.h"
/* The BCM2712 SDHCI driver uses bounded PIO sector groups with Auto CMD12 termination.
 * Writes start disabled and remain confined to validated Lume partitions. */
#include "lume/early.h"
#include "lume/mmio.h"
#include "lume/counter.h"
#include "lume/sd.h"
#include "lume/watchdog.h"
#include "lume/storage.h"
#include "lume/device.h"
#include "lume/block.h"
#include "lume/irq.h"
#include "lume/irq_async.h"
#include "lume/abi.h"
#include "lume/worker.h"
#ifndef LUME_SD_POLL
#define LUME_SD_POLL 0
#endif
static struct lume_resource resources[2] = {{UINT64_C(0x1000fff000), 0x100, LUME_RESOURCE_MMIO, 0},
                                            {0, 1, LUME_RESOURCE_IRQ, 0}};
static struct lume_device device;
static struct lume_block block;
static const struct lume_block_ops block_operations;
static bool interrupt_mode, irq_registered;
static volatile uint32_t pending;
static uint64_t interrupts;
static uint64_t frequency, capacity, write_first, write_count;
static uint64_t boot_first, boot_count;
static uint32_t rca;
static uint64_t reads, writes, errors, rejected;
static bool ready, transferring;
static unsigned bus_width = 1;
static uint32_t maximum_hz = 50000000;
static uint64_t configuration_base, gpio_base;
static unsigned uhs_modes;
static bool voltage_1v8, legacy_retry, tuning_active, tuning_required;
static uint32_t bus_hz;
static uint64_t tuned_at;
static uint64_t retune_interval;
static uint32_t interrupt_mask = 0xffff017f;
static volatile bool retune_requested;

static uint32_t rd(unsigned reg)
{
    return lume_mmio_read32_relaxed(resources[0].base + reg);
}

static void wr(unsigned reg, uint32_t value)
{
    lume_mmio_write32_sync(resources[0].base + reg, value);
}

static void irq_handler(unsigned irq, void *context)
{
    (void)irq;
    (void)context;
    uint32_t status = rd(0x30);
    if (status & 4096u)
        __atomic_store_n(&retune_requested, true, __ATOMIC_RELEASE);
    __atomic_fetch_or(&pending, status, __ATOMIC_RELEASE);
    wr(0x30, status);
    __atomic_fetch_add(&interrupts, 1, __ATOMIC_RELAXED);
}

bool lume_sd_configure(uint64_t base, uint64_t size, uint32_t irq)
{
    if (device.state != LUME_DEVICE_OFF || base != UINT64_C(0x1000fff000) || size < 0x100 ||
        size > 0x1000 || irq < 32 || irq >= LUME_IRQ_LIMIT)
        return false;
    resources[0].base = base;
    resources[0].size = size;
    resources[1].base = irq;
    return true;
}

bool lume_sd_configure_bus(unsigned width, uint32_t hz)
{
    if (device.state != LUME_DEVICE_OFF || (width != 1 && width != 4) || hz < 400000)
        return false;
    bus_width = width;
    maximum_hz = hz > 200000000 ? 200000000 : hz;
    return true;
}

bool lume_sd_configure_uhs(uint64_t configuration, uint64_t gpio, unsigned modes)
{
    if (device.state != LUME_DEVICE_OFF || (modes & ~3u) ||
        (modes && (configuration != UINT64_C(0x1000fff400) || gpio != UINT64_C(0x107d517c00))))
        return false;
    configuration_base = configuration;
    gpio_base = gpio;
    uhs_modes = modes;
    return true;
}

static void delay(unsigned us)
{
    if (!lume_worker_pause(lume_counter_read() + frequency * us / 1000000))
        lume_counter_delay_us(frequency, us);
}

static bool wait_register(unsigned reg, uint32_t mask, uint32_t wanted, unsigned ms)
{
    uint64_t start = lume_counter_read();
    do
    {
        if ((rd(reg) & mask) == wanted)
            return true;
        (void)lume_worker_pause(lume_counter_read() + frequency / 1000);
    } while (lume_counter_read() - start < frequency * ms / 1000);
    return false;
}

static bool event(uint32_t mask, unsigned ms)
{
    uint64_t start = lume_counter_read();
    uint64_t flags = lume_irq_save();
    uint32_t status = 0;
    do
    {
        if (interrupt_mode)
        {
            uint32_t asserted = rd(0x30);
            if (asserted & 4096u)
                __atomic_store_n(&retune_requested, true, __ATOMIC_RELEASE);
            if (asserted)
            {
                __atomic_fetch_or(&pending, asserted, __ATOMIC_RELEASE);
                wr(0x30, asserted);
            }
        }
        status = interrupt_mode ? __atomic_load_n(&pending, __ATOMIC_ACQUIRE) : rd(0x30);
        if (status & 0xffff8000u)
            break;
        /* Buffer readiness is a level. Its interrupt notifications can coalesce between blocks. */
        bool buffer_ready =
            !tuning_active && (mask == 16u || mask == 32u) && (rd(0x24) & (mask << 6));
        if ((status & mask) || buffer_ready)
        {
            if (interrupt_mode)
                __atomic_fetch_and(&pending, ~mask, __ATOMIC_ACQ_REL);
            if (!interrupt_mode || buffer_ready)
                wr(0x30, mask);
            lume_irq_restore(flags);
            return true;
        }
        /* Short PIO gaps avoid a scheduler round trip. Longer waits release the worker. */
        if (lume_counter_read() - start < frequency / 40000)
        {
            lume_cpu_relax_ordered();
            continue;
        }
        if (interrupt_mode)
        {
            /* Service an already asserted IRQ before suspending the call stack. */
            lume_irq_restore(flags & ~UINT64_C(0x80));
            lume_cpu_relax_ordered();
            (void)lume_irq_save();
            if (!(__atomic_load_n(&pending, __ATOMIC_ACQUIRE) & (mask | 0xffff8000u)))
            {
                uint64_t deadline = start + frequency * ms / 1000;
                uint64_t check = lume_counter_read() + frequency / 1000;
                if ((mask == 16u || mask == 32u) && (int64_t)(check - deadline) < 0)
                    deadline = check;
                (void)lume_worker_wait_bits(&pending, mask | 0xffff8000u, deadline);
            }
        }
        else if (!lume_worker_pause(lume_counter_read() + frequency / 1000))
            lume_cpu_relax_ordered();
    } while (lume_counter_read() - start < frequency * ms / 1000);
    lume_irq_restore(flags);
    lume_value("SD_ERROR_INTERRUPT", status);
    lume_value("SD_ERROR_PRESENT", rd(0x24));
    if (status & (1u << 24))
        lume_value("SD_ERROR_AUTO_CMD", rd(0x3c) & 0xffffu);
    ++errors;
    ready = false;
    if (interrupt_mode)
    {
        wr(0x38, 0);
        (void)lume_irq_enable((unsigned)resources[1].base, false);
    }
    return false;
}

#define RESPONSE (2u << 16)
#define CHECKS ((1u << 19) | (1u << 20))

static bool command(unsigned index, uint32_t argument, uint32_t flags, uint32_t *response)
{
    uint32_t inhibit = 1u | ((index != 12 && (flags & ((1u << 21) | (1u << 16)))) ? 2u : 0u);
    if (!wait_register(0x24, inhibit, 0, 1000))
    {
        lume_value("SD_INHIBIT_CMD", index);
        ++errors;
        ready = false;
        return false;
    }
    wr(0x30, 0xffffffffu);
    __atomic_store_n(&pending, 0, __ATOMIC_RELEASE);
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

static void gpio_output(unsigned pin, bool high)
{
    uint32_t mask = 1u << pin;
    uint32_t value = lume_mmio_read32_relaxed(gpio_base + 4);
    lume_mmio_write32_sync(gpio_base + 4, high ? value | mask : value & ~mask);
    value = lume_mmio_read32_relaxed(gpio_base + 8);
    lume_mmio_write32_sync(gpio_base + 8, value & ~mask);
}

static void card_power_cycle(void)
{
    wr(0x2c, 0);
    wr(0x28, 0);
    gpio_output(4, false);
    delay(10000);
    gpio_output(3, false);
    delay(5000);
    gpio_output(4, true);
    delay(10000);
    voltage_1v8 = false;
}

static bool switch_voltage(void)
{
    uint32_t response;
    if (!command(11, 0, RESPONSE | CHECKS, &response) || (response & 0xfdffe008u))
        return false;
    if (!wait_register(0x24, 15u << 20, 0, 10))
        return false;
    uint32_t clock = rd(0x2c);
    wr(0x2c, clock & ~4u);
    gpio_output(3, true);
    wr(0x3c, (rd(0x3c) & 0xffff0000u) | (8u << 16));
    delay(5000);
    if (!(rd(0x3c) & (8u << 16)))
        return false;
    wr(0x2c, clock);
    delay(1000);
    if (!wait_register(0x24, 15u << 20, 15u << 20, 10))
        return false;
    voltage_1v8 = true;
    return true;
}

static bool tune(void)
{
    uint32_t signals = rd(0x38);
    uint32_t enabled = rd(0x34);
    tuning_active = true;
    wr(0x34, 32);
    wr(0x38, interrupt_mode ? 32 : 0);
    wr(0x3c, (rd(0x3c) & 0xffff0000u) | (64u << 16));
    bool success = false;
    for (unsigned i = 0; i < 40; ++i)
    {
        if (!wait_register(0x24, 1u, 0, 50))
            break;
        wr(4, (1u << 16) | 64u);
        wr(0x30, 0xffffffffu);
        __atomic_store_n(&pending, 0, __ATOMIC_RELEASE);
        wr(8, 0);
        wr(12, (19u << 24) | RESPONSE | CHECKS | (1u << 21) | 16u);
        if (!event(32, 50) || (rd(0x10) & 0xfdffe008u))
            break;
        delay(1000);
        if (!(rd(0x3c) & (64u << 16)))
        {
            success = (rd(0x3c) & (128u << 16)) != 0;
            break;
        }
    }
    if (!success)
    {
        lume_value("SD_TUNING_CONTROL", rd(0x3c) >> 16);
        wr(0x3c, rd(0x3c) & 0xffff0000u & ~(192u << 16));
    }
    wr(0x30, 0xffffffffu);
    __atomic_store_n(&pending, 0, __ATOMIC_RELEASE);
    wr(0x34, enabled);
    wr(0x38, signals);
    tuning_active = false;
    if (success)
        tuned_at = lume_counter_read();
    return success;
}

static bool read_register_data(unsigned index, uint32_t argument, bool application,
                               unsigned char *data, unsigned bytes)
{
    uint32_t response;
    wr(4, (1u << 16) | bytes);
    uint32_t flags = RESPONSE | CHECKS | (1u << 21) | 16u;
    bool success = application ? app(index, argument, flags, &response)
                               : command(index, argument, flags, &response);
    if (!success)
        return false;
    if (response & 0xfdffe008u)
    {
        lume_value("SD_CARD_ERROR", response);
        ++errors;
        ready = false;
        return false;
    }
    if (!event(32, 1000))
        return false;
    for (unsigned i = 0; i < bytes; i += 4)
    {
        uint32_t value = rd(0x20);
        for (unsigned j = 0; j < 4; ++j)
            data[i + j] = (unsigned char)(value >> (8 * j));
    }
    return event(2, 1000);
}

static bool select_bus(uint32_t capabilities, uint32_t csd2)
{
    unsigned char scr[8], status[64];
    uint32_t response;
    uint32_t hz = maximum_hz < 25000000 ? maximum_hz : 25000000;
    unsigned divider = (100000000u + hz - 1) / hz;
    if (!clock_rate(divider) || !read_register_data(51, 0, true, scr, sizeof(scr)))
        return false;
    unsigned width = 1;
    if (bus_width == 4 && (scr[1] & 4))
    {
        if (!app(6, 2, RESPONSE | CHECKS, &response))
            return false;
        if (response & 0xfdffe008u)
        {
            ++errors;
            return false;
        }
        wr(0x28, rd(0x28) | 2u);
        width = 4;
    }
    if (voltage_1v8 && width == 4)
    {
        if (!read_register_data(6, 0x00fffff0, false, status, sizeof(status)))
            return false;
        unsigned mode = maximum_hz > 100000000 && (uhs_modes & 2) && (status[13] & 8)  ? 3
                        : maximum_hz > 50000000 && (uhs_modes & 1) && (status[13] & 4) ? 2
                                                                                       : 1;
        if (!read_register_data(6, 0x80fffff0u | mode, false, status, sizeof(status)) ||
            (status[16] & 15) != mode)
            return false;
        wr(0x28, rd(0x28) | 4u);
        uint32_t control = rd(0x3c) & 0xffff0000u;
        wr(0x3c, (control & ~(7u << 16)) | (mode << 16));
        hz = mode == 3 ? 200000000u : mode == 2 ? 100000000u : 50000000u;
        if (hz > maximum_hz)
            hz = maximum_hz;
        divider = hz >= 200000000 ? 0 : (100000000u + hz - 1) / hz;
        uint32_t setting = lume_mmio_read32_relaxed(configuration_base + 0x1ac);
        lume_mmio_write32_sync(configuration_base + 0x1ac, (setting & ~1u) | (1u << 31));
        setting = lume_mmio_read32_relaxed(configuration_base + 0x44);
        lume_mmio_write32_sync(configuration_base + 0x44, (setting & ~3u) | 2u);
        if (!clock_rate(divider))
            return false;
        tuning_required = mode == 3 || (mode == 2 && (rd(0x44) & (1u << 13)));
        if (tuning_required && !tune())
            return false;
    }
    else if (maximum_hz > 25000000 && (capabilities & (1u << 21)) && (csd2 & (1u << 22)) &&
             (scr[0] & 15))
    {
        if (!read_register_data(6, 0x00fffff1, false, status, sizeof(status)))
            return false;
        if (status[13] & 2)
        {
            if (!read_register_data(6, 0x80fffff1, false, status, sizeof(status)))
                return false;
            if ((status[16] & 15) == 1)
            {
                wr(0x28, rd(0x28) | 4u);
                hz = maximum_hz < 50000000 ? maximum_hz : 50000000;
                divider = (100000000u + hz - 1) / hz;
                if (!clock_rate(divider))
                    return false;
            }
        }
    }
    lume_value("SD_BUS_WIDTH", width);
    bus_hz = divider ? 100000000u / divider : 200000000u;
    lume_value("SD_BUS_HZ", bus_hz);
    lume_value("SD_SIGNAL_MV", voltage_1v8 ? 1800 : 3300);
    return true;
}

static bool initialize(uint64_t hz)
{
    ready = transferring = false;
    capacity = write_first = write_count = 0;
    boot_first = boot_count = 0;
    rca = 0;
    reads = writes = errors = rejected = 0;
    frequency = hz;
    if (!hz || hz > 1000000000)
        return false;
    voltage_1v8 = false;
    bus_hz = 0;
    tuned_at = 0;
    tuning_active = tuning_required = false;
    __atomic_store_n(&retune_requested, false, __ATOMIC_RELEASE);
    if (uhs_modes)
        card_power_cycle();
    lume_value("SD_HOST", resources[0].base);
    lume_value("SD_VERSION", rd(0xfc));
    uint32_t capabilities = rd(0x40);
    lume_value("SD_CAPABILITIES", capabilities);
    uint32_t capabilities_1 = rd(0x44);
    lume_value("SD_CAPABILITIES_1", capabilities_1);
    unsigned timer = (capabilities_1 >> 8) & 15;
    unsigned retune_mode = (capabilities_1 >> 14) & 3;
    unsigned seconds = !retune_mode && timer ? 1u << (timer - 1) : 0;
    retune_interval = frequency * seconds;
    interrupt_mask = 0xffff017fu | (retune_mode == 1 || retune_mode == 2 ? 4096u : 0u);
    lume_value("SD_RETUNE_SECONDS", seconds);
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
    wr(0x34, interrupt_mask);
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
        uint32_t request = 0x40ff8000u;
        if (uhs_modes && !legacy_retry && bus_width == 4 && maximum_hz > 50000000)
            request |= 1u << 24;
        if (!app(41, request, RESPONSE, &response))
            return false;
        if (response & 0x80000000u)
            break;
        delay(10000);
    } while (lume_counter_read() - start < frequency * 3);
    if ((response & 0xc0000000u) != 0xc0000000u)
        return false; /* SDHC/SDXC only */
    lume_value("SD_OCR", response);
    if ((response & (1u << 24)) && !switch_voltage())
        return false;
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
    if (!select_bus(capabilities, csd2))
        return false;
    ready = true;
    lume_value("SD_SECTORS", capacity);
    lume_puts("LUME NATIVE SD PIO READY\n");
    return true;
}

static int probe(struct lume_device *d)
{
    (void)d;
    if (!lume_irq_available())
        return -LUME_EIO;
    __atomic_store_n(&pending, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&interrupts, 0, __ATOMIC_RELAXED);
    return 0;
}

static int start(struct lume_device *d)
{
    (void)d;
    legacy_retry = false;
    if (!initialize(frequency))
    {
        if (!uhs_modes)
            return -LUME_EIO;
        lume_puts("SD UHS negotiation failed. Retrying at 3.3 V.\n");
        legacy_retry = true;
        if (!initialize(frequency))
            return -LUME_EIO;
    }
    block.sectors = capacity;
    if (resources[1].base && !LUME_SD_POLL)
    {
        int result = lume_irq_register_async((unsigned)resources[1].base, LUME_IRQ_LEVEL,
                                             irq_handler, &device);
        if (result)
            return result;
        irq_registered = true;
        if ((result = lume_irq_enable((unsigned)resources[1].base, true)) != 0)
            return result;
        interrupt_mode = true;
        wr(0x38, interrupt_mask);
    }
    lume_value("SD_IRQ_MODE", interrupt_mode);
    return 0;
}

static int stop(struct lume_device *d)
{
    (void)d;
    wr(0x38, 0);
    if (irq_registered)
    {
        int result = lume_irq_unregister((unsigned)resources[1].base, &device);
        if (result)
            return result;
        irq_registered = false;
    }
    interrupt_mode = ready = false;
    __atomic_store_n(&pending, 0, __ATOMIC_RELEASE);
    if (voltage_1v8)
        card_power_cycle();
    return 0;
}
static const struct lume_driver_ops operations = {probe, start, stop, 0};

bool lume_sd_start(uint64_t hz)
{
    if (!hz || hz > 1000000000 || device.state != LUME_DEVICE_OFF)
        return false;
    frequency = hz;
    device = (struct lume_device){.version = LUME_DDK_VERSION,
                                  .class_id = LUME_DEVICE_BLOCK,
                                  .name = "sdhci0",
                                  .resources = resources,
#ifdef LUME_PLATFORM_LKM
                                  .resource_count = 0,
#else
                                  .resource_count = resources[1].base ? 2 : 1,
#endif
                                  .ops = &operations};
    block = (struct lume_block){.device = &device,
                                .ops = &block_operations,
                                .sector_bytes = 512,
                                .max_sectors = LUME_SD_MAX_SECTORS};
    return lume_device_attach(&device) == 0;
}

bool lume_sd_stop(void)
{
    if (transferring)
        return false;
    return device.state == LUME_DEVICE_OFF || lume_device_detach(&device) == 0;
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

static void abort_transfer(void)
{
    bool use_interrupts = interrupt_mode;
    wr(0x38, 0);
    if (use_interrupts)
        (void)lume_irq_enable((unsigned)resources[1].base, false);
    interrupt_mode = false;
    /* Reset the host command and data paths before issuing an abort command. */
    wr(0x2c, rd(0x2c) | (3u << 25));
    if (wait_register(0x2c, 3u << 25, 0, 1000))
        (void)command(12, 0, (3u << 16) | CHECKS | (3u << 22), NULL);
    interrupt_mode = use_interrupts;
    ready = false;
}

static bool transfer(uint64_t sector, unsigned count, void *data, bool write)
{
    if (!ready || transferring || !data || !count || count > LUME_SD_MAX_SECTORS ||
        sector >= capacity || count > capacity - sector || sector > UINT32_MAX ||
        count - 1 > UINT32_MAX - sector)
        return false;
    bool data_allowed = write_count && sector >= write_first &&
                        sector - write_first < write_count &&
                        count <= write_count - (sector - write_first);
    bool boot_allowed = boot_count && sector >= boot_first && sector - boot_first < boot_count &&
                        count <= boot_count - (sector - boot_first);
    if (write && !data_allowed && !boot_allowed)
    {
        ++rejected;
        return false;
    }
    transferring = true;
    bool requested =
        __atomic_exchange_n(&retune_requested, false, __ATOMIC_ACQ_REL) || (rd(0x30) & 4096u);
    if (tuning_required &&
        (requested || (retune_interval && lume_counter_read() - tuned_at >= retune_interval)) &&
        !tune())
        goto failed;
    wr(4, (count << 16) | 512u);
    uint32_t response;
    uint32_t flags = RESPONSE | CHECKS | (1u << 21) | (write ? 0u : 16u);
    if (count > 1)
        flags |= 2u | 4u | 32u;
    unsigned index = write ? (count > 1 ? 25 : 24) : (count > 1 ? 18 : 17);
    if (!command(index, (uint32_t)sector, flags, &response))
        goto failed;
    if (response & 0xfdffe008u)
    {
        lume_value("SD_CARD_ERROR", response);
        ++errors;
        goto failed;
    }
    unsigned char *p = data;
    for (unsigned block = 0; block < count; ++block)
    {
        if (!event(write ? 16u : 32u, 1000))
            goto failed;
        for (unsigned i = 0; i < 512; i += 4)
        {
            if (write)
                lume_mmio_write32_relaxed(resources[0].base + 0x20,
                                          (uint32_t)p[i] | (uint32_t)p[i + 1] << 8 |
                                              (uint32_t)p[i + 2] << 16 | (uint32_t)p[i + 3] << 24);
            else
            {
                uint32_t v = rd(0x20);
                for (unsigned j = 0; j < 4; ++j)
                    p[i + j] = (unsigned char)(v >> (j * 8));
            }
        }
        p += 512;
        lume_watchdog_progress();
        lume_watchdog_poll();
        lume_worker_checkpoint(512);
    }
    if (!event(2, 2000))
        goto failed;
    if (write || count > 1)
    {
        uint64_t start = lume_counter_read();
        do
        {
            if (!command(13, rca, RESPONSE | CHECKS, &response))
                goto failed;
            if (response & 0xfdffe008u)
            {
                ++errors;
                goto failed;
            }
            if ((response & 0x1f00) == 0x900)
            {
                break;
            }
            if (lume_counter_read() - start >= frequency * 2)
            {
                ++errors;
                goto failed;
            }
            delay(1000);
        } while (true);
    }
    if (write)
        writes += count;
    else
        reads += count;
    transferring = false;
    return true;
failed:
    if (count > 1)
        abort_transfer();
    ready = transferring = false;
    return false;
}

bool lume_sd_read(uint64_t sector, void *data)
{
    return lume_sd_read_blocks(sector, 1, data);
}

bool lume_sd_write(uint64_t sector, const void *data)
{
    return lume_sd_write_blocks(sector, 1, data);
}

bool lume_sd_read_blocks(uint64_t sector, unsigned count, void *data)
{
    return lume_block_read(&block, sector, count, data) == 0;
}

bool lume_sd_write_blocks(uint64_t sector, unsigned count, const void *data)
{
    return lume_block_write(&block, sector, count, data) == 0;
}

void lume_sd_stats(void)
{
    lume_value("SD_READ_SECTORS", reads);
    lume_value("SD_WRITTEN_SECTORS", writes);
    lume_value("SD_ERRORS", errors);
    lume_value("SD_REJECTED_WRITES", rejected);
    lume_value("SD_IRQ_HANDLED", __atomic_load_n(&interrupts, __ATOMIC_RELAXED));
    lume_value("SD_BUS_HZ", bus_hz);
    lume_value("SD_SIGNAL_MV", voltage_1v8 ? 1800 : 3300);
    lume_worker_stats();
}

static int block_read(struct lume_block *b, uint64_t sector, unsigned count, void *data)
{
    (void)b;
    return transfer(sector, count, data, false) ? 0 : -LUME_EIO;
}

static int block_write(struct lume_block *b, uint64_t sector, unsigned count, const void *data)
{
    (void)b;
    return transfer(sector, count, (void *)data, true) ? 0 : -LUME_EIO;
}

static int block_sync(struct lume_block *b)
{
    (void)b;
    return ready && !transferring ? 0 : -LUME_EIO;
}
static const struct lume_block_ops block_operations = {block_read, block_write, block_sync};
