#ifndef LUME_PI5_UART_RX_H
#define LUME_PI5_UART_RX_H
#include <stdbool.h>
#include <stdint.h>
/* CPU0 provides background collection. Consumers hold the kernel lock and may also
 * collect raw words. Collection never enters subsystem callbacks. */
void lume_pi5_uart_rx_enable(bool);
void lume_pi5_uart_rx_collect(void);
void lume_pi5_uart_rx_checkpoint(void);
int lume_pi5_uart_rx_read(void);
uint64_t lume_pi5_uart_rx_dropped(void);
#endif
