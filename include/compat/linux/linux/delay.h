/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LUME_LINUX_DELAY_H
#define LUME_LINUX_DELAY_H
#include <linux/types.h>
#define HZ 1000ul
unsigned long lume_linux_jiffies(void);
#define jiffies lume_linux_jiffies()
#define time_before(a, b) ((long)((unsigned long)(a) - (unsigned long)(b)) < 0)
void usleep_range(unsigned long, unsigned long);
#endif
