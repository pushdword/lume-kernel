/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LUME_LINUX_IDR_H
#define LUME_LINUX_IDR_H
#include <linux/types.h>

struct ida
{
    uint32_t allocated;
};

#define DEFINE_IDA(name) struct ida name = {0}
int ida_alloc(struct ida *, gfp_t);
void ida_free(struct ida *, unsigned);
#endif
