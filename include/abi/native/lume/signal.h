#ifndef LUME_SIGNAL_ABI_H
#define LUME_SIGNAL_ABI_H
#include <stdint.h>
#define LUME_SIGNAL_VERSION 1u
#define LUME_SIGNAL_COUNT 32u

enum
{
    LUME_SIGNAL_ACTION,
    LUME_SIGNAL_MASK,
    LUME_SIGNAL_SEND,
    LUME_SIGNAL_TIMER,
    LUME_SIGNAL_RETURN,
    LUME_SIGNAL_PARENT,
    LUME_SIGNAL_SESSION,
    LUME_SIGNAL_GROUP,
    LUME_SIGNAL_SET_SESSION,
    LUME_SIGNAL_SET_GROUP
};

struct lume_signal_request
{
    uint32_t version, bytes, number, flags;
    uint64_t handler, restorer, delay_ms, interval_ms;
    uint32_t mask, operation;
    int32_t pid, group;
};

_Static_assert(sizeof(struct lume_signal_request) == 64, "signal request ABI");
#endif
