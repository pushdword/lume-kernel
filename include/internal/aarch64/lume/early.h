#ifndef LUME_EARLY_H
#define LUME_EARLY_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct lume_fdt {
    uint32_t size;
    bool pi5;
    bool psci_smc;
    char model[96];
    unsigned ram_count, reserved_count;
    struct lume_range { uint64_t base, size; } ram[8], reserved[32];
    bool gic400, physical_timer, bcm2712d0;
    bool wifi_mac_valid;
    unsigned char wifi_mac[6];
    uint64_t initrd_start, initrd_end;
};
bool lume_fdt_read(const void *blob, size_t available, struct lume_fdt *out);
void lume_putc(char c);
void lume_puts(const char *s);
void lume_hex(uint64_t value);
void lume_value(const char *name, uint64_t value);
void lume_uart_flush(void);
void lume_uart_input_stats(void);
uintptr_t lume_uart_base(void);
_Noreturn void lume_stop(void);
_Noreturn void lume_main(uint64_t dtb, uint64_t entry_el);
struct lume_context {
    uint64_t x[31], sp_el0, elr, spsr, tpidr, unused;
};
_Static_assert(sizeof(struct lume_context) == 288, "AArch64 vector ABI");
struct lume_context *lume_exception(struct lume_context *, uint64_t vector);
uint64_t lume_psci_call(uint64_t function);
_Noreturn void lume_fail(const char *why);
bool lume_mmu_start(const struct lume_fdt *);
bool lume_mmu_check(void);
uint64_t lume_mmu_root(void);
void lume_mmu_switch(uint64_t root);
bool lume_mmu_user_root(unsigned task, uint64_t data, const uint64_t stack[4]);
uint64_t lume_mmu_task_root(unsigned task);
bool lume_user_buffer(uint64_t pointer, uint64_t bytes, bool write);
void lume_pages_start(const struct lume_fdt *, uint64_t dtb, uint64_t image_start, uint64_t image_end);
uint64_t lume_page_alloc(uint64_t minimum);
bool lume_page_free(uint64_t page);
uint64_t lume_pages_free(void);
bool lume_pages_check(void);
void lume_timer_start(uint64_t frequency);
void lume_timer_stop(void);
unsigned lume_irq_ack(void);
void lume_irq_eoi(unsigned);
void lume_timer_rearm(void);
bool lume_timer_check(uint64_t frequency);
bool lume_tasks_check(uint64_t frequency);
bool lume_elf_tasks_check(uint64_t frequency);
bool lume_shell_start(uint64_t frequency, bool script);
struct lume_context *lume_tasks_irq(struct lume_context *);
struct lume_context *lume_tasks_sync(struct lume_context *, uint64_t esr, uint64_t far);
bool lume_tasks_active(void);
void lume_run_user(struct lume_context *);
extern unsigned char __user_code_start[], __user_code_end[];
#define LUME_USER_BASE UINT64_C(0x4000000000)
#define LUME_USER_DATA (LUME_USER_BASE + 0x10000)
#define LUME_USER_STACK (LUME_USER_BASE + 0x20000)

static inline uint64_t lume_current_el(void)
{
    uint64_t value;
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(value));
    return value >> 2;
}

/* Monotonic platform clock shared by protocols and drivers. */
uint32_t lume_clock_millis(void);
uint64_t lume_clock_monotonic(void);
bool lume_entropy(void *,size_t);
#endif
