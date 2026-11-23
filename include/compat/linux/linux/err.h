/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LUME_LINUX_ERR_H
#define LUME_LINUX_ERR_H
#include <linux/types.h>
#define EPERM 1
#define ENOENT 2
#define EINTR 4
#define EIO 5
#define ENOMEM 12
#define EBUSY 16
#define ENODEV 19
#define EINVAL 22
#define ENOSPC 28
#define ENOSYS 38
#ifdef EOPNOTSUPP
#undef EOPNOTSUPP
#endif
#ifdef ETIMEDOUT
#undef ETIMEDOUT
#endif
#define EOPNOTSUPP 95
#define ETIMEDOUT 110
#define ERR_PTR(error) ((void *)(intptr_t)(error))
#define PTR_ERR(pointer) ((long)(intptr_t)(pointer))
#define IS_ERR(pointer) ((uintptr_t)(pointer) >= (uintptr_t)-4095)
#endif
