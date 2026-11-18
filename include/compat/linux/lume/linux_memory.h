/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LUME_LINUX_MEMORY_H
#define LUME_LINUX_MEMORY_H
#include <stddef.h>

/* Runtime ownership serializes allocator setup, calls and teardown. */
int lume_linux_memory_start(void *(*allocate)(size_t), void (*release)(void *));
int lume_linux_memory_stop(void);
unsigned lume_linux_memory_allocations(void);
#endif
