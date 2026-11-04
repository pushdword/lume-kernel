#include "lume/realtime.h"
#include "lume/abi.h"

bool lume_realtime_set(struct lume_realtime *clock, uint64_t seconds, uint32_t nanoseconds,
                       uint64_t monotonic)
{
    if (!clock || seconds > INT64_MAX || nanoseconds >= UINT32_C(1000000000))
        return false;
    *clock = (struct lume_realtime){
        .seconds = seconds, .monotonic = monotonic, .nanoseconds = nanoseconds, .valid = true};
    return true;
}

bool lume_realtime_read(const struct lume_realtime *clock, uint64_t monotonic, uint64_t *seconds,
                        uint32_t *nanoseconds)
{
    if (!clock || !clock->valid || !seconds || !nanoseconds || monotonic < clock->monotonic)
        return false;
    uint64_t elapsed = monotonic - clock->monotonic;
    uint64_t whole = elapsed / 1000;
    uint32_t fraction = clock->nanoseconds + (uint32_t)(elapsed % 1000) * UINT32_C(1000000);
    if (fraction >= UINT32_C(1000000000))
    {
        ++whole;
        fraction -= UINT32_C(1000000000);
    }
    if (whole > INT64_MAX || clock->seconds > (uint64_t)INT64_MAX - whole)
        return false;
    *seconds = clock->seconds + whole;
    *nanoseconds = fraction;
    return true;
}

int lume_realtime_control(struct lume_realtime *clock, unsigned action,
                          struct lume_realtime_request *request, uint64_t monotonic,
                          bool administrator)
{
    if (!clock || !request || request->version != LUME_REALTIME_VERSION ||
        request->bytes != sizeof(*request) || request->flags || request->reserved ||
        action > LUME_REALTIME_SET)
        return -LUME_EINVAL;
    if (action == LUME_REALTIME_SET)
    {
        if (!administrator)
            return -LUME_EACCES;
        return lume_realtime_set(clock, request->seconds, request->nanoseconds, monotonic)
                   ? 0
                   : -LUME_EINVAL;
    }
    if (request->seconds || request->nanoseconds)
        return -LUME_EINVAL;
    return lume_realtime_read(clock, monotonic, &request->seconds, &request->nanoseconds)
               ? 0
               : -LUME_EAGAIN;
}
