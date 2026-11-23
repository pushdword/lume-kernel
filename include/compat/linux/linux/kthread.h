/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LUME_LINUX_KTHREAD_H
#define LUME_LINUX_KTHREAD_H

#include <linux/sched.h>
#include <linux/types.h>

struct task_struct *kthread_create(int (*threadfn)(void *), void *data, const char *namefmt, ...);
struct task_struct *kthread_create_on_node(int (*threadfn)(void *), void *data, int node,
                                          const char *namefmt, ...);
struct task_struct *kthread_run(int (*threadfn)(void *), void *data, const char *namefmt, ...);
bool kthread_should_stop(void);
int kthread_stop(struct task_struct *task);
int kthread_park(struct task_struct *task);
void kthread_unpark(struct task_struct *task);
bool kthread_should_park(void);
void kthread_parkme(void);

#endif
