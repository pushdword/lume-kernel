/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef LUME_SIGNAL_WAIT_H
#define LUME_SIGNAL_WAIT_H
#include "signal_mask.h"
#include <stdbool.h>

static inline bool signal_pending_keep(unsigned number, uint64_t handler, uint32_t blocked)
{
    if (!number || number >= LUME_SIGNAL_COUNT || handler == 1)
        return false;
    bool ignored =
        !handler && (number == 16 || number == 19 || number == 20 || number == 23 || number == 28);
    return !ignored || (blocked & (UINT32_C(1) << number));
}

static inline int64_t signal_try_wait(uint32_t *pending, uint32_t blocked,
                                      struct lume_signal_request *request)
{
    if (request->version != LUME_SIGNAL_VERSION || request->bytes != sizeof(*request) ||
        !request->mask || (request->mask & (LUME_SIGNAL_UNMASKABLE | 1u)) ||
        (request->mask & ~blocked) || request->number || request->flags || request->handler ||
        request->restorer || request->delay_ms || request->interval_ms || request->operation ||
        request->pid || request->group)
        return -LUME_EINVAL;
    uint32_t selected = *pending & request->mask;
    if (!selected)
        return -LUME_EAGAIN;
    unsigned number = 1;
    while (!(selected & (UINT32_C(1) << number)))
        ++number;
    *pending &= ~(UINT32_C(1) << number);
    request->number = number;
    return 0;
}
#endif
