/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LUME_LINUX_MODULE_H
#define LUME_LINUX_MODULE_H
#include <linux/kernel.h>
#include <linux/slab.h>
#define MODULE_DEVICE_TABLE(kind, table)
#define LUME_LINUX_MODINFO(kind, value)                                                            \
    static const char lume_linux_modinfo_##kind[] __attribute__((used, section(".modinfo"))) =     \
        #kind "=" value
#define MODULE_AUTHOR(value) LUME_LINUX_MODINFO(author, value)
#define MODULE_DESCRIPTION(value) LUME_LINUX_MODINFO(description, value)
#define MODULE_LICENSE(value) LUME_LINUX_MODINFO(license, value)
#endif
