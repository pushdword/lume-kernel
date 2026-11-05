#ifndef LUME_REALTIME_H
#define LUME_REALTIME_H
#include <stdbool.h>
#include <stdint.h>
#include "lume/realtime_abi.h"

struct lume_realtime
{
    uint64_t seconds, monotonic;
    uint32_t nanoseconds;
    bool valid;
};

bool lume_realtime_set(struct lume_realtime *, uint64_t, uint32_t, uint64_t);
bool lume_realtime_read(const struct lume_realtime *, uint64_t, uint64_t *, uint32_t *);
int lume_realtime_control(struct lume_realtime *, unsigned, struct lume_realtime_request *,
                          uint64_t, bool);
int lume_clock_realtime_ms(uint64_t, uint64_t *);
#endif
