#include "lume/early.h"
#include "lume/process.h"
#include "lume/smp.h"
#include "supervision_bus.h"

static struct supervision_bus bus;

int64_t lume_supervision_call(unsigned pid, unsigned operation,
                              struct lume_supervision_request *request)
{
    int64_t result =
        supervision_call(&bus, pid, operation, lume_task_capable(pid, LUME_CAP_SUPERVISOR),
                         lume_task_capable(pid, LUME_CAP_SERVICE_CONTROL), request);
    if (!result &&
        (operation == LUME_SV_SUBMIT || operation == LUME_SV_REPLY || operation == LUME_SV_CANCEL))
        lume_smp_wake();
    return result;
}

int lume_supervision_ready(unsigned pid, uint64_t sequence)
{
    return supervision_ready(&bus, pid, sequence, lume_task_capable(pid, LUME_CAP_SUPERVISOR));
}

void lume_supervision_release(unsigned pid)
{
    bool changed = bus.manager == pid;
    for (unsigned i = 0; i < LUME_SUPERVISION_COUNT; ++i)
        changed |= bus.slot[i].owner == pid;
    supervision_release(&bus, pid);
    if (changed)
        lume_smp_wake();
}

bool lume_supervision_manager(unsigned pid)
{
    return pid && bus.manager == pid && lume_task_capable(pid, LUME_CAP_SUPERVISOR);
}
