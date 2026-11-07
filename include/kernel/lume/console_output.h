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

/* Capture, render and completion require the kernel lock. Transmission requires a
 * copied payload and a pinned provider with the kernel lock released. A platform receive scope
 * may admit resident raw RX while subsystem interrupts remain masked. */
void lume_console_output_begin(struct lume_console_output *);
void lume_console_output_end(struct lume_console_output *);
void lume_console_output_send(const struct lume_console_output *);
void lume_console_output_finish(struct lume_console_output *);

void lume_console_output_start(void);
/* Raw receive progress is bounded and cannot enter subsystem callbacks. */
void lume_console_receive_checkpoint(void);
/* Serial scopes nest and cover the complete ticket wait and transmission. */
void lume_console_receive_begin(void);
void lume_console_receive_end(void);
uint64_t lume_console_serial_reserve(void);
void lume_console_serial_wait(uint64_t);
uint64_t lume_console_serial_lock(void);
void lume_console_serial_unlock(uint64_t);
void lume_uart_render(const char *, size_t);
void lume_uart_transmit(const char *, size_t);
#endif
