#include "lume/uart_rx.h"
#include "lume/board.h"
#include "lume/mmio.h"
#include "lume/counter.h"
#include "lume/smp.h"
#include "lume/abi.h"
#include "lume/cpu.h"
#include "lume/console_output.h"

static uint32_t words[16384];
static unsigned head, tail, collecting;
static bool enabled;
static uint64_t dropped, checked, interval;

void lume_pi5_uart_rx_enable(bool on)
{
    if (on)
        interval = lume_counter_frequency() / 4000 + 1;
    __atomic_store_n(&enabled, on, __ATOMIC_RELEASE);
}

void lume_pi5_uart_rx_collect(void)
{
    if (!__atomic_load_n(&enabled, __ATOMIC_ACQUIRE))
        return;
    uint64_t flags = lume_arch_irq_save();
    if (__atomic_exchange_n(&collecting, 1, __ATOMIC_ACQUIRE))
    {
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
    __atomic_store_n(&collecting, 0, __ATOMIC_RELEASE);
    lume_arch_irq_restore(flags);
}

void lume_pi5_uart_rx_checkpoint(void)
{
    if (lume_cpu_index() || !__atomic_load_n(&enabled, __ATOMIC_ACQUIRE))
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
