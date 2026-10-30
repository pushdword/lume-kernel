#ifndef LUME_DDK_CONSOLE_H
#define LUME_DDK_CONSOLE_H
#include <stdint.h>

/* Diagnostic output requires serialized task context and must not run in an IRQ handler. */
void lume_puts(const char *);
void lume_value(const char *name, uint64_t value);
#endif
