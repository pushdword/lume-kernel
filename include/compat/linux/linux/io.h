/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LUME_LINUX_IO_H
#define LUME_LINUX_IO_H
#include <linux/types.h>
u32 lume_linux_ioread32(const volatile void *);
void lume_linux_iowrite32(u32, volatile void *);
#define ioread32(address) lume_linux_ioread32(address)
#define iowrite32(value, address) lume_linux_iowrite32(value, address)
#define readl(address) ioread32(address)
#define writel(value, address) iowrite32(value, address)
#endif
