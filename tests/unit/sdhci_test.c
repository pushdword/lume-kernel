#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LUME_MMIO_H
#define LUME_COUNTER_H
#define LUME_ARCH_CPU_H

static uint32_t model_read(uintptr_t);
static void model_write(uintptr_t, uint32_t);
static uint64_t model_counter(void);
static void model_delay(uint64_t, unsigned);
static void deliver_irq(void);

#define lume_mmio_read32_relaxed model_read
#define lume_mmio_write32_sync model_write
#define lume_counter_read model_counter
#define lume_counter_delay_us model_delay
#define lume_cpu_relax_ordered deliver_irq
#include "../../drivers/storage/sdhci/sd.c"

static unsigned checks, cases;
static uint32_t registers[64];
static unsigned char card[128][512];
static uint64_t ticks, irq_flags;
static unsigned mmio_calls, data_commands, status_commands, stops, auto_stops, resets;
static unsigned sectors_done, words, total_blocks, first_sector, checkpoint_calls, watchdog_calls;
static unsigned wait_calls, busy_responses, failed_block, error_command;
static uint32_t deferred, card_error, response_error;
static bool writing, defer_events, missing_complete, auto_error, reset_stuck, missing_ready;
static bool reenter, card_busy, abort_inhibit;

#define CHECK(condition)                                                                           \
    do                                                                                             \
    {                                                                                              \
        ++checks;                                                                                  \
        if (!(condition))                                                                          \
        {                                                                                          \
            fprintf(stderr, "SDHCI check failed at line %u: %s\n", (unsigned)__LINE__,             \
                    #condition);                                                                   \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

static void queue_events(uint32_t events)
{
    if (defer_events && interrupt_mode)
        deferred |= events;
    else
        registers[0x30 / 4] |= events;
}

static void deliver_irq(void)
{
    if (interrupt_mode && !(irq_flags & 0x80) && (registers[0x30 / 4] & registers[0x38 / 4]))
        irq_handler((unsigned)resources[1].base, NULL);
}

static void sector_complete(void)
{
    ++sectors_done;
    if (sectors_done == total_blocks)
    {
        if (total_blocks > 1 && !auto_error)
            ++auto_stops;
        if (auto_error)
        {
            registers[0x3c / 4] = 1;
            queue_events((1u << 24) | 0x8000u | 2u);
        }
        else if (!missing_complete)
            queue_events(2);
    }
    else if (sectors_done == failed_block)
        queue_events((1u << 21) | 0x8000u);
    else
        queue_events(writing ? 16u : 32u);
}

static unsigned register_index(uintptr_t address)
{
    CHECK(address >= resources[0].base && address - resources[0].base < 256);
    CHECK(!(address & 3));
    ++mmio_calls;
    return (unsigned)(address - resources[0].base) / 4;
}

static uint32_t model_read(uintptr_t address)
{
    unsigned reg = register_index(address);
    if (reg != 0x20 / 4)
        return registers[reg];
    CHECK(!writing && words < total_blocks * 128);
    unsigned char *p = card[first_sector + words / 128] + (words % 128) * 4;
    uint32_t value =
        (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
    if (++words % 128 == 0)
        sector_complete();
    return value;
}

static void model_write(uintptr_t address, uint32_t value)
{
    unsigned reg = register_index(address);
    if (reg == 0x30 / 4)
    {
        registers[reg] &= ~value;
        return;
    }
    registers[reg] = value;
    if (reg == 0x2c / 4 && (value & (3u << 25)))
    {
        ++resets;
        if (!reset_stuck)
        {
            registers[reg] &= ~(3u << 25);
            registers[0x24 / 4] = abort_inhibit ? 2 : 0;
        }
        return;
    }
    if (reg == 0x20 / 4)
    {
        CHECK(writing && words < total_blocks * 128);
        unsigned char *p = card[first_sector + words / 128] + (words % 128) * 4;
        for (unsigned i = 0; i < 4; ++i)
            p[i] = (unsigned char)(value >> (8 * i));
        if (++words % 128 == 0)
            sector_complete();
        return;
    }
    if (reg != 12 / 4)
        return;
    unsigned index = value >> 24;
    if (index == 12)
    {
        CHECK((value & (3u << 22)) == (3u << 22));
        CHECK(registers[8 / 4] == 0);
        ++stops;
        queue_events(index == error_command ? (1u << 16) | 0x8000u : 3u);
        return;
    }
    if (index == 13)
    {
        CHECK(registers[8 / 4] == rca);
        ++status_commands;
        registers[0x10 / 4] = card_error                    ? card_error
                              : card_busy || busy_responses ? 0xe00u
                                                            : 0x900u;
        if (busy_responses)
            --busy_responses;
        queue_events(index == error_command ? (1u << 16) | 0x8000u : 1u);
        return;
    }
    CHECK(index == 17 || index == 18 || index == 24 || index == 25);
    ++data_commands;
    total_blocks = registers[4 / 4] >> 16;
    CHECK(total_blocks && total_blocks <= LUME_SD_MAX_SECTORS);
    CHECK((registers[4 / 4] & 0xffff) == 512);
    CHECK((total_blocks > 1) == (index == 18 || index == 25));
    CHECK((value & 0x3fu) ==
          ((index == 17 || index == 18 ? 16u : 0u) | (total_blocks > 1 ? 38u : 0u)));
    CHECK((value & 0x003f0000u) == (RESPONSE | CHECKS | (1u << 21)));
    first_sector = registers[8 / 4];
    CHECK(first_sector + total_blocks <= 128);
    words = sectors_done = 0;
    writing = index == 24 || index == 25;
    registers[0x10 / 4] = response_error;
    queue_events(index == error_command ? (1u << 16) | 0x8000u
                                        : 1u | (missing_ready ? 0u
                                                : writing     ? 16u
                                                              : 32u));
}

static uint64_t model_counter(void)
{
    ticks += 1000;
    return ticks;
}

static void model_delay(uint64_t hz, unsigned us)
{
    ticks += hz * us / 1000000;
}

uint64_t lume_irq_save(void)
{
    uint64_t saved = irq_flags;
    irq_flags = 0x80;
    return saved;
}

void lume_irq_restore(uint64_t flags)
{
    irq_flags = flags;
    deliver_irq();
}

bool lume_irq_available(void)
{
    return true;
}

int lume_irq_register(unsigned irq, unsigned trigger, lume_irq_handler handler, void *context)
{
    (void)irq;
    (void)trigger;
    (void)handler;
    (void)context;
    return 0;
}

int lume_irq_enable(unsigned irq, bool enabled)
{
    (void)irq;
    (void)enabled;
    return 0;
}

int lume_irq_unregister(unsigned irq, void *context)
{
    (void)irq;
    (void)context;
    return 0;
}

int lume_device_attach(struct lume_device *d)
{
    (void)d;
    return -LUME_ENOSYS;
}

int lume_device_detach(struct lume_device *d)
{
    (void)d;
    return -LUME_ENOSYS;
}

void lume_value(const char *name, uint64_t value)
{
    (void)name;
    (void)value;
}

void lume_puts(const char *text)
{
    (void)text;
}

void lume_watchdog_progress(void)
{
    ++watchdog_calls;
}

void lume_watchdog_poll(void)
{
}

bool lume_worker_pause(uint64_t deadline)
{
    ticks = deadline;
    return true;
}

bool lume_worker_wait_bits(volatile uint32_t *value, uint32_t mask, uint64_t deadline)
{
    ++wait_calls;
    registers[0x30 / 4] |= deferred;
    deferred = 0;
    lume_irq_restore(0);
    irq_flags = 0x80;
    if (!(*value & mask))
        ticks = deadline;
    return true;
}

void lume_worker_checkpoint(unsigned bytes)
{
    CHECK(bytes == 512 && transferring);
    ++checkpoint_calls;
    if (reenter)
    {
        unsigned before = mmio_calls;
        unsigned char data[512];
        CHECK(!lume_sd_read(0, data));
        CHECK(!lume_sd_stop());
        CHECK(lume_block_sync(&block) == -LUME_EIO);
        CHECK(mmio_calls == before && transferring);
    }
}

void lume_worker_stats(void)
{
}

static void setup(bool use_interrupts)
{
    ++cases;
    memset(registers, 0, sizeof(registers));
    memset(card, 0x55, sizeof(card));
    ticks = 0;
    irq_flags = 0x80;
    mmio_calls = data_commands = status_commands = stops = auto_stops = resets = 0;
    sectors_done = words = total_blocks = first_sector = checkpoint_calls = watchdog_calls = 0;
    wait_calls = busy_responses = error_command = 0;
    failed_block = UINT32_MAX;
    deferred = card_error = response_error = 0;
    writing = missing_complete = auto_error = reset_stuck = missing_ready = false;
    reenter = card_busy = abort_inhibit = false;
    interrupt_mode = defer_events = use_interrupts;
    pending = 0;
    registers[0x38 / 4] = use_interrupts ? UINT32_MAX : 0;
    frequency = 1000000;
    capacity = 128;
    write_first = 16;
    write_count = 64;
    boot_first = 4;
    boot_count = 8;
    rca = 0x10000;
    reads = writes = errors = rejected = 0;
    ready = true;
    transferring = false;
    device.state = LUME_DEVICE_READY;
    block = (struct lume_block){.device = &device,
                                .ops = &block_operations,
                                .sectors = capacity,
                                .sector_bytes = 512,
                                .max_sectors = LUME_SD_MAX_SECTORS};
}

static void success(bool use_interrupts)
{
    unsigned char data[LUME_SD_MAX_SECTORS * 512 + 1], out[sizeof(data)];
    for (unsigned i = 0; i < sizeof(data); ++i)
        data[i] = (unsigned char)(i * 17 + i / 512);
    const unsigned counts[] = {1, 2, 3, 16, 31, 32};
    for (unsigned i = 0; i < sizeof(counts) / sizeof(counts[0]); ++i)
    {
        unsigned count = counts[i];
        setup(use_interrupts);
        CHECK(lume_sd_write_blocks(16, count, data + 1));
        CHECK(!memcmp(card[16], data + 1, count * 512));
        CHECK(writes == count && data_commands == 1 && status_commands == 1);
        CHECK(auto_stops == (count > 1) && !stops && !resets);
        CHECK(checkpoint_calls == count && watchdog_calls == count && ready && !transferring);
        CHECK(lume_sd_read_blocks(16, count, out + 1));
        CHECK(!memcmp(out + 1, data + 1, count * 512));
        CHECK(reads == count && data_commands == 2 && status_commands == (count > 1 ? 2u : 1u));
        CHECK(lume_block_sync(&block) == 0);
        CHECK(irq_flags == 0x80 && (!use_interrupts || wait_calls));
    }
    setup(use_interrupts);
    busy_responses = 3;
    reenter = true;
    ticks = UINT64_MAX - 15000;
    CHECK(lume_sd_write_blocks(16, 4, data));
    CHECK(status_commands == 4 && writes == 4 && !errors);
    setup(use_interrupts);
    CHECK(lume_sd_write_blocks(boot_first, 8, data));
    CHECK(!memcmp(card[boot_first], data, 8 * 512));
}

static void boundaries(void)
{
    unsigned char data[(LUME_SD_MAX_SECTORS + 1) * 512] = {0};
    setup(false);
    CHECK(!lume_sd_read_blocks(0, 0, data));
    CHECK(!lume_sd_read_blocks(0, LUME_SD_MAX_SECTORS + 1, data));
    CHECK(!lume_sd_read_blocks(0, 1, NULL));
    CHECK(!lume_sd_read_blocks(capacity - 1, 2, data));
    CHECK(!lume_sd_read_blocks(UINT64_MAX, 2, data));
    CHECK(!lume_sd_write_blocks(write_first - 1, 2, data));
    CHECK(!lume_sd_write_blocks(write_first + write_count - 1, 2, data));
    CHECK(!lume_sd_write_blocks(boot_first + boot_count - 1, 2, data));
    CHECK(!lume_sd_write_blocks(0, 1, data));
    CHECK(!mmio_calls && !data_commands);
    capacity = block.sectors = UINT64_C(0x100000010);
    CHECK(!lume_sd_read_blocks(UINT32_MAX, 2, data));
    CHECK(!lume_sd_read_blocks(UINT64_C(0x100000000), 1, data));
    CHECK(!mmio_calls);
}

static void failures(bool use_interrupts, bool write)
{
    unsigned char data[LUME_SD_MAX_SECTORS * 512];
    memset(data, 0x37, sizeof(data));
    for (unsigned scenario = 0; scenario < 12; ++scenario)
    {
        setup(use_interrupts);
        if (scenario == 0)
            failed_block = 1;
        else if (scenario == 1)
            auto_error = true;
        else if (scenario == 2)
            missing_complete = true;
        else if (scenario == 3)
            card_error = 1u << 22;
        else if (scenario == 4)
            card_busy = true;
        else if (scenario == 5)
            error_command = write ? 25 : 18;
        else if (scenario == 6)
            error_command = 13;
        else if (scenario == 7)
        {
            failed_block = 1;
            reset_stuck = true;
        }
        else if (scenario == 8)
            registers[0x24 / 4] = 3;
        else if (scenario == 9)
            response_error = 1u << 22;
        else if (scenario == 10)
            missing_ready = true;
        else
        {
            failed_block = 1;
            error_command = 12;
        }
        abort_inhibit = true;
        CHECK(!(write ? lume_sd_write_blocks(16, 4, data) : lume_sd_read_blocks(16, 4, data)));
        CHECK(!ready && !transferring && !writes && !reads && errors && resets == 1);
        CHECK(stops == (reset_stuck ? 0u : 1u));
        CHECK(lume_block_sync(&block) == -LUME_EIO);
        unsigned before = mmio_calls;
        CHECK(!lume_sd_read(16, data));
        CHECK(!lume_sd_write(16, data));
        CHECK(mmio_calls == before);
        CHECK(irq_flags == 0x80);
        if (scenario == 0 && write)
        {
            CHECK(!memcmp(card[16], data, 512));
            CHECK(card[17][0] == 0x55 && checkpoint_calls == 1);
        }
    }
}

static void command_counts(void)
{
    unsigned char data[LUME_SD_MAX_SECTORS * 512] = {0};
    setup(false);
    for (unsigned i = 0; i < LUME_SD_MAX_SECTORS; ++i)
        CHECK(lume_sd_write(16 + i, data + i * 512));
    unsigned single_commands = data_commands + status_commands;
    CHECK(single_commands == 64 && writes == 32);
    setup(false);
    CHECK(lume_sd_write_blocks(16, LUME_SD_MAX_SECTORS, data));
    CHECK(data_commands + status_commands == 2 && auto_stops == 1 && writes == 32);
    printf("SDHCI 32-sector write commands: single=%u grouped=%u automatic-stop=%u\n",
           single_commands, data_commands + status_commands, auto_stops);
}

int main(void)
{
    success(false);
    success(true);
    boundaries();
    failures(false, false);
    failures(true, false);
    failures(false, true);
    failures(true, true);
    command_counts();
    printf("PASS SDHCI PIO groups: cases=%u checks=%u\n", cases, checks);
    return 0;
}
