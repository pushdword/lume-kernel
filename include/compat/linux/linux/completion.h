/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LUME_LINUX_COMPLETION_H
#define LUME_LINUX_COMPLETION_H
#include <linux/types.h>

struct completion
{
    unsigned done;
};

void init_completion(struct completion *);
void reinit_completion(struct completion *);
void complete(struct completion *);
/* Task waits abort on transport failure or after five seconds without completion. */
int wait_for_completion_killable(struct completion *);
#endif
