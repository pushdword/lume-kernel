#include "lume/realtime.h"
#include "lume/abi.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    struct lume_realtime clock = {0};
    uint64_t seconds = 77;
    uint32_t nanoseconds = 88;
    assert(!lume_realtime_read(&clock, 100, &seconds, &nanoseconds));
    assert(seconds == 77 && nanoseconds == 88);
    assert(!lume_realtime_set(NULL, 0, 0, 0));
    assert(!lume_realtime_set(&clock, UINT64_MAX, 0, 0));
    assert(!lume_realtime_set(&clock, 0, UINT32_C(1000000000), 0));
    assert(!clock.valid);
    assert(lume_realtime_set(&clock, UINT64_C(1792800000), 999000000, UINT64_C(4294967295)));
    assert(lume_realtime_read(&clock, UINT64_C(4294967295), &seconds, &nanoseconds));
    assert(seconds == UINT64_C(1792800000) && nanoseconds == 999000000);
    assert(lume_realtime_read(&clock, UINT64_C(4294967296), &seconds, &nanoseconds));
    assert(seconds == UINT64_C(1792800001) && nanoseconds == 0);
    assert(lume_realtime_read(&clock, UINT64_C(4294972296), &seconds, &nanoseconds));
    assert(seconds == UINT64_C(1792800006) && nanoseconds == 0);
    assert(!lume_realtime_read(&clock, 0, &seconds, &nanoseconds));
    assert(!lume_realtime_read(NULL, 0, &seconds, &nanoseconds));
    assert(!lume_realtime_read(&clock, UINT64_MAX, NULL, &nanoseconds));
    assert(!lume_realtime_read(&clock, UINT64_MAX, &seconds, NULL));
    assert(!lume_realtime_set(&clock, UINT64_MAX, 0, 0));
    assert(lume_realtime_read(&clock, UINT64_C(4294967296), &seconds, &nanoseconds));
    assert(seconds == UINT64_C(1792800001) && nanoseconds == 0);
    assert(lume_realtime_set(&clock, INT64_MAX, 999000000, 0));
    assert(!lume_realtime_read(&clock, 1, &seconds, &nanoseconds));
    assert(lume_realtime_set(&clock, 0, 0, 0));
    assert(lume_realtime_read(&clock, UINT64_MAX, &seconds, &nanoseconds));
    assert(seconds == UINT64_MAX / 1000 && nanoseconds == (UINT64_MAX % 1000) * 1000000);
    clock = (struct lume_realtime){0};
    struct lume_realtime_request request = {.version = LUME_REALTIME_VERSION,
                                            .bytes = sizeof(request)};
    assert(lume_realtime_control(&clock, LUME_REALTIME_GET, &request, 12, false) == -LUME_EAGAIN);
    request.seconds = 1793628000;
    request.nanoseconds = 900000000;
    assert(lume_realtime_control(&clock, LUME_REALTIME_SET, &request, 20, false) == -LUME_EACCES);
    assert(!clock.valid);
    assert(!lume_realtime_control(&clock, LUME_REALTIME_SET, &request, 20, true));
    request.seconds = 0;
    request.nanoseconds = 0;
    assert(!lume_realtime_control(&clock, LUME_REALTIME_GET, &request, 170, false));
    assert(request.seconds == 1793628001 && request.nanoseconds == 50000000);
    request.flags = 1;
    assert(lume_realtime_control(&clock, LUME_REALTIME_GET, &request, 200, true) == -LUME_EINVAL);
    request.flags = 0;
    request.nanoseconds = 1000000000;
    assert(lume_realtime_control(&clock, LUME_REALTIME_SET, &request, 200, true) == -LUME_EINVAL);
    assert(clock.seconds == 1793628000 && clock.monotonic == 20);
    request.nanoseconds = 0;
    request.reserved = 1;
    assert(lume_realtime_control(&clock, LUME_REALTIME_SET, &request, 200, true) == -LUME_EINVAL);
    request.reserved = 0;
    request.bytes = 0;
    assert(lume_realtime_control(&clock, LUME_REALTIME_SET, &request, 200, true) == -LUME_EINVAL);
    puts("REALTIME_ANCHOR_PASS");
    return 0;
}
