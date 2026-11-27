#ifndef LUME_REALTIME_ABI_H
#define LUME_REALTIME_ABI_H
#include <stdint.h>

#define LUME_REALTIME_VERSION 1u

enum
{
    LUME_REALTIME_GET,
    LUME_REALTIME_SET,
    LUME_REALTIME_INITIALIZE
};

/* Seconds count from the Unix epoch. An unset clock returns EAGAIN.
 * SET requires clock administration. INITIALIZE allows the boot network service to
 * set an unset clock once and returns EALREADY if the clock is set.
 * Flags and reserved fields are zero. */
struct lume_realtime_request
{
    uint32_t version, bytes;
    uint64_t seconds;
    uint32_t nanoseconds, flags;
    uint64_t reserved;
};

_Static_assert(sizeof(struct lume_realtime_request) == 32, "realtime request ABI");
#endif
