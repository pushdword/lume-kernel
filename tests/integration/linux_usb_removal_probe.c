/* SPDX-License-Identifier: GPL-2.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "lume/abi.h"
#include "lume/process.h"

static const char path[] = "/dev/usb-storage0";
static unsigned char sector[512];

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

static int64_t wait_open(bool present)
{
    for (unsigned attempt = 0; attempt < 160; ++attempt)
    {
        int64_t fd = call(LUME_SYS_OPEN, (uintptr_t)path, LUME_O_RDWR, 0);
        if ((present && fd >= 0) || (!present && fd == -LUME_ENOENT))
            return fd;
        if (fd >= 0)
            (void)call(LUME_SYS_CLOSE, (uint64_t)fd, 0, 0);
        (void)call(LUME_SYS_SLEEP, 100, 0, 0);
    }
    return -LUME_EIO;
}

static bool read_pattern(uint64_t fd, bool replacement)
{
    if (call(LUME_SYS_SEEK, fd, 17 * sizeof(sector), LUME_SEEK_SET) < 0 ||
        call(LUME_SYS_READ, fd, (uintptr_t)sector, sizeof(sector)) != sizeof(sector))
        return false;
    for (unsigned i = 0; i < sizeof(sector); ++i)
    {
        unsigned char expected =
            replacement ? (unsigned char)(i * 31 + 0x55) : (unsigned char)(i * 17 + 0x2b);
        if (sector[i] != expected)
            return false;
    }
    return true;
}

__attribute__((section(".text.start"), noreturn)) void _start(void)
{
    int64_t fd = wait_open(true);
    if (fd < 0 || !read_pattern((uint64_t)fd, false))
        goto failed;
    message("USB_REMOVAL_OPEN\n");
    if (wait_open(false) != -LUME_ENOENT ||
        call(LUME_SYS_READ, (uint64_t)fd, (uintptr_t)sector, sizeof(sector)) != -LUME_EIO ||
        call(LUME_SYS_WRITE, (uint64_t)fd, (uintptr_t)sector, sizeof(sector)) != -LUME_EIO ||
        call(LUME_SYS_FS, LUME_FS_SYNC, (uint64_t)fd, 0) != -LUME_EIO ||
        call(LUME_SYS_CLOSE, (uint64_t)fd, 0, 0))
        goto failed;
    message("USB_REMOVAL_CLOSED\n");
    fd = wait_open(true);
    if (fd < 0 || !read_pattern((uint64_t)fd, true) || call(LUME_SYS_CLOSE, (uint64_t)fd, 0, 0))
        goto failed;
    message("USB_REMOVAL_PASS\n");
    (void)call(LUME_SYS_EXIT, 0, 0, 0);
    for (;;)
    {
    }
failed:
    message("USB_REMOVAL_FAIL\n");
    (void)call(LUME_SYS_EXIT, 1, 0, 0);
    for (;;)
    {
    }
}
