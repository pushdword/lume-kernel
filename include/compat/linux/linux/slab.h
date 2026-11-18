/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LUME_LINUX_SLAB_H
#define LUME_LINUX_SLAB_H
#include <linux/types.h>
struct device;
#define ZERO_SIZE_PTR ((void *)16)
#define ZERO_OR_NULL_PTR(pointer) ((uintptr_t)(pointer) <= (uintptr_t)ZERO_SIZE_PTR)
void *kmalloc(size_t, gfp_t);
void *kzalloc(size_t, gfp_t);
void *kmalloc_array(size_t, size_t, gfp_t);
void *kcalloc(size_t, size_t, gfp_t);
void *krealloc(const void *, size_t, gfp_t);
void *kmemdup(const void *, size_t, gfp_t);
size_t ksize(const void *);
void kfree(const void *);
void kfree_sensitive(const void *);
void *devm_kzalloc(struct device *, size_t, gfp_t);
#endif
