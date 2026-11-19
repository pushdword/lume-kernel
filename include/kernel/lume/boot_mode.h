#ifndef LUME_BOOT_MODE_H
#define LUME_BOOT_MODE_H

#include "lume/early.h"
#include "lume/vfs.h"
#include <string.h>

static inline bool lume_boot_mode_is(const char *expected)
{
    struct lume_files files;
    if (!lume_files_init(&files))
        lume_fail("boot mode initialization");
    char mode[16];
    int64_t fd = lume_vfs_open(&files, "/etc/bootmode", LUME_O_RDONLY);
    int64_t bytes = fd < 0 ? fd : lume_vfs_read(&files, (uint64_t)fd, mode, sizeof(mode));
    lume_files_release(&files);
    size_t length = strlen(expected);
    return length < sizeof(mode) && bytes == (int64_t)length && !memcmp(mode, expected, length);
}

#endif
