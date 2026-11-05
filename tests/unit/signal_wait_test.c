/* SPDX-License-Identifier: BSD-3-Clause */
#include "signal_wait.h"
#include <assert.h>
#include <stdio.h>

static struct lume_signal_request request(uint32_t selected)
{
    return (struct lume_signal_request){.version = LUME_SIGNAL_VERSION,
                                        .bytes = sizeof(struct lume_signal_request),
                                        .mask = selected};
}

int main(void)
{
    const uint32_t selected = (UINT32_C(1) << 14) | (UINT32_C(1) << 23);
    uint32_t pending = selected | (UINT32_C(1) << 30);
    struct lume_signal_request r = request(selected);
    assert(signal_try_wait(&pending, selected, &r) == 0 && r.number == 14);
    assert(pending == ((UINT32_C(1) << 23) | (UINT32_C(1) << 30)));
    r = request(selected);
    assert(signal_try_wait(&pending, selected, &r) == 0 && r.number == 23);
    assert(pending == (UINT32_C(1) << 30));
    r = request(selected);
    assert(signal_try_wait(&pending, selected, &r) == -LUME_EAGAIN && !r.number);
    assert(pending == (UINT32_C(1) << 30));
    r = request(0);
    assert(signal_try_wait(&pending, UINT32_MAX, &r) == -LUME_EINVAL);
    const uint32_t invalid[] = {1, UINT32_C(1) << 9, UINT32_C(1) << 17};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
    {
        r = request(selected | invalid[i]);
        assert(signal_try_wait(&pending, UINT32_MAX, &r) == -LUME_EINVAL);
    }
    r = request(selected);
    assert(signal_try_wait(&pending, UINT32_C(1) << 14, &r) == -LUME_EINVAL);
    for (unsigned field = 0; field < 12; ++field)
    {
        r = request(selected);
        switch (field)
        {
        case 0:
            r.version++;
            break;
        case 1:
            r.bytes--;
            break;
        case 2:
            r.number = 1;
            break;
        case 3:
            r.flags = 1;
            break;
        case 4:
            r.handler = 1;
            break;
        case 5:
            r.restorer = 1;
            break;
        case 6:
            r.delay_ms = 1;
            break;
        case 7:
            r.interval_ms = 1;
            break;
        case 8:
            r.operation = 1;
            break;
        case 9:
            r.pid = 1;
            break;
        case 10:
            r.group = 1;
            break;
        default:
            r.mask = 1;
            break;
        }
        assert(signal_try_wait(&pending, UINT32_MAX, &r) == -LUME_EINVAL);
        assert(pending == (UINT32_C(1) << 30));
    }
    const unsigned ignored[] = {16, 19, 20, 23, 28};
    for (unsigned i = 0; i < sizeof(ignored) / sizeof(ignored[0]); ++i)
    {
        unsigned number = ignored[i];
        assert(!signal_pending_keep(number, 0, 0));
        assert(signal_pending_keep(number, 0, UINT32_C(1) << number));
        assert(!signal_pending_keep(number, 1, UINT32_MAX));
        assert(signal_pending_keep(number, 0x40040000, 0));
    }
    assert(!signal_pending_keep(0, 0, UINT32_MAX));
    assert(!signal_pending_keep(32, 0, UINT32_MAX));
    assert(signal_pending_keep(30, 0, 0));
    puts("PASS_SIGNAL_TRY_WAIT pending selection, blocked masks and ignored dispositions");
    return 0;
}
