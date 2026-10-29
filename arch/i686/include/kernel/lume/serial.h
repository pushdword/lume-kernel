#ifndef LUME_PC_SERIAL_H
#define LUME_PC_SERIAL_H
int lume_pc_uart_start(void);
int lume_pc_uart_getc(void);
void lume_pc_uart_poll(void);
void lume_pc_uart_stats(void);
#endif
