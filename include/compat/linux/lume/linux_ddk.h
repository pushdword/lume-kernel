/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LUME_LINUX_DDK_H
#define LUME_LINUX_DDK_H
#include <lume/linux_compat.h>
#include <lume/hardware.h>
int lume_linux_ddk_start(struct lume_hardware *);
/* Lifecycle callbacks require scheduler serialization. Pending reads prevent stop. */
int lume_linux_ddk_can_stop(void);
bool lume_linux_ddk_entropy(void *, size_t);
#endif
