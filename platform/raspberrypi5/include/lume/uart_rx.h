#ifndef LUME_PI5_UART_RX_H
#define LUME_PI5_UART_RX_H
#include <stdbool.h>
#include <stdint.h>
/* CPU0 handles raw receive interrupts without entering subsystem callbacks.
 * Consumers hold the kernel lock. Lock requests are idempotent and serial scopes nest.
 * The final owner restores DAIF and PMR. IRQ stop quiesces handlers without waiting for owners.
 * Polling remains available when interrupt delivery cannot make progress. */
void lume_pi5_uart_rx_enable(bool);
void lume_pi5_uart_rx_collect(void);
void lume_pi5_uart_rx_checkpoint(void);
void lume_pi5_uart_rx_irq_mode(bool);
void lume_pi5_uart_rx_irq_drain(void);
uint64_t lume_pi5_uart_rx_irq_count(void);
bool lume_pi5_uart_rx_interrupt(unsigned *token);
void lume_pi5_uart_rx_lock_enter(void);
void lume_pi5_uart_rx_lock_wait(void);
void lume_pi5_uart_rx_lock_leave(void);
int lume_pi5_uart_rx_read(void);
uint64_t lume_pi5_uart_rx_dropped(void);
#endif
