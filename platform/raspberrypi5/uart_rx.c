#include "lume/uart_rx.h"
#include "lume/board.h"
#include "lume/mmio.h"
#include "lume/counter.h"
#include "lume/smp.h"
#include "lume/abi.h"
#include "lume/cpu.h"
#include "lume/console_output.h"
#include "lume/early.h"
#include <limits.h>

#define GICD LUME_BOARD_GIC_DISTRIBUTOR_BASE
#define GICC LUME_BOARD_GIC_CPU_BASE
#define INTC (LUME_BOARD_RP1_BASE + 0x108000)
#define UART_SOURCE 25u
#define UART_CONFIG (8u + 4u * UART_SOURCE)
#define RX_MASK ((1u << 4) | (1u << 6) | (15u << 7))

_Static_assert(LUME_BOARD_UART_IRQ_PRIORITY < LUME_BOARD_IRQ_PRIORITY,
               "raw receive priority must precede subsystem interrupts");

static uint32_t words[16384];
static unsigned head, tail, collecting;
static bool enabled;
static uint64_t dropped, checked, interval;
static bool irq_ready, lock_window;
static bool lock_requested;
static unsigned serial_scopes;
static unsigned irq_active;
static uint64_t interrupts, window_flags;
static uint32_t saved_priority, window_priority;

static void update_window(uint64_t flags)
{
    if ((!lock_requested && !serial_scopes) || !__atomic_load_n(&irq_ready, __ATOMIC_SEQ_CST))
    {
        if (__atomic_load_n(&lock_window, __ATOMIC_ACQUIRE))
        {
            flags = window_flags;
            __atomic_store_n(&lock_window, false, __ATOMIC_RELEASE);
            lume_mmio_write32_sync(GICC + 4, window_priority);
        }
        lume_arch_irq_restore(flags);
        return;
    }
    if (__atomic_load_n(&lock_window, __ATOMIC_ACQUIRE))
    {
        lume_arch_irq_restore(flags);
        return;
    }
    window_flags = flags;
    window_priority = lume_mmio_read32_relaxed(GICC + 4);
    uint32_t priority =
        window_priority < LUME_BOARD_IRQ_PRIORITY ? window_priority : LUME_BOARD_IRQ_PRIORITY;
    /* Lock and serial owners share this CPU0 window. Subsystem handlers remain masked. */
    lume_mmio_write32_sync(GICC + 4, priority);
    __atomic_store_n(&lock_window, true, __ATOMIC_RELEASE);
    lume_arch_irq_restore(flags & ~UINT64_C(0x80));
}

void lume_pi5_uart_rx_lock_enter(void)
{
    if (lume_cpu_index())
        return;
    if (lock_requested && __atomic_load_n(&lock_window, __ATOMIC_ACQUIRE) &&
        __atomic_load_n(&irq_ready, __ATOMIC_SEQ_CST))
        return;
    uint64_t flags = lume_arch_irq_save();
    lock_requested = true;
    update_window(flags);
}

void lume_pi5_uart_rx_lock_leave(void)
{
    if (lume_cpu_index())
        return;
    uint64_t flags = lume_arch_irq_save();
    lock_requested = false;
    update_window(flags);
}

void lume_pi5_uart_rx_lock_wait(void)
{
    if (lume_cpu_index())
        return;
    lume_pi5_uart_rx_lock_enter();
    lume_pi5_uart_rx_checkpoint();
}

void lume_console_receive_begin(void)
{
    if (lume_cpu_index())
        return;
    uint64_t flags = lume_arch_irq_save();
    if (serial_scopes == UINT_MAX)
        lume_fail("UART receive scope nesting");
    ++serial_scopes;
    update_window(flags);
}

void lume_console_receive_end(void)
{
    if (lume_cpu_index())
        return;
    uint64_t flags = lume_arch_irq_save();
    if (!serial_scopes)
        lume_fail("UART receive scope ownership");
    --serial_scopes;
    update_window(flags);
}

void lume_pi5_uart_rx_irq_mode(bool on)
{
    uint64_t flags = lume_arch_irq_save();
    const uint64_t address = GICD + 0x400 + (LUME_BOARD_UART_IRQ & ~3u);
    const unsigned shift = (LUME_BOARD_UART_IRQ & 3u) * 8;
    if (on)
    {
        if (!__atomic_load_n(&enabled, __ATOMIC_ACQUIRE) ||
            __atomic_load_n(&irq_ready, __ATOMIC_SEQ_CST))
        {
            lume_arch_irq_restore(flags);
            return;
        }
        uint32_t priority = lume_mmio_read32_relaxed(address);
        saved_priority = (priority >> shift) & 255u;
        lume_mmio_write32_sync(address, (priority & ~(255u << shift)) |
                                            (LUME_BOARD_UART_IRQ_PRIORITY << shift));
        __atomic_store_n(&irq_ready, true, __ATOMIC_SEQ_CST);
        if (!lume_cpu_index())
        {
            lock_requested |= lume_smp_owned();
            update_window(flags);
            return;
        }
    }
    else if (__atomic_exchange_n(&irq_ready, false, __ATOMIC_SEQ_CST))
    {
        lume_mmio_write32_sync(GICD + 0x180 + 4 * (LUME_BOARD_UART_IRQ / 32),
                               1u << (LUME_BOARD_UART_IRQ % 32));
        while (__atomic_load_n(&irq_active, __ATOMIC_SEQ_CST))
            lume_cpu_relax();
        uint32_t priority = lume_mmio_read32_relaxed(address);
        lume_mmio_write32_sync(address, (priority & ~(255u << shift)) | (saved_priority << shift));
    }
    lume_arch_irq_restore(flags);
}

void lume_pi5_uart_rx_enable(bool on)
{
    if (on)
        interval = lume_counter_frequency() / 4000 + 1;
    else
        lume_pi5_uart_rx_irq_mode(false);
    __atomic_store_n(&enabled, on, __ATOMIC_SEQ_CST);
    if (!on)
        while (__atomic_load_n(&collecting, __ATOMIC_SEQ_CST))
            lume_cpu_relax();
}

void lume_pi5_uart_rx_collect(void)
{
    if (!__atomic_load_n(&enabled, __ATOMIC_ACQUIRE))
        return;
    uint64_t flags = lume_arch_irq_save();
    if (__atomic_exchange_n(&collecting, 1, __ATOMIC_SEQ_CST))
    {
        lume_arch_irq_restore(flags);
        return;
    }
    if (!__atomic_load_n(&enabled, __ATOMIC_SEQ_CST))
    {
        __atomic_store_n(&collecting, 0, __ATOMIC_SEQ_CST);
        lume_arch_irq_restore(flags);
        return;
    }
    for (unsigned i = 0; i < 128; ++i)
    {
        if (lume_mmio_read32_relaxed(LUME_BOARD_RP1_UART_BASE + 0x18) & (1u << 4))
            break;
        uint32_t word = lume_mmio_read32_relaxed(LUME_BOARD_RP1_UART_BASE);
        if (word & 0xf00)
            lume_mmio_write32_relaxed(LUME_BOARD_RP1_UART_BASE + 4, 0);
        unsigned at = __atomic_load_n(&head, __ATOMIC_RELAXED);
        unsigned end = __atomic_load_n(&tail, __ATOMIC_ACQUIRE);
        if (at - end == sizeof(words) / sizeof(words[0]))
            __atomic_fetch_add(&dropped, 1, __ATOMIC_RELAXED);
        else
        {
            words[at & (sizeof(words) / sizeof(words[0]) - 1)] = word;
            __atomic_store_n(&head, at + 1, __ATOMIC_RELEASE);
        }
    }
    __atomic_store_n(&collecting, 0, __ATOMIC_SEQ_CST);
    lume_arch_irq_restore(flags);
}

static bool drain_interrupt(void)
{
    if (!__atomic_load_n(&enabled, __ATOMIC_ACQUIRE) ||
        !(lume_mmio_read32_relaxed(INTC + 0x108) & (1u << UART_SOURCE)))
        return false;
    __atomic_fetch_add(&interrupts, 1, __ATOMIC_RELAXED);
    lume_pi5_uart_rx_collect();
    lume_mmio_write32_sync(LUME_BOARD_RP1_UART_BASE + 0x44, RX_MASK);
    lume_mmio_write32_sync(INTC + 0x800 + UART_CONFIG, 1u << 2);
    return true;
}

void lume_pi5_uart_rx_irq_drain(void)
{
    (void)drain_interrupt();
}

uint64_t lume_pi5_uart_rx_irq_count(void)
{
    return __atomic_load_n(&interrupts, __ATOMIC_RELAXED);
}

bool lume_pi5_uart_rx_interrupt(unsigned *token)
{
    if (lume_cpu_index() || (!__atomic_load_n(&irq_ready, __ATOMIC_SEQ_CST) &&
                             !__atomic_load_n(&lock_window, __ATOMIC_ACQUIRE)))
        return false;
    *token = lume_mmio_read32_relaxed(GICC + 0xc);
    unsigned irq = *token & 1023u;
    if (irq >= 1020)
        return true;
    if (irq != LUME_BOARD_UART_IRQ)
        return false;

    /* The stop path waits for admitted handlers before restoring device registers. */
    __atomic_fetch_add(&irq_active, 1, __ATOMIC_SEQ_CST);
    bool drained = __atomic_load_n(&irq_ready, __ATOMIC_SEQ_CST) && drain_interrupt();
    lume_mmio_write32_sync(GICC + 0x10, *token);
    __atomic_fetch_sub(&irq_active, 1, __ATOMIC_SEQ_CST);
    if (drained)
        /* The regular SGI path resolves the current console provider under the kernel lock. */
        lume_mmio_write32_sync(GICD + 0xf00, UINT32_C(2) << 24);
    return true;
}

void lume_pi5_uart_rx_checkpoint(void)
{
    if (lume_cpu_index())
        return;
    bool ready = __atomic_load_n(&irq_ready, __ATOMIC_SEQ_CST);
    if (!ready && __atomic_load_n(&lock_window, __ATOMIC_ACQUIRE))
        update_window(lume_arch_irq_save());
    if (!__atomic_load_n(&enabled, __ATOMIC_ACQUIRE))
        return;
    /* A restrictive caller PMR is preserved and serviced by bounded polling instead. */
    if (ready && __atomic_load_n(&lock_window, __ATOMIC_ACQUIRE) &&
        window_priority > LUME_BOARD_UART_IRQ_PRIORITY)
        return;
    uint64_t now = lume_counter_read_relaxed();
    if (now - checked < interval)
        return;
    checked = now;
    lume_pi5_uart_rx_collect();
}

int lume_pi5_uart_rx_read(void)
{
    unsigned at = __atomic_load_n(&tail, __ATOMIC_RELAXED);
    if (at == __atomic_load_n(&head, __ATOMIC_ACQUIRE))
    {
        if (__atomic_load_n(&irq_ready, __ATOMIC_SEQ_CST))
            return -LUME_EAGAIN;
        lume_pi5_uart_rx_collect();
        if (at == __atomic_load_n(&head, __ATOMIC_ACQUIRE))
            return -LUME_EAGAIN;
    }
    uint32_t word = words[at & (sizeof(words) / sizeof(words[0]) - 1)];
    __atomic_store_n(&tail, at + 1, __ATOMIC_RELEASE);
    return (int)word;
}

uint64_t lume_pi5_uart_rx_dropped(void)
{
    return __atomic_load_n(&dropped, __ATOMIC_RELAXED);
}

void lume_console_receive_checkpoint(void)
{
    lume_pi5_uart_rx_checkpoint();
}
