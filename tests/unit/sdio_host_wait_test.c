#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LUME_COUNTER_H
#define LUME_MMIO_H
#define LUME_ARCH_CPU_H

static uint64_t ticks, ready_at, last_pause;
static uint32_t ready_status, acknowledged, error_interrupt;
static unsigned pauses, attempts, reads, worker;
static bool cooperative;
static void check_reservation(void);

static uint64_t lume_counter_read(void)
{
    return ticks++;
}

static void lume_counter_delay_us(uint64_t frequency, unsigned us)
{
    ticks += frequency * us / 1000000;
}

static void lume_cpu_relax_ordered(void)
{
    ticks += 100;
}

static uint32_t lume_mmio_read32_relaxed(uintptr_t address)
{
    (void)address;
    ++reads;
    return ticks >= ready_at ? ready_status : 0;
}

static void lume_mmio_write32_sync(uintptr_t address, uint32_t value)
{
    (void)address;
    acknowledged |= value;
}

#include "../../drivers/storage/bcm2712_sdio/host.c"

static void check_reservation(void)
{
    assert(reservation_depth == 1 && reservation_owner == worker);
    unsigned saved = worker;
    worker = saved + 1;
    assert(!lume_wifi_claim());
    worker = saved;
    assert(reservation_depth == 1 && reservation_owner == worker);
}

bool lume_worker_pause(uint64_t deadline)
{
    ++attempts;
    check_reservation();
    if (!cooperative)
        return false;
    assert(deadline >= ticks - 1 && deadline - ticks <= 1000 && deadline <= 500000);
    assert(deadline >= last_pause);
    last_pause = deadline;
    ticks = deadline;
    ++pauses;
    return true;
}

unsigned lume_worker_current(void)
{
    return worker;
}

void lume_value(const char *name, uint64_t value)
{
    if (!strcmp(name, "WIFI_ERROR_INTERRUPT"))
        error_interrupt = (uint32_t)value;
}

void lume_puts(const char *text)
{
    (void)text;
}

_Noreturn void lume_fail(const char *reason)
{
    fprintf(stderr, "%s\n", reason);
    abort();
}

bool lume_sdio_identify(const struct lume_sdio_bus *bus, struct lume_sdio_identity *identity)
{
    (void)bus;
    (void)identity;
    return false;
}

static void reset(uint64_t ready, uint32_t status, bool use_worker)
{
    frequency = 1000000;
    ticks = last_pause = errors = 0;
    ready_at = ready;
    ready_status = status;
    acknowledged = error_interrupt = pauses = attempts = reads = 0;
    worker = use_worker ? 1 : LUME_WORKER_LIMIT;
    cooperative = use_worker;
    assert(!reservation_depth && lume_wifi_claim());
}

static void finish(bool success)
{
    assert(errors == (success ? 0u : 1u));
    assert(reads && ticks <= 500104);
    check_reservation();
    lume_wifi_release();
    assert(!reservation_depth);
}

int main(void)
{
    const uint64_t boundaries[] = {498000, 499900, 500000};
    for (unsigned i = 0; i < sizeof(boundaries) / sizeof(boundaries[0]); ++i)
    {
        reset(boundaries[i], 2, true);
        assert(wait_register(0x2c, 2, 2, 500));
        assert(pauses && !acknowledged);
        finish(true);
        reset(boundaries[i], 1, true);
        assert(event(1, 500));
        assert(pauses && acknowledged == 1);
        finish(true);
    }
    reset(500001, 2, true);
    assert(!wait_register(0x2c, 2, 2, 500) && !acknowledged && last_pause == 500000);
    finish(false);
    reset(UINT64_MAX, 1, true);
    assert(!event(1, 500) && !acknowledged && !error_interrupt && last_pause == 500000);
    finish(false);
    reset(499900, 0x8001, true);
    assert(!event(1, 500) && !acknowledged && error_interrupt == 0x8001);
    finish(false);
    reset(1000, 2, false);
    assert(wait_register(0x2c, 2, 2, 500) && attempts && !pauses);
    finish(true);
    reset(UINT64_MAX, 1, false);
    assert(!event(1, 500) && attempts && !pauses);
    finish(false);
    puts("PASS SDIO final status observation, absolute deadline, error priority and reservation");
    return 0;
}
