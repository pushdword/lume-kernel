#ifndef LUME_IO_ABI_H
#define LUME_IO_ABI_H
#include <stdint.h>
#define LUME_IO_VERSION 1u
#define LUME_IO_BYTES 4096u

enum
{
    LUME_STREAM_CONNECT,
    LUME_STREAM_SEND,
    LUME_STREAM_RECEIVE,
    LUME_STREAM_CLOSE
};

struct lume_stream_request
{
    uint32_t version, handle, count, port;
    unsigned char address[4];
    uint32_t reserved;
    unsigned char data[LUME_IO_BYTES];
};

enum
{
    LUME_BOOT_INFO,
    LUME_BOOT_BEGIN,
    LUME_BOOT_WRITE,
    LUME_BOOT_VERIFY,
    LUME_BOOT_ACTIVATE,
    LUME_BOOT_ABORT
};

struct lume_boot_request
{
    uint32_t version, count, total, offset, crc, active;
    unsigned char data[LUME_IO_BYTES];
};
#endif
