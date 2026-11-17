/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LUME_LINUX_SLAB_H
#define LUME_LINUX_SLAB_H
#include <linux/types.h>
struct device;
void *kmalloc(size_t, gfp_t);
void *kzalloc(size_t, gfp_t);
void kfree(const void *);
void *devm_kzalloc(struct device *, size_t, gfp_t);
#endif
