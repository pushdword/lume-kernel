/* SPDX-License-Identifier: GPL-2.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "lume/abi.h"
#include "lume/process.h"

#define SECTOR 17u
#define BYTES 512u

static int64_t call(uint64_t number, uint64_t a, uint64_t b, uint64_t c)
{
    register uint64_t r8 __asm__("r8") = number;
    __asm__ volatile("int $0x80" : "+D"(a) : "S"(b), "d"(c), "r"(r8) : "memory", "cc");
    return (int64_t)a;
}

static void message(const char *text)
{
    size_t bytes = 0;
    while (text[bytes])
        ++bytes;
    (void)call(LUME_SYS_WRITE, 1, (uintptr_t)text, bytes);
}

static unsigned char initial(unsigned index)
{
    return (unsigned char)(index * 17u + 0x2bu);
}

static unsigned char updated(unsigned index)
{
    return (unsigned char)(index * 31u + 0x55u);
}

static bool pattern(const unsigned char *data, bool changed)
{
    for (unsigned index = 0; index < BYTES; ++index)
        if (data[index] != (changed ? updated(index) : initial(index)))
            return false;
    return true;
}

__attribute__((section(".text.start"), noreturn)) void _start(void)
{
    static const char path[] = "/dev/usb-storage0";
    static unsigned char data[BYTES];
    int64_t fd = -LUME_ENOENT;
    for (unsigned attempt = 0; attempt < 10 && fd < 0; ++attempt)
    {
        fd = call(LUME_SYS_OPEN, (uintptr_t)path, LUME_O_RDWR, 0);
        if (fd < 0)
            (void)call(LUME_SYS_SLEEP, 100, 0, 0);
    }
    if (fd < 0 || call(LUME_SYS_SEEK, (uint64_t)fd, SECTOR * BYTES, LUME_SEEK_SET) < 0 ||
        call(LUME_SYS_READ, (uint64_t)fd, (uintptr_t)data, sizeof(data)) != sizeof(data))
        goto failed;
    if (pattern(data, true))
    {
        message("LINUX_USB_STORAGE_PERSIST_PASS\n");
        goto passed;
    }
    if (!pattern(data, false))
        goto failed;
    for (unsigned index = 0; index < BYTES; ++index)
        data[index] = updated(index);
    if (call(LUME_SYS_SEEK, (uint64_t)fd, SECTOR * BYTES, LUME_SEEK_SET) < 0 ||
        call(LUME_SYS_WRITE, (uint64_t)fd, (uintptr_t)data, sizeof(data)) != sizeof(data) ||
        call(LUME_SYS_FS, LUME_FS_SYNC, (uint64_t)fd, 0) ||
        call(LUME_SYS_SEEK, (uint64_t)fd, SECTOR * BYTES, LUME_SEEK_SET) < 0)
        goto failed;
    for (unsigned index = 0; index < BYTES; ++index)
        data[index] = 0;
    if (call(LUME_SYS_READ, (uint64_t)fd, (uintptr_t)data, sizeof(data)) != sizeof(data) ||
        !pattern(data, true))
        goto failed;
    message("LINUX_USB_STORAGE_WRITE_PASS\n");
passed:
    if (call(LUME_SYS_CLOSE, (uint64_t)fd, 0, 0))
        goto failed_closed;
    (void)call(LUME_SYS_EXIT, 0, 0, 0);
    for (;;)
    {
    }
failed:
    if (fd >= 0)
        (void)call(LUME_SYS_CLOSE, (uint64_t)fd, 0, 0);
failed_closed:
    message("LINUX_USB_STORAGE_FAIL\n");
    (void)call(LUME_SYS_EXIT, 1, 0, 0);
    for (;;)
    {
    }
}
