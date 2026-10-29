#ifndef LUME_DISPLAY_H
#define LUME_DISPLAY_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define LUME_DISPLAY_BATCH 256u
bool lume_display_start(uint64_t frequency);
void lume_display_putc(char c);
void lume_display_write(const char *data, size_t bytes);
void lume_display_clear(void);
void lume_display_stop(void);
int64_t lume_display_video(unsigned action, unsigned value, const unsigned char *font);
unsigned lume_display_geometry(void);
#endif
