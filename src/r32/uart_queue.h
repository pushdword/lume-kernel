#ifndef LUME_UART_QUEUE_H
#define LUME_UART_QUEUE_H
#include <stdint.h>
#define UART_REQUEST_BYTES 4096u
#define UART_REQUESTS 4u

struct uart_queue_stats
{
    uint32_t version, bytes, active, logical_bytes, fifo_bytes, requests, high_water, backpressure,
        service_calls, service_ticks, max_service_ticks, tx_polls, rx_bytes, rx_high_water, overrun,
        framing, stalls, recoveries;
};

/* Producers are serialized by queue_lock. The internal UART lock is always
 * try-only; IRQ/lock-wait consumers never acquire the scheduler lock. */
int uart_queue_start(void);
int uart_queue_active(void);
int uart_queue_submit(const unsigned char *, unsigned);
/* Read-only eligibility hint; service rechecks ownership and capacity. */
int uart_queue_service_ready(void);
int uart_queue_service(void);
int uart_queue_read(void);
int uart_queue_drain(void);
void uart_queue_tick(uint32_t);
int uart_queue_snapshot(struct uart_queue_stats *);
/* Panic: quarantines RAM requests, waits finitely for a concurrent consumer,
 * drains existing HW FIFO, emits finitely, never depends on scheduler/IRQ. */
void uart_queue_emergency(const char *);
#endif
