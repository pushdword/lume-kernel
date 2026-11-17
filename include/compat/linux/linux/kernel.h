/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LUME_LINUX_KERNEL_H
#define LUME_LINUX_KERNEL_H
#include <linux/types.h>
#include <linux/err.h>
#define container_of(pointer, type, member) ((type *)((char *)(pointer) - offsetof(type, member)))
#define min(a, b)                                                                                  \
    ({                                                                                             \
        __auto_type _a = (a);                                                                      \
        __auto_type _b = (b);                                                                      \
        _a < _b ? _a : _b;                                                                         \
    })
#define min_t(type, a, b)                                                                          \
    ({                                                                                             \
        type _a = (a);                                                                             \
        type _b = (b);                                                                             \
        _a < _b ? _a : _b;                                                                         \
    })
#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))
int lume_linux_sprintf(char *, const char *, ...);
#define sprintf lume_linux_sprintf
#endif
