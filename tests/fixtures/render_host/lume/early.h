/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef LUME_RENDER_HOST_EARLY_H
#define LUME_RENDER_HOST_EARLY_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
bool lume_user_buffer(uint64_t, uint64_t, bool);
bool lume_task_capable(unsigned, uint64_t);
_Noreturn void lume_fail(const char *);
#endif
