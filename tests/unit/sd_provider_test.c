#include "lume/platform_driver.h"
#include "lume/sd.h"
#include "lume/modules.h"
#include "lume/activity.h"
#include "lume/abi.h"
#include "lume/early.h"
#include "lume/console_output.h"
#include <stdio.h>
#include <stdlib.h>

static unsigned checks, pins, unpins, entries, leaves, read_groups, write_groups;
static bool succeed = true;
static unsigned char owner;

bool lume_boot_watchdog_boot(void)
{
    abort();
}

void lume_boot_watchdog_poll(void)
{
    abort();
}

void lume_boot_watchdog_ready(void)
{
    abort();
}

void lume_boot_watchdog_enter_call(void)
{
    abort();
}

void lume_boot_watchdog_progress(void)
{
    abort();
}

void lume_boot_watchdog_user_progress(uint64_t spsr)
{
    (void)spsr;
    abort();
}

void lume_boot_console_write(const char *data, size_t bytes)
{
    (void)data;
    (void)bytes;
    abort();
}

void lume_console_serial_wait(uint64_t ticket)
{
    (void)ticket;
    abort();
}

void lume_console_serial_unlock(uint64_t ticket)
{
    (void)ticket;
    abort();
}

void lume_putc(char c)
{
    (void)c;
    abort();
}

_Noreturn void lume_fail(const char *why)
{
    (void)why;
    abort();
}

#define CHECK(condition)                                                                           \
    do                                                                                             \
    {                                                                                              \
        ++checks;                                                                                  \
        if (!(condition))                                                                          \
        {                                                                                          \
            fprintf(stderr, "SD provider check failed at line %u\n", (unsigned)__LINE__);          \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

int lume_module_pin(uintptr_t callback, void **result)
{
    CHECK(callback != 0);
    ++pins;
    *result = &owner;
    return 0;
}

void lume_module_unpin(void *value)
{
    CHECK(value == &owner);
    ++unpins;
}

unsigned lume_activity_enter(unsigned kind)
{
    CHECK(kind == LUME_ACTIVITY_STORAGE);
    ++entries;
    return 7;
}

void lume_activity_leave(unsigned previous)
{
    CHECK(previous == 7);
    ++leaves;
}

static bool start(uint64_t hz)
{
    return hz != 0;
}

static bool configure(uint64_t base, uint64_t size, uint32_t irq)
{
    return base && size && irq;
}

static bool stop(void)
{
    return true;
}

static bool read_one(uint64_t sector, void *data)
{
    return sector && data;
}

static bool write_one(uint64_t sector, const void *data)
{
    return sector && data;
}

static uint64_t sectors(void)
{
    return 128;
}

static bool allow(uint64_t first, uint64_t count)
{
    return first && count;
}

static void stats(void)
{
}

static bool read_many(uint64_t sector, unsigned count, void *data)
{
    CHECK(sector == 16 && count == 32 && data && entries == leaves + 1);
    ++read_groups;
    return succeed;
}

static bool write_many(uint64_t sector, unsigned count, const void *data)
{
    CHECK(sector == 16 && count == 32 && data && entries == leaves + 1);
    ++write_groups;
    return succeed;
}

int main(void)
{
    const struct lume_platform_sd_ops operations = {
        .lume_sd_start = start,
        .lume_sd_configure = configure,
        .lume_sd_stop = stop,
        .lume_sd_read = read_one,
        .lume_sd_write = write_one,
        .lume_sd_sectors = sectors,
        .lume_sd_allow_volume = allow,
        .lume_sd_allow_boot_volume = allow,
        .lume_sd_stats = stats,
        .lume_sd_read_blocks = read_many,
        .lume_sd_write_blocks = write_many,
    };
    unsigned char data[32 * 512];
    CHECK(!lume_sd_read_blocks(16, 32, data));
    CHECK(!lume_sd_write_blocks(16, 32, data));
    CHECK(!entries && !pins);
    CHECK(lume_platform_driver_register(LUME_PLATFORM_SD, &operations,
                                        sizeof(operations) - 2 * sizeof(void *)) == -LUME_EINVAL);
    CHECK(!pins);
    CHECK(lume_platform_driver_register(LUME_PLATFORM_SD, &operations, sizeof(operations)) == 0);
    CHECK(pins == 11 && unpins == 10);
    CHECK(lume_sd_read_blocks(16, 32, data));
    CHECK(lume_sd_write_blocks(16, 32, data));
    succeed = false;
    CHECK(!lume_sd_read_blocks(16, 32, data));
    CHECK(!lume_sd_write_blocks(16, 32, data));
    CHECK(entries == 4 && leaves == entries && read_groups == 2 && write_groups == 2);
    CHECK(lume_platform_driver_unregister(LUME_PLATFORM_SD, &operations) == 0);
    CHECK(unpins == pins);
    CHECK(!lume_sd_read_blocks(16, 32, data));
    CHECK(!lume_sd_write_blocks(16, 32, data));
    CHECK(entries == 4 && leaves == 4);
    printf("PASS SD provider group dispatch: checks=%u\n", checks);
    return 0;
}
