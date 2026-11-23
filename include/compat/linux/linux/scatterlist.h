/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LUME_LINUX_SCATTERLIST_H
#define LUME_LINUX_SCATTERLIST_H
#include <linux/types.h>

struct scatterlist
{
    void *buffer;
    unsigned length;
    struct scatterlist *chain;
    bool last;
};

static inline void sg_set_buf(struct scatterlist *sg, const void *buffer, unsigned length)
{
    sg->buffer = (void *)buffer;
    sg->length = length;
}

static inline void sg_mark_end(struct scatterlist *sg)
{
    sg->chain = NULL;
    sg->last = true;
}

static inline void sg_init_table(struct scatterlist *sg, unsigned count)
{
    for (unsigned i = 0; i < count; ++i)
        sg[i] = (struct scatterlist){0};
    if (count)
        sg_mark_end(&sg[count - 1]);
}

static inline void sg_init_one(struct scatterlist *sg, const void *buffer, unsigned length)
{
    sg_init_table(sg, 1);
    sg_set_buf(sg, buffer, length);
}

static inline void sg_chain(struct scatterlist *sg, unsigned count, struct scatterlist *next)
{
    sg[count - 1] = (struct scatterlist){.chain = next};
}

static inline struct scatterlist *sg_next(struct scatterlist *sg)
{
    if (sg->last)
        return NULL;
    ++sg;
    return sg->chain ? sg->chain : sg;
}
#endif
