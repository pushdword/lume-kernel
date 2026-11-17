/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LUME_LINUX_LIST_H
#define LUME_LINUX_LIST_H

struct list_head
{
    struct list_head *next, *prev;
};
#endif
