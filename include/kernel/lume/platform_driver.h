#ifndef LUME_PLATFORM_DRIVER_H
#define LUME_PLATFORM_DRIVER_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum lume_platform_driver_kind
{
    LUME_PLATFORM_IRQ = 0,
    LUME_PLATFORM_TIMER = 1,
    LUME_PLATFORM_WATCHDOG = 2,
    LUME_PLATFORM_DMA = 3,
    LUME_PLATFORM_UART = 4,
    LUME_PLATFORM_SD = 5,
    LUME_PLATFORM_USB = 6,
    LUME_PLATFORM_DISPLAY = 7,
    LUME_PLATFORM_ENTROPY = 8,
    LUME_PLATFORM_WIFI = 9,
    LUME_PLATFORM_KEYBOARD = 10,
    LUME_PLATFORM_GRAPHICS = 11,
    LUME_PLATFORM_DRIVER_COUNT
};

struct lume_platform_irq_ops
{
    bool (*lume_pi5_irq_start)(void);
    unsigned (*lume_irq_ack)(void);
    void (*lume_irq_eoi)(unsigned token);
    void (*lume_pi5_irq_secondary)(void);
    void (*lume_pi5_irq_notify)(void);
};

struct lume_platform_timer_ops
{
    void (*lume_timer_start)(uint64_t frequency);
    void (*lume_timer_stop)(void);
    void (*lume_timer_rearm)(void);
    void (*lume_timer_idle)(bool idle);
    bool (*lume_timer_check)(uint64_t frequency);
};

struct lume_platform_watchdog_ops
{
    bool (*lume_watchdog_boot)(void);
    void (*lume_watchdog_poll)(void);
    void (*lume_watchdog_ready)(void);
    void (*lume_watchdog_enter_call)(void);
    void (*lume_watchdog_progress)(void);
    void (*lume_watchdog_user_progress)(uint64_t spsr);
};

struct lume_platform_dma_ops
{
    bool (*lume_pi5_dma_start)(void);
};

struct lume_platform_uart_ops
{
    int (*lume_console_getc)(void);
    bool (*lume_console_ready)(void);
    void (*lume_console_putc)(char c);
    void (*lume_console_write)(const char *data, size_t bytes);
    void (*lume_uart_poll_input)(void);
    void (*lume_uart_input_stats)(void);
    /* Rendering holds the kernel lock. Transmission receives encoded kernel bytes
     * with IRQs masked and the kernel lock released. It must only access TX state. */
    void (*lume_console_render)(const char *data, size_t bytes);
    void (*lume_console_transmit)(const char *data, size_t bytes);
};

struct lume_platform_sd_ops
{
    bool (*lume_sd_start)(uint64_t frequency);
    bool (*lume_sd_configure)(uint64_t base, uint64_t size, uint32_t irq);
    bool (*lume_sd_stop)(void);
    bool (*lume_sd_read)(uint64_t sector, void *data);
    bool (*lume_sd_write)(uint64_t sector, const void *data);
    uint64_t (*lume_sd_sectors)(void);
    bool (*lume_sd_allow_volume)(uint64_t first, uint64_t count);
    bool (*lume_sd_allow_boot_volume)(uint64_t first, uint64_t count);
    void (*lume_sd_stats)(void);
    bool (*lume_sd_read_blocks)(uint64_t sector, unsigned count, void *data);
    bool (*lume_sd_write_blocks)(uint64_t sector, unsigned count, const void *data);
};

struct lume_platform_usb_ops
{
    bool (*lume_usb_start)(uint64_t frequency);
    void (*lume_usb_poll)(void);
    int (*lume_usb_getc)(void);
    bool (*lume_usb_stop)(void);
    unsigned (*lume_usb_keyboards)(void);
    void (*lume_usb_stats)(void);
};

struct lume_platform_display_ops
{
    bool (*lume_display_start)(uint64_t frequency);
    void (*lume_display_putc)(char c);
    void (*lume_display_write)(const char *data, size_t bytes);
    void (*lume_display_clear)(void);
    void (*lume_display_stop)(void);
    int64_t (*lume_display_video)(unsigned action, unsigned value, const unsigned char *font);
};

struct lume_platform_entropy_ops
{
    bool (*lume_entropy)(void *data, size_t bytes);
};

struct lume_graphics_request;

struct lume_platform_graphics_ops
{
    int (*query)(struct lume_graphics_request *);
    int (*claim)(bool);
    int (*present)(const struct lume_graphics_request *);
};

int lume_platform_graphics_query(struct lume_graphics_request *);
int lume_platform_graphics_claim(bool);
int lume_platform_graphics_present(const struct lume_graphics_request *);

struct lume_platform_wifi_ops
{
    bool (*lume_wifi_probe)(uint64_t frequency, bool d0);
    void (*lume_wifi_stop)(void);
    bool (*lume_wifi_firmware)(void);
    bool (*lume_pi5_wifi_register)(const unsigned char *mac);
    void (*lume_wifi_leave)(void);
};

struct lume_platform_keyboard_ops
{
    void (*poll)(void);
    int (*getc)(void);
};

void lume_keyboard_poll(void);
int lume_keyboard_getc(void);

/* Registered callbacks belong to one loaded module or the platform kernel text.
 * Registration pins module callbacks. Callers serialize lifecycle changes with the kernel
 * and quiesce hardware before removal. */
struct lume_fdt;
void lume_platform_firmware_configure(const struct lume_fdt *);
const struct lume_fdt *lume_platform_firmware(void);
int lume_platform_driver_register(unsigned kind, const void *operations, size_t bytes);
int lume_platform_driver_can_stop(unsigned kind, const void *operations);
int lume_platform_driver_unregister(unsigned kind, const void *operations);
bool lume_platform_driver_ready(unsigned kind);
bool lume_boot_watchdog_boot(void);
void lume_boot_watchdog_poll(void);
void lume_boot_watchdog_ready(void);
void lume_boot_watchdog_enter_call(void);
void lume_boot_watchdog_progress(void);
void lume_boot_watchdog_user_progress(uint64_t);
void lume_boot_console_write(const char *, size_t);
#endif
