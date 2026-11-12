/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef LUME_DEVFS_CHARACTER_H
#define LUME_DEVFS_CHARACTER_H
#include <stdint.h>

struct lume_files;

/* Character devices create /dev/<directory>/<name>. Open callbacks enforce
 * device access requirements. They return an
 * attached descriptor and retain their driver for its complete lifetime.
 * Registration fields remain valid until unregister succeeds. Unregister rejects
 * an active open callback. Drivers also reject removal while descriptions exist. */
struct lume_devfs_character
{
    const char *directory, *name;
    void *context;
    int (*open)(void *, struct lume_files *, uint64_t);
};

int lume_devfs_register_character(struct lume_devfs_character *);
int lume_devfs_unregister_character(struct lume_devfs_character *);
#endif
