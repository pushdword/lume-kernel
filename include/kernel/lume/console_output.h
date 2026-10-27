#ifndef LUME_CONSOLE_OUTPUT_H
#define LUME_CONSOLE_OUTPUT_H
#include <stddef.h>
#include <stdint.h>

#define LUME_CONSOLE_OUTPUT_BYTES 64u

struct lume_console_output
{
    char data[LUME_CONSOLE_OUTPUT_BYTES * 2];
    size_t bytes;
    void (*transmit)(const char *, size_t);
    void *provider;
    uint64_t ticket;
};

/* Capture, render and completion require the kernel lock. Transmission uses only
 * the copied payload and a pinned provider, with IRQs masked and the kernel lock released. */
void lume_console_output_begin(struct lume_console_output *);
void lume_console_output_end(struct lume_console_output *);
void lume_console_output_send(const struct lume_console_output *);
void lume_console_output_finish(struct lume_console_output *);

void lume_console_output_start(void);
uint64_t lume_console_serial_reserve(void);
void lume_console_serial_wait(uint64_t);
uint64_t lume_console_serial_lock(void);
void lume_console_serial_unlock(uint64_t);
void lume_uart_render(const char *, size_t);
void lume_uart_transmit(const char *, size_t);
#endif
