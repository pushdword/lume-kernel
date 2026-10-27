#include "lume/platform_driver.h"
#include "lume/modules.h"
#include "lume/abi.h"
#include "lume/early.h"
#include "lume/dma.h"
#include "lume/usb.h"
#include "lume/console_output.h"
#include <string.h>

static struct lume_fdt firmware;
static bool firmware_ready;
static int pending_keyboard = -1;

void lume_platform_firmware_configure(const struct lume_fdt *description)
{
    firmware = *description;
    firmware_ready = true;
}

const struct lume_fdt *lume_platform_firmware(void)
{
    return firmware_ready ? &firmware : NULL;
}

union operations
{
    struct lume_platform_irq_ops irq;
    struct lume_platform_timer_ops timer;
    struct lume_platform_watchdog_ops watchdog;
    struct lume_platform_dma_ops dma;
    struct lume_platform_uart_ops uart;
    struct lume_platform_sd_ops sd;
    struct lume_platform_usb_ops usb;
    struct lume_platform_display_ops display;
    struct lume_platform_entropy_ops entropy;
    struct lume_platform_wifi_ops wifi;
    struct lume_platform_keyboard_ops keyboard;
};

static struct provider
{
    const void *source;
    void *owner;
    union operations operations;
    bool mounted;
    unsigned bindings;
    unsigned active;
} providers[LUME_PLATFORM_DRIVER_COUNT];

static struct lume_console_output *console_capture;

static size_t operation_bytes(unsigned kind)
{
    switch (kind)
    {
    case LUME_PLATFORM_IRQ:
        return sizeof(struct lume_platform_irq_ops);
    case LUME_PLATFORM_TIMER:
        return sizeof(struct lume_platform_timer_ops);
    case LUME_PLATFORM_WATCHDOG:
        return sizeof(struct lume_platform_watchdog_ops);
    case LUME_PLATFORM_DMA:
        return sizeof(struct lume_platform_dma_ops);
    case LUME_PLATFORM_UART:
        return sizeof(struct lume_platform_uart_ops);
    case LUME_PLATFORM_SD:
        return sizeof(struct lume_platform_sd_ops);
    case LUME_PLATFORM_USB:
        return sizeof(struct lume_platform_usb_ops);
    case LUME_PLATFORM_DISPLAY:
        return sizeof(struct lume_platform_display_ops);
    case LUME_PLATFORM_ENTROPY:
        return sizeof(struct lume_platform_entropy_ops);
    case LUME_PLATFORM_WIFI:
        return sizeof(struct lume_platform_wifi_ops);
    case LUME_PLATFORM_KEYBOARD:
        return sizeof(struct lume_platform_keyboard_ops);
    default:
        return 0;
    }
}

int lume_platform_driver_register(unsigned kind, const void *operations, size_t bytes)
{
    if (!operations || !operation_bytes(kind) || bytes != operation_bytes(kind))
        return -LUME_EINVAL;
    struct provider *provider = providers + kind;
    if (provider->source)
    {
        if (provider->source != operations || provider->bindings == UINT32_MAX)
            return -LUME_EBUSY;
        ++provider->bindings;
        return 0;
    }
    void *owner = NULL;
    for (size_t offset = 0; offset < bytes; offset += sizeof(uintptr_t))
    {
        uintptr_t callback;
        void *current = NULL;
        memcpy(&callback, (const unsigned char *)operations + offset, sizeof(callback));
        int result = callback ? lume_module_pin(callback, &current) : -LUME_EINVAL;
#if defined(LUME_ARCH_AMD64)
        extern unsigned char __image_start[], __text_end[];
        if (result == -LUME_ENOENT && callback >= (uintptr_t)__image_start &&
            callback < (uintptr_t)__text_end)
            result = 0;
#endif
        if (result || (owner && current != owner))
        {
            lume_module_unpin(current);
            lume_module_unpin(owner);
            return result ? result : -LUME_EINVAL;
        }
        if (owner)
            lume_module_unpin(current);
        else
            owner = current;
    }
    *provider = (struct provider){.source = operations, .owner = owner, .bindings = 1};
    memcpy(&provider->operations, operations, bytes);
    return 0;
}

bool lume_platform_driver_ready(unsigned kind)
{
    return kind < LUME_PLATFORM_DRIVER_COUNT && providers[kind].source;
}

void lume_keyboard_poll(void)
{
    if (providers[LUME_PLATFORM_KEYBOARD].source)
        providers[LUME_PLATFORM_KEYBOARD].operations.keyboard.poll();
}

int lume_keyboard_getc(void)
{
    return providers[LUME_PLATFORM_KEYBOARD].source
               ? providers[LUME_PLATFORM_KEYBOARD].operations.keyboard.getc()
               : -LUME_EAGAIN;
}

int lume_platform_driver_can_stop(unsigned kind, const void *operations)
{
    if (kind >= LUME_PLATFORM_DRIVER_COUNT || !operations)
        return -LUME_EINVAL;
    const struct provider *provider = providers + kind;
    if (provider->source != operations)
        return -LUME_ENOENT;
    /* Active CPU services and mounted storage outlive an individual device operation. */
    if (provider->active || provider->mounted || kind == LUME_PLATFORM_IRQ ||
        kind == LUME_PLATFORM_TIMER || kind == LUME_PLATFORM_WATCHDOG || kind == LUME_PLATFORM_DMA)
        return -LUME_EBUSY;
    return 0;
}

int lume_platform_driver_unregister(unsigned kind, const void *operations)
{
    int result = lume_platform_driver_can_stop(kind, operations);
    if (result)
        return result;
    if (--providers[kind].bindings)
        return 0;
    void *owner = providers[kind].owner;
    providers[kind] = (struct provider){0};
    lume_module_unpin(owner);
    return 0;
}

const struct lume_dma_domain lume_rp1_dma_domain = {0, UINT64_C(0x1000000000),
                                                    UINT64_C(0x200000000), UINT64_C(0xffffffffff)};

bool lume_pi5_irq_start(void)
{
    struct provider *provider = providers + LUME_PLATFORM_IRQ;
    if (!provider->source)
        return false;
    return provider->operations.irq.lume_pi5_irq_start();
}

unsigned lume_irq_ack(void)
{
    struct provider *provider = providers + LUME_PLATFORM_IRQ;
    if (!provider->source)
        return 1023;
    return provider->operations.irq.lume_irq_ack();
}

void lume_irq_eoi(unsigned token)
{
    struct provider *provider = providers + LUME_PLATFORM_IRQ;
    if (!provider->source)
    {
        return;
    }
    provider->operations.irq.lume_irq_eoi(token);
}

void lume_pi5_irq_secondary(void)
{
    struct provider *provider = providers + LUME_PLATFORM_IRQ;
    if (!provider->source)
    {
        return;
    }
    provider->operations.irq.lume_pi5_irq_secondary();
}

void lume_timer_start(uint64_t frequency)
{
    struct provider *provider = providers + LUME_PLATFORM_TIMER;
    if (!provider->source)
    {
        return;
    }
    provider->operations.timer.lume_timer_start(frequency);
}

void lume_timer_stop(void)
{
    struct provider *provider = providers + LUME_PLATFORM_TIMER;
    if (!provider->source)
    {
        return;
    }
    provider->operations.timer.lume_timer_stop();
}

void lume_timer_rearm(void)
{
    struct provider *provider = providers + LUME_PLATFORM_TIMER;
    if (!provider->source)
    {
        return;
    }
    provider->operations.timer.lume_timer_rearm();
}

void lume_timer_idle(bool idle)
{
    struct provider *provider = providers + LUME_PLATFORM_TIMER;
    if (!provider->source)
    {
        return;
    }
    provider->operations.timer.lume_timer_idle(idle);
}

bool lume_timer_check(uint64_t frequency)
{
    struct provider *provider = providers + LUME_PLATFORM_TIMER;
    if (!provider->source)
        return false;
    return provider->operations.timer.lume_timer_check(frequency);
}

bool lume_watchdog_boot(void)
{
    struct provider *provider = providers + LUME_PLATFORM_WATCHDOG;
    if (!provider->source)
        return lume_boot_watchdog_boot();
    return provider->operations.watchdog.lume_watchdog_boot();
}

void lume_watchdog_poll(void)
{
    struct provider *provider = providers + LUME_PLATFORM_WATCHDOG;
    if (!provider->source)
    {
        lume_boot_watchdog_poll();
        return;
    }
    provider->operations.watchdog.lume_watchdog_poll();
}

void lume_watchdog_ready(void)
{
    struct provider *provider = providers + LUME_PLATFORM_WATCHDOG;
    if (!provider->source)
    {
        lume_boot_watchdog_ready();
        return;
    }
    provider->operations.watchdog.lume_watchdog_ready();
}

void lume_watchdog_enter_call(void)
{
    struct provider *provider = providers + LUME_PLATFORM_WATCHDOG;
    if (!provider->source)
    {
        lume_boot_watchdog_enter_call();
        return;
    }
    provider->operations.watchdog.lume_watchdog_enter_call();
}

void lume_watchdog_progress(void)
{
    struct provider *provider = providers + LUME_PLATFORM_WATCHDOG;
    if (!provider->source)
    {
        lume_boot_watchdog_progress();
        return;
    }
    provider->operations.watchdog.lume_watchdog_progress();
}

void lume_watchdog_user_progress(uint64_t spsr)
{
    struct provider *provider = providers + LUME_PLATFORM_WATCHDOG;
    if (!provider->source)
    {
        lume_boot_watchdog_user_progress(spsr);
        return;
    }
    provider->operations.watchdog.lume_watchdog_user_progress(spsr);
}

bool lume_pi5_dma_start(void)
{
    struct provider *provider = providers + LUME_PLATFORM_DMA;
    if (!provider->source)
        return false;
    return provider->operations.dma.lume_pi5_dma_start();
}

int lume_console_getc(void)
{
    if (pending_keyboard >= 0)
    {
        int value = pending_keyboard;
        pending_keyboard = -1;
        return value;
    }
    struct provider *provider = providers + LUME_PLATFORM_UART;
    if (!provider->source)
        return lume_usb_getc();
    return provider->operations.uart.lume_console_getc();
}

bool lume_console_ready(void)
{
    if (pending_keyboard >= 0)
        return true;
    struct provider *provider = providers + LUME_PLATFORM_UART;
    if (!provider->source)
    {
        pending_keyboard = lume_usb_getc();
        return pending_keyboard >= 0;
    }
    return provider->operations.uart.lume_console_ready();
}

void lume_console_putc(char c)
{
    struct provider *provider = providers + LUME_PLATFORM_UART;
    if (!provider->source)
    {
        lume_putc(c);
        return;
    }
    provider->operations.uart.lume_console_putc(c);
}

void lume_console_write(const char *data, size_t bytes)
{
    struct provider *provider = providers + LUME_PLATFORM_UART;
    struct lume_console_output *output = console_capture;
    if (output && provider->source)
    {
        if (output->provider || bytes > LUME_CONSOLE_OUTPUT_BYTES || provider->active == UINT32_MAX)
            lume_fail("console output capture bounds");
        if (!bytes)
            return;
        ++provider->active;
        output->provider = provider;
        output->transmit = provider->operations.uart.lume_console_transmit;
        memcpy(output->data, data, bytes);
        provider->operations.uart.lume_console_render(output->data, bytes);
        size_t encoded = bytes;
        for (size_t i = 0; i < bytes; ++i)
            encoded += output->data[i] == '\n';
        output->bytes = encoded;
        while (bytes)
        {
            char c = output->data[--bytes];
            output->data[--encoded] = c;
            if (c == '\n')
                output->data[--encoded] = '\r';
        }
        return;
    }
    if (!provider->source)
    {
        lume_boot_console_write(data, bytes);
        return;
    }
    provider->operations.uart.lume_console_write(data, bytes);
}

void lume_console_output_begin(struct lume_console_output *output)
{
    if (console_capture)
        lume_fail("nested console output capture");
    *output = (struct lume_console_output){0};
    console_capture = output;
}

void lume_console_output_end(struct lume_console_output *output)
{
    if (console_capture != output)
        lume_fail("console output capture ownership");
    console_capture = NULL;
}

void lume_console_output_send(const struct lume_console_output *output)
{
    lume_console_serial_wait(output->ticket);
    output->transmit(output->data, output->bytes);
    lume_console_serial_unlock(output->ticket);
}

void lume_console_output_finish(struct lume_console_output *output)
{
    struct provider *provider = output->provider;
    if (!provider || !provider->active)
        lume_fail("console output provider ownership");
    --provider->active;
    output->provider = NULL;
}

void lume_uart_poll_input(void)
{
    struct provider *provider = providers + LUME_PLATFORM_UART;
    if (!provider->source)
    {
        return;
    }
    provider->operations.uart.lume_uart_poll_input();
}

void lume_uart_input_stats(void)
{
    struct provider *provider = providers + LUME_PLATFORM_UART;
    if (!provider->source)
    {
        return;
    }
    provider->operations.uart.lume_uart_input_stats();
}

bool lume_sd_start(uint64_t frequency)
{
    struct provider *provider = providers + LUME_PLATFORM_SD;
    if (!provider->source)
        return false;
    return provider->operations.sd.lume_sd_start(frequency);
}

bool lume_sd_configure(uint64_t base, uint64_t size, uint32_t irq)
{
    struct provider *provider = providers + LUME_PLATFORM_SD;
    if (!provider->source)
        return false;
    return provider->operations.sd.lume_sd_configure(base, size, irq);
}

bool lume_sd_stop(void)
{
    struct provider *provider = providers + LUME_PLATFORM_SD;
    if (!provider->source)
        return true;
    return provider->operations.sd.lume_sd_stop();
}

bool lume_sd_read(uint64_t sector, void *data)
{
    struct provider *provider = providers + LUME_PLATFORM_SD;
    if (!provider->source)
        return false;
    return provider->operations.sd.lume_sd_read(sector, data);
}

bool lume_sd_write(uint64_t sector, const void *data)
{
    struct provider *provider = providers + LUME_PLATFORM_SD;
    if (!provider->source)
        return false;
    return provider->operations.sd.lume_sd_write(sector, data);
}

uint64_t lume_sd_sectors(void)
{
    struct provider *provider = providers + LUME_PLATFORM_SD;
    if (!provider->source)
        return 0;
    return provider->operations.sd.lume_sd_sectors();
}

bool lume_sd_allow_volume(uint64_t first, uint64_t count)
{
    struct provider *provider = providers + LUME_PLATFORM_SD;
    if (!provider->source)
        return false;
    bool result = provider->operations.sd.lume_sd_allow_volume(first, count);
    if (result)
        provider->mounted = true;
    return result;
}

bool lume_sd_allow_boot_volume(uint64_t first, uint64_t count)
{
    struct provider *provider = providers + LUME_PLATFORM_SD;
    if (!provider->source)
        return false;
    return provider->operations.sd.lume_sd_allow_boot_volume(first, count);
}

void lume_sd_stats(void)
{
    struct provider *provider = providers + LUME_PLATFORM_SD;
    if (!provider->source)
    {
        return;
    }
    provider->operations.sd.lume_sd_stats();
}

bool lume_usb_start(uint64_t frequency)
{
    struct provider *provider = providers + LUME_PLATFORM_USB;
    if (!provider->source)
        return false;
    return provider->operations.usb.lume_usb_start(frequency);
}

void lume_usb_poll(void)
{
    struct provider *provider = providers + LUME_PLATFORM_USB;
    if (!provider->source)
    {
        return;
    }
    provider->operations.usb.lume_usb_poll();
}

int lume_usb_getc(void)
{
    struct provider *provider = providers + LUME_PLATFORM_USB;
    if (!provider->source)
        return -LUME_EAGAIN;
    return provider->operations.usb.lume_usb_getc();
}

bool lume_usb_stop(void)
{
    struct provider *provider = providers + LUME_PLATFORM_USB;
    if (!provider->source)
        return true;
    return provider->operations.usb.lume_usb_stop();
}

unsigned lume_usb_keyboards(void)
{
    struct provider *provider = providers + LUME_PLATFORM_USB;
    if (!provider->source)
        return 0;
    return provider->operations.usb.lume_usb_keyboards();
}

void lume_usb_stats(void)
{
    struct provider *provider = providers + LUME_PLATFORM_USB;
    if (!provider->source)
    {
        return;
    }
    provider->operations.usb.lume_usb_stats();
}

bool lume_display_start(uint64_t frequency)
{
    struct provider *provider = providers + LUME_PLATFORM_DISPLAY;
    if (!provider->source)
        return false;
    return provider->operations.display.lume_display_start(frequency);
}

void lume_display_putc(char c)
{
    struct provider *provider = providers + LUME_PLATFORM_DISPLAY;
    if (!provider->source)
    {
        return;
    }
    provider->operations.display.lume_display_putc(c);
}

void lume_display_write(const char *data, size_t bytes)
{
    struct provider *provider = providers + LUME_PLATFORM_DISPLAY;
    if (!provider->source)
    {
        return;
    }
    provider->operations.display.lume_display_write(data, bytes);
}

void lume_display_clear(void)
{
    struct provider *provider = providers + LUME_PLATFORM_DISPLAY;
    if (!provider->source)
    {
        return;
    }
    provider->operations.display.lume_display_clear();
}

void lume_display_stop(void)
{
    struct provider *provider = providers + LUME_PLATFORM_DISPLAY;
    if (!provider->source)
    {
        return;
    }
    provider->operations.display.lume_display_stop();
}

bool lume_entropy(void *data, size_t bytes)
{
    struct provider *provider = providers + LUME_PLATFORM_ENTROPY;
    if (!provider->source)
        return false;
    return provider->operations.entropy.lume_entropy(data, bytes);
}

bool lume_wifi_probe(uint64_t frequency, bool d0)
{
    struct provider *provider = providers + LUME_PLATFORM_WIFI;
    if (!provider->source)
        return false;
    return provider->operations.wifi.lume_wifi_probe(frequency, d0);
}

void lume_wifi_stop(void)
{
    struct provider *provider = providers + LUME_PLATFORM_WIFI;
    if (!provider->source)
    {
        return;
    }
    provider->operations.wifi.lume_wifi_stop();
}

bool lume_wifi_firmware(void)
{
    struct provider *provider = providers + LUME_PLATFORM_WIFI;
    if (!provider->source)
        return false;
    return provider->operations.wifi.lume_wifi_firmware();
}

bool lume_pi5_wifi_register(const unsigned char *mac)
{
    struct provider *provider = providers + LUME_PLATFORM_WIFI;
    if (!provider->source)
        return false;
    return provider->operations.wifi.lume_pi5_wifi_register(mac);
}

void lume_wifi_leave(void)
{
    struct provider *provider = providers + LUME_PLATFORM_WIFI;
    if (!provider->source)
    {
        return;
    }
    provider->operations.wifi.lume_wifi_leave();
}

void lume_pi5_irq_notify(void)
{
    struct provider *provider = providers + LUME_PLATFORM_IRQ;
    if (provider->source)
        provider->operations.irq.lume_pi5_irq_notify();
}
