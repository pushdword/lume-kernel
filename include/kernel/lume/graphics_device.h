#ifndef LUME_GRAPHICS_H
#define LUME_GRAPHICS_H
#include <stdbool.h>
#include "lume/graphics.h"
struct lume_files;
int lume_graphics_call(struct lume_files *, unsigned, unsigned, struct lume_graphics_request *);
int lume_graphics_user_call(struct lume_files *, unsigned, unsigned, uint64_t, uint64_t);
void lume_graphics_release(unsigned);
bool lume_graphics_input_owned(void);
uint64_t lume_graphics_active_generation(void);
uint64_t lume_graphics_session_generation(struct lume_files *, unsigned, unsigned);
void lume_graphics_input(const struct lume_input_event *);
#endif
