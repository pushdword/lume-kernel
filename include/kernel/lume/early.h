#ifndef LUME_EARLY_H
#define LUME_EARLY_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct lume_fdt
{
    uint32_t size;
    bool pi5;
    bool psci_smc;
    char model[96];
    unsigned ram_count, reserved_count;

    struct lume_range
    {
        uint64_t base, size;
    } ram[8], reserved[32];

    bool gic400, physical_timer, bcm2712d0;
    bool wifi_mac_valid;
    unsigned char wifi_mac[6];
    uint64_t initrd_start, initrd_end;
    uint64_t sdhci_base, sdhci_size;
    uint32_t sdhci_irq;
    bool sdhci_valid;
    unsigned cpu_count;
    uint64_t cpu_affinity[4];
};

bool lume_fdt_read(const void *blob, size_t available, struct lume_fdt *out);
bool lume_fdt_set_initrd(void *blob, size_t available, uint64_t start, uint64_t end);
void lume_putc(char c);
void lume_puts(const char *s);
void lume_hex(uint64_t value);
void lume_value(const char *name, uint64_t value);
void lume_uart_flush(void);
void lume_uart_input_stats(void);
void lume_uart_poll_input(void);
uintptr_t lume_uart_base(void);
_Noreturn void lume_stop(void);
_Noreturn void lume_main(uint64_t dtb, uint64_t entry_el);

#include "lume/context.h"

struct lume_context *lume_exception(struct lume_context *, uint64_t vector);
uint64_t lume_psci_call(uint64_t function);
_Noreturn void lume_fail(const char *why);
bool lume_mmu_start(const struct lume_fdt *);
bool lume_mmu_check(void);
uint64_t lume_mmu_root(void);
void lume_mmu_switch(uint64_t root);
bool lume_mmu_user_root(unsigned task, uint64_t data, const uint64_t *stack);
void lume_mmu_release_user(unsigned task);
uint64_t lume_mmu_task_root(unsigned task);
void lume_mmu_share_user(unsigned, unsigned);
bool lume_user_buffer(uint64_t pointer, uint64_t bytes, bool write);
void lume_pages_start(const struct lume_fdt *, uint64_t dtb, uint64_t image_start,
                      uint64_t image_end);
uint64_t lume_page_alloc(uint64_t minimum);
uint64_t lume_pages_alloc_span(unsigned count);
uint64_t lume_pages_total(void);
bool lume_page_free(uint64_t page);
uint64_t lume_pages_free(void);
unsigned lume_program_staging_pages(void);
bool lume_pages_check(void);
void lume_timer_start(uint64_t frequency);
void lume_timer_stop(void);
unsigned lume_irq_ack(void);
void lume_irq_eoi(unsigned);
void lume_timer_rearm(void);
void lume_timer_idle(bool);
bool lume_timer_check(uint64_t frequency);
bool lume_tasks_check(uint64_t frequency);
bool lume_elf_tasks_check(uint64_t frequency);
bool lume_shell_start(uint64_t frequency, bool script);
struct lume_context *lume_tasks_irq(struct lume_context *);
struct lume_context *lume_tasks_device_irq(struct lume_context *);
struct lume_context *lume_tasks_sync(struct lume_context *, uint64_t esr, uint64_t far);
bool lume_tasks_active(void);
bool lume_task_capable(unsigned pid, uint64_t capabilities);
uint64_t lume_task_capabilities(unsigned pid);
int lume_task_drop_capabilities(unsigned pid, uint64_t);
void lume_run_user(struct lume_context *);
extern unsigned char __user_code_start[], __user_code_end[];
#define LUME_USER_BASE UINT64_C(0x40000000)
#define LUME_USER_LIMIT UINT64_C(0x60000000)
#define LUME_USER_DATA (LUME_USER_BASE + 0x10000)
#define LUME_USER_STACK UINT64_C(0x5ff00000)
#define LUME_STACK_PAGES 256u
#define LUME_HEAP_BASE UINT64_C(0x48000000)
#define LUME_THREAD_BASE UINT64_C(0x5e000000)
#define LUME_HEAP_LIMIT (LUME_THREAD_BASE - 4096)
#define LUME_TASK_LIMIT 16u

struct lume_context *lume_tasks_deliver_signals(struct lume_context *);

static inline uint64_t lume_current_el(void)
{
#if defined(LUME_ARCH_AMD64) || defined(LUME_ARCH_I686)
    return 1;
#else
    uint64_t value;
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(value));
    return value >> 2;
#endif
}

/* Monotonic platform clock shared by protocols and drivers. */
uint32_t lume_clock_millis(void);
uint64_t lume_clock_monotonic(void);
bool lume_entropy(void *, size_t);
#endif
