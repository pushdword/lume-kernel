#ifndef LUME_REALTIME_H
#define LUME_REALTIME_H
#include <stdbool.h>
#include <stdint.h>

struct lume_realtime
{
    uint64_t seconds, monotonic;
    uint32_t nanoseconds;
    bool valid;
};

bool lume_realtime_set(struct lume_realtime *, uint64_t, uint32_t, uint64_t);
bool lume_realtime_read(const struct lume_realtime *, uint64_t, uint64_t *, uint32_t *);
#endif
