/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef LUME_RENDER_DEVICE_H
#define LUME_RENDER_DEVICE_H
#include <stdint.h>

struct lume_files;
int lume_render_open_file(struct lume_files *, unsigned);
int lume_render_device_register(void);
int lume_render_device_unregister(void);
int64_t lume_render_call(struct lume_files *, unsigned, unsigned, uint64_t, uint64_t);
void lume_render_poll_background(void);
#endif
