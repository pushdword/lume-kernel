#include "lume/abi.h"
#include "lume/realtime.h"

int lume_clock_realtime_ms(uint64_t monotonic, uint64_t *out)
{
    (void)monotonic;
    (void)out;
    return -LUME_EAGAIN;
}
