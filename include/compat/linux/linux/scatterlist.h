/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LUME_LINUX_SCATTERLIST_H
#define LUME_LINUX_SCATTERLIST_H
#include <linux/types.h>

struct scatterlist
{
    void *buffer;
    unsigned length;
};

static inline void sg_init_one(struct scatterlist *sg, const void *buffer, unsigned length)
{
    sg->buffer = (void *)buffer;
    sg->length = length;
}
#endif
