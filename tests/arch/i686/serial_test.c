#include <assert.h>
#include <stdio.h>
#include "lume/abi.h"
#include "lume/irq.h"
#define LUME_Q35_H
static unsigned char hardware[8192];
static unsigned first, last, line_error;
static uint64_t irq_flags = 0x202;
static lume_irq_handler handler;

uint64_t lume_irq_save(void)
{
    uint64_t before = irq_flags;
    irq_flags &= ~UINT64_C(0x200);
    return before;
}

void lume_irq_restore(uint64_t flags)
{
    irq_flags = flags;
}

uint8_t lume_in8(uint16_t port)
{
    if (port == 0x3fd)
        return (first != last ? 0x61 : 0x60) | line_error;
    assert(port == 0x3f8 && first < last);
    return hardware[first++];
}

void lume_out8(uint16_t port, uint8_t value)
{
    assert((port == 0x3fa && value == 7) || (port == 0x3f9 && value == 5));
}

int lume_irq_register(unsigned irq, unsigned trigger, lume_irq_handler callback, void *context)
{
    assert(irq == 36 && trigger == LUME_IRQ_EDGE && !context);
    handler = callback;
    return 0;
}

int lume_irq_enable(unsigned irq, bool enable)
{
    assert(irq == 36 && enable);
    return 0;
}

void lume_value(const char *name, uint64_t value)
{
    (void)name;
    (void)value;
}

#include "../../../drivers/serial/uart16550/input.c"

int main(void)
{
    assert(!lume_pc_uart_start() && handler);
    for (unsigned round = 0; round < 8; ++round)
    {
        first = 0;
        last = 1024;
        for (unsigned i = 0; i < last; ++i)
            hardware[i] = (unsigned char)(i * 37 + round);
        handler(36, NULL);
        assert(first == last && irq_flags == 0x202);
        for (unsigned i = 0; i < last; ++i)
            assert(lume_pc_uart_getc() == hardware[i]);
        assert(lume_pc_uart_getc() == -LUME_EAGAIN);
    }
    assert(bytes == 8192 && !errors);
    first = 0;
    last = RX_BYTES + 17;
    handler(36, NULL);
    for (unsigned i = 0; i < RX_BYTES; ++i)
        assert(lume_pc_uart_getc() == hardware[i]);
    assert(lume_pc_uart_getc() == -LUME_EAGAIN && errors == 17);
    puts("PASS COM1 interrupt setup, burst reception, ring wrap, bounded overflow and IRQ flags");
    return 0;
}
