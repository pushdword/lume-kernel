#include "lume/q35.h"
#include "lume/irq.h"
#include "lume/serial.h"
#include "lume/abi.h"
#define COM1 0x3f8
#define RX_BYTES 4096u
static unsigned char received[RX_BYTES];
static unsigned head, count;
static uint64_t bytes, errors;

void lume_pc_uart_poll(void)
{
    uint64_t flags = lume_irq_save();
    for (unsigned i = 0; i < RX_BYTES; ++i)
    {
        uint8_t status = lume_in8(COM1 + 5);
        if (status & 0x1e)
            ++errors;
        if (!(status & 1))
            break;
        unsigned char value = lume_in8(COM1);
        if (count == RX_BYTES || (status & 0x1c))
        {
            ++errors;
            continue;
        }
        received[(head + count) % RX_BYTES] = value;
        ++count;
        ++bytes;
    }
    lume_irq_restore(flags);
}

int lume_pc_uart_getc(void)
{
    lume_pc_uart_poll();
    uint64_t flags = lume_irq_save();
    int value = -LUME_EAGAIN;
    if (count)
    {
        value = received[head];
        head = (head + 1) % RX_BYTES;
        --count;
    }
    lume_irq_restore(flags);
    return value;
}

static void receive_irq(unsigned irq, void *context)
{
    (void)irq;
    (void)context;
    lume_pc_uart_poll();
}

int lume_pc_uart_start(void)
{
    int result = lume_irq_register(36, LUME_IRQ_EDGE, receive_irq, NULL);
    if (result)
        return result;
    lume_out8(COM1 + 2, 0x07);
    lume_out8(COM1 + 1, 0x05);
    return lume_irq_enable(36, true);
}

void lume_pc_uart_stats(void)
{
    lume_value("UART_RX_BYTES", bytes);
    lume_value("UART_RX_ERRORS", errors);
}
