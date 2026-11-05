#ifndef LUME_SIGNAL_MASK_STATE_H
#define LUME_SIGNAL_MASK_STATE_H
#include "lume/abi.h"
#include "lume/signal.h"

#define LUME_SIGNAL_UNMASKABLE ((UINT32_C(1) << 9) | (UINT32_C(1) << 17))

static inline int64_t signal_mask_update(uint32_t *mask, uint32_t pending, uint64_t epoch,
                                         struct lume_signal_request *request)
{
    if (request->flags > 1 || (request->flags && request->operation > 2))
        return -LUME_EINVAL;
    uint32_t previous = *mask;
    if (request->flags)
    {
        uint32_t selected = request->mask & ~LUME_SIGNAL_UNMASKABLE;
        *mask = request->operation == 0   ? selected
                : request->operation == 1 ? previous | selected
                                          : previous & ~selected;
    }
    request->mask = previous;
    request->delay_ms = epoch;
    request->interval_ms = pending & ~*mask;
    return 0;
}
#endif
