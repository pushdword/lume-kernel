/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LUME_LINUX_TIMER_H
#define LUME_LINUX_TIMER_H
#include <linux/delay.h>
#include <linux/kernel.h>

struct timer_list;
typedef void (*timer_func_t)(struct timer_list *);

struct timer_list
{
    struct timer_list *next;
    timer_func_t function;
    unsigned long expires;
    volatile u32 state;
};

void lume_linux_timer_setup(struct timer_list *, timer_func_t, unsigned);
#define timer_setup(timer, callback, flags) lume_linux_timer_setup((timer), (callback), (flags))
#define from_timer(object, timer, member) container_of((timer), typeof(*(object)), member)
int mod_timer(struct timer_list *, unsigned long);
int del_timer(struct timer_list *);
int del_timer_sync(struct timer_list *);
#define timer_delete(timer) del_timer(timer)
#define timer_delete_sync(timer) del_timer_sync(timer)
int timer_pending(const struct timer_list *);

#endif
