#ifndef LUME_PL011_UART_IRQ_H
#define LUME_PL011_UART_IRQ_H
#include <stdbool.h>
#include <stdint.h>
bool lume_uart_irq_start(void);
void lume_uart_irq_stop(void);
uint64_t lume_uart_irq_count(void);
#endif
