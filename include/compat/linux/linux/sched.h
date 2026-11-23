/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LUME_LINUX_SCHED_H
#define LUME_LINUX_SCHED_H

struct task_struct;

int wake_up_process(struct task_struct *task);

#endif
