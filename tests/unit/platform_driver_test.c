#include "lume/platform_driver.h"
#include "lume/display.h"
#include "lume/video_control.h"
#include "lume/early.h"
#include "lume/sd.h"
#include "lume/vfs.h"
#include "lume/abi.h"
#include "lume/console_output.h"
#include "lume/activity.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned references, other_references, pin_calls, fail_pin, received, fallback;
static int owner, other_owner;
static bool ready_other(void);

unsigned lume_activity_enter(unsigned kind)
{
    assert(kind < LUME_ACTIVITY_COUNT);
    return UINT32_MAX;
}

void lume_activity_leave(unsigned previous)
{
    assert(previous == UINT32_MAX);
}

_Noreturn void lume_fail(const char *why)
{
    fprintf(stderr, "%s\n", why);
    assert(false);
    for (;;)
    {
    }
}

void lume_console_serial_wait(uint64_t ticket)
{
    assert(ticket == 17);
}

void lume_console_serial_unlock(uint64_t ticket)
{
    assert(ticket == 17);
}

int lume_module_pin(uintptr_t callback, void **out)
{
    assert(callback);
    if (++pin_calls == fail_pin)
        return -LUME_ENOENT;
    if (callback == (uintptr_t)ready_other)
    {
        *out = &other_owner;
        ++other_references;
    }
    else
    {
        *out = &owner;
        ++references;
    }
    return 0;
}

void lume_module_unpin(void *value)
{
    if (!value)
        return;
    if (value == &other_owner)
    {
        assert(other_references);
        --other_references;
    }
    else
    {
        assert(value == &owner && references);
        --references;
    }
}

void lume_putc(char c)
{
    (void)c;
    ++fallback;
}

void lume_boot_console_write(const char *p, size_t n)
{
    (void)p;
    fallback += (unsigned)n;
}

bool lume_boot_watchdog_boot(void)
{
    ++fallback;
    return true;
}

void lume_boot_watchdog_poll(void)
{
    ++fallback;
}

void lume_boot_watchdog_ready(void)
{
    ++fallback;
}

void lume_boot_watchdog_enter_call(void)
{
    ++fallback;
}

void lume_boot_watchdog_progress(void)
{
    ++fallback;
}

void lume_boot_watchdog_user_progress(uint64_t s)
{
    (void)s;
    ++fallback;
}

static bool entropy(void *p, size_t n)
{
    memset(p, 0x5a, n);
    return true;
}

static bool entropy_other(void *p, size_t n)
{
    (void)p;
    (void)n;
    return false;
}

static int getc_driver(void)
{
    ++received;
    return 42;
}

static bool ready(void)
{
    return true;
}

static bool ready_other(void)
{
    return true;
}

static void putc_driver(char c)
{
    received += (unsigned char)c;
}

static void write_driver(const char *p, size_t n)
{
    (void)p;
    received += (unsigned)n;
}

static void poll_driver(void)
{
    ++received;
}

static bool start(uint64_t f)
{
    return f != 0;
}

static bool configure(uint64_t b, uint64_t s, uint32_t i)
{
    return b && s && i;
}

static bool stop(void)
{
    return true;
}

static bool read_sector(uint64_t s, void *p)
{
    (void)s;
    (void)p;
    return true;
}

static bool write_sector(uint64_t s, const void *p)
{
    (void)s;
    (void)p;
    return true;
}

static bool read_blocks(uint64_t s, unsigned n, void *p)
{
    return n && read_sector(s, p);
}

static bool write_blocks(uint64_t s, unsigned n, const void *p)
{
    return n && write_sector(s, p);
}

static uint64_t sectors(void)
{
    return 8192;
}

static bool allow(uint64_t f, uint64_t n)
{
    return f && n;
}

static int64_t video_driver(unsigned action, unsigned value, const unsigned char *font)
{
    assert(!value && !font);
    return action == LUME_VIDEO_CELLS ? 100 | 37u << 16 : -LUME_ENOSYS;
}

int main(void)
{
    assert(lume_keyboard_getc() == -LUME_EAGAIN);
    lume_keyboard_poll();
    const struct lume_platform_keyboard_ops keyboard = {poll_driver, getc_driver};
    assert(!lume_platform_driver_register(LUME_PLATFORM_KEYBOARD, &keyboard, sizeof(keyboard)));
    assert(references == 1);
    lume_keyboard_poll();
    assert(lume_keyboard_getc() == 42 && received == 2);
    assert(!lume_platform_driver_unregister(LUME_PLATFORM_KEYBOARD, &keyboard));
    assert(!references && lume_keyboard_getc() == -LUME_EAGAIN);
    received = 0;
    unsigned char data[4];
    assert(!lume_platform_driver_ready(LUME_PLATFORM_ENTROPY));
    assert(!lume_entropy(data, sizeof(data)));
    lume_console_write("boot", 4);
    assert(fallback == 4);
    struct lume_platform_entropy_ops random = {entropy};
    assert(lume_platform_driver_register(99, &random, sizeof(random)) == -LUME_EINVAL);
    assert(lume_platform_driver_register(LUME_PLATFORM_ENTROPY, &random, 1) == -LUME_EINVAL);
    assert(!lume_platform_driver_register(LUME_PLATFORM_ENTROPY, &random, sizeof(random)));
    assert(references == 1);
    random.lume_entropy = entropy_other;
    assert(lume_entropy(data, sizeof(data)) && data[0] == 0x5a);
    assert(!lume_platform_driver_unregister(LUME_PLATFORM_ENTROPY, &random));
    assert(!references && !lume_entropy(data, sizeof(data)));
    struct lume_platform_uart_ops uart = {getc_driver, ready,       putc_driver,  write_driver,
                                          poll_driver, poll_driver, write_driver, write_driver};
    for (unsigned i = 1; i <= 8; ++i)
    {
        fail_pin = pin_calls + i;
        assert(lume_platform_driver_register(LUME_PLATFORM_UART, &uart, sizeof(uart)) ==
               -LUME_ENOENT);
        assert(!references && !other_references);
    }
    fail_pin = 0;
    uart.lume_console_ready = ready_other;
    assert(lume_platform_driver_register(LUME_PLATFORM_UART, &uart, sizeof(uart)) == -LUME_EINVAL);
    assert(!references && !other_references);
    uart.lume_console_ready = ready;
    assert(!lume_platform_driver_register(LUME_PLATFORM_UART, &uart, sizeof(uart)));
    assert(!lume_platform_driver_register(LUME_PLATFORM_UART, &uart, sizeof(uart)));
    assert(references == 1 && lume_console_getc() == 42 && received == 1);
    struct lume_platform_uart_ops replacement = uart;
    assert(lume_platform_driver_register(LUME_PLATFORM_UART, &replacement, sizeof(replacement)) ==
           -LUME_EBUSY);
    assert(lume_platform_driver_unregister(LUME_PLATFORM_UART, &replacement) == -LUME_ENOENT);
    assert(!lume_platform_driver_unregister(LUME_PLATFORM_UART, &uart));
    assert(lume_platform_driver_ready(LUME_PLATFORM_UART) && references == 1);
    struct lume_console_output output;
    char message[] = "a\nb\n";
    lume_console_output_begin(&output);
    lume_console_write(message, sizeof(message) - 1);
    lume_console_output_end(&output);
    assert(output.bytes == 6 && !memcmp(output.data, "a\r\nb\r\n", 6));
    assert(output.provider && output.transmit == write_driver);
    memset(message, 'X', sizeof(message));
    assert(!memcmp(output.data, "a\r\nb\r\n", 6));
    assert(lume_platform_driver_can_stop(LUME_PLATFORM_UART, &uart) == -LUME_EBUSY);
    assert(lume_platform_driver_unregister(LUME_PLATFORM_UART, &uart) == -LUME_EBUSY);
    unsigned before = received;
    output.ticket = 17;
    lume_console_output_send(&output);
    assert(received == before + 6 && references == 1);
    lume_console_output_finish(&output);
    assert(!output.provider);

    struct
    {
        uint64_t before;
        struct lume_console_output output;
        uint64_t after;
    } guarded = {.before = UINT64_C(0x123456789abcdef0), .after = UINT64_C(0xfedcba9876543210)};

    char newlines[LUME_CONSOLE_OUTPUT_BYTES];
    memset(newlines, '\n', sizeof(newlines));
    lume_console_output_begin(&guarded.output);
    lume_console_write(newlines, sizeof(newlines));
    lume_console_output_end(&guarded.output);
    assert(guarded.before == UINT64_C(0x123456789abcdef0));
    assert(guarded.after == UINT64_C(0xfedcba9876543210));
    assert(guarded.output.bytes == 128);
    for (unsigned i = 0; i < 128; i += 2)
        assert(guarded.output.data[i] == '\r' && guarded.output.data[i + 1] == '\n');
    lume_console_output_finish(&guarded.output);
    assert(!lume_platform_driver_unregister(LUME_PLATFORM_UART, &uart));
    assert(!references && !lume_platform_driver_ready(LUME_PLATFORM_UART));
    struct lume_platform_display_ops display = {start,       putc_driver, write_driver,
                                                poll_driver, poll_driver, video_driver};
    assert(lume_display_video(LUME_VIDEO_CELLS, 0, NULL) == -LUME_ENOSYS);
    assert(!lume_platform_driver_register(LUME_PLATFORM_DISPLAY, &display, sizeof(display)));
    assert(references == 1);
    display.lume_display_video = NULL;
    assert(lume_display_geometry() == (100 | 37u << 16));
    assert(!lume_platform_driver_unregister(LUME_PLATFORM_DISPLAY, &display));
    assert(!references && lume_display_geometry() == (80 | 25u << 16));
    const struct lume_platform_sd_ops sd = {start,        configure,   stop,        read_sector,
                                            write_sector, sectors,     allow,       allow,
                                            poll_driver,  read_blocks, write_blocks};
    assert(!lume_platform_driver_register(LUME_PLATFORM_SD, &sd, sizeof(sd)));
    assert(lume_sd_allow_volume(2048, 4096));
    assert(lume_platform_driver_can_stop(LUME_PLATFORM_SD, &sd) == -LUME_EBUSY);
    assert(lume_platform_driver_unregister(LUME_PLATFORM_SD, &sd) == -LUME_EBUSY);
    assert(references == 1 && lume_sd_sectors() == 8192);
    puts(
        "PLATFORM_DRIVER_PASS ownership, callback copies, shared binds, rollback and mounted storage");
    return 0;
}
