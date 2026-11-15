#include "lume/q35.h"
#include "lume/irq.h"
#include "lume/irq_async.h"
#include "lume/ticket_lock.h"
#include "lume/serial.h"
#include "lume/abi.h"
#define COM1 0x3f8
#define RX_BYTES 4096u
static unsigned char received[RX_BYTES];
static unsigned head, count;
static uint64_t bytes, errors;
static struct lume_ticket_lock rx_lock;

static uint32_t receive_lock(void)
{
    uint32_t ticket = lume_ticket_reserve(&rx_lock);
    while (!lume_ticket_ready(&rx_lock, ticket))
        __asm__ volatile("pause" ::: "memory");
    return ticket;
}

static void collect(void)
{
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
}

void lume_pc_uart_poll(void)
{
    uint64_t flags = lume_irq_save();
    uint32_t ticket = receive_lock();
    collect();
    lume_ticket_release(&rx_lock, ticket);
    lume_irq_restore(flags);
}

int lume_pc_uart_getc(void)
{
    uint64_t flags = lume_irq_save();
    uint32_t ticket = receive_lock();
    collect();
    int value = -LUME_EAGAIN;
    if (count)
    {
        value = received[head];
        head = (head + 1) % RX_BYTES;
        --count;
    }
    lume_ticket_release(&rx_lock, ticket);
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
    int result = lume_irq_register_async(36, LUME_IRQ_EDGE, receive_irq, NULL);
    if (result)
        return result;
    lume_out8(COM1 + 2, 0x07);
    lume_out8(COM1 + 1, 0x05);
    return lume_irq_enable(36, true);
}

void lume_pc_uart_stats(void)
{
    uint64_t flags = lume_irq_save();
    uint32_t ticket = receive_lock();
    uint64_t received_bytes = bytes, received_errors = errors;
    lume_ticket_release(&rx_lock, ticket);
    lume_irq_restore(flags);
    lume_value("UART_RX_BYTES", received_bytes);
    lume_value("UART_RX_ERRORS", received_errors);
}
