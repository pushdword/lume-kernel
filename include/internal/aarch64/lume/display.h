#ifndef LUME_DISPLAY_H
#define LUME_DISPLAY_H
#include <stdbool.h>
#include <stdint.h>
bool lume_display_start(uint64_t frequency);
void lume_display_putc(char c);
void lume_display_clear(void);
void lume_display_stop(void);
#endif
