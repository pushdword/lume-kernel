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
    LUME_STREAM_CLOSE,
    LUME_STREAM_CONNECT_ASYNC,
    LUME_STREAM_POLL,
    LUME_STREAM_SHUTDOWN_WRITE,
    LUME_STREAM_LISTEN,
    LUME_STREAM_ACCEPT,
    LUME_DATAGRAM_CONNECT,
    LUME_DATAGRAM_LISTEN
};

enum
{
    LUME_STREAM_READABLE = 1,
    LUME_STREAM_WRITABLE = 2,
    LUME_STREAM_ERROR = 4,
    LUME_STREAM_HANGUP = 8
};

/* The original four operations retain their blocking behavior. POLL returns readiness
 * flags in count and as its result. CONNECT_ASYNC returns a handle before TCP connects.
 * LISTEN/ACCEPT create separate handles. SHUTDOWN_WRITE sends FIN after queued TCP data.
 * Datagram receives consume one packet and return its source in address/port. Sends are
 * limited to 1472 bytes. A datagram listener selects its first peer until closed. */
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
