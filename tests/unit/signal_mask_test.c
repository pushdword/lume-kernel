#include "signal_mask.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    uint32_t creator = UINT32_C(1) << 30, worker = creator, sibling = creator;
    const uint32_t second = UINT32_C(1) << 31;
    struct lume_signal_request request = {.flags = 1, .operation = 1, .mask = second};
    assert(!signal_mask_update(&worker, second, 41, &request));
    assert(request.mask == creator && request.delay_ms == 41 && !request.interval_ms);
    assert(worker == (creator | second) && sibling == creator);
    request = (struct lume_signal_request){.flags = 1, .operation = 2, .mask = creator};
    assert(!signal_mask_update(&worker, second, 42, &request));
    assert(request.mask == (creator | second) && worker == second && creator != worker);
    request = (struct lume_signal_request){.flags = 1, .operation = 0, .mask = UINT32_MAX};
    assert(!signal_mask_update(&worker, second, 43, &request));
    assert(!(worker & LUME_SIGNAL_UNMASKABLE));
    request = (struct lume_signal_request){.operation = UINT32_MAX};
    uint32_t before = worker;
    assert(!signal_mask_update(&worker, second, 44, &request));
    assert(request.mask == before && worker == before);
    request = (struct lume_signal_request){.flags = 2, .mask = second};
    struct lume_signal_request unchanged = request;
    assert(signal_mask_update(&worker, second, 45, &request) == -LUME_EINVAL);
    assert(worker == before && !memcmp(&request, &unchanged, sizeof(request)));
    request = (struct lume_signal_request){.flags = 1, .operation = 3, .mask = second};
    unchanged = request;
    assert(signal_mask_update(&worker, second, 46, &request) == -LUME_EINVAL);
    assert(worker == before && !memcmp(&request, &unchanged, sizeof(request)));
    request = (struct lume_signal_request){.flags = 1, .operation = 0};
    assert(!signal_mask_update(&worker, second, 47, &request));
    assert(!worker && request.interval_ms == second && sibling == creator);
    puts(
        "PASS signal masks, independent state, pending bits, unmaskable signals and invalid requests");
    return 0;
}
