#ifndef LUME_GRAPHICS_H
#define LUME_GRAPHICS_H
#include <stdbool.h>
#include "lume/graphics.h"
struct lume_files;
int lume_graphics_call(struct lume_files *, unsigned, unsigned, struct lume_graphics_request *);
void lume_graphics_release(unsigned);
bool lume_graphics_input_owned(void);
void lume_graphics_input(const struct lume_input_event *);
#endif
