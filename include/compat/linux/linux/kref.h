/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LUME_LINUX_KREF_H
#define LUME_LINUX_KREF_H
#include <linux/list.h>

struct kref
{
    unsigned references;
};
#endif
