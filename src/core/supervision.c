#include "lume/early.h"
#include "lume/process.h"
#include "supervision_bus.h"

static struct supervision_bus bus;

int64_t lume_supervision_call(unsigned pid, unsigned operation,
                              struct lume_supervision_request *request)
{
    return supervision_call(&bus, pid, operation, lume_task_capable(pid, LUME_CAP_SUPERVISOR),
                            lume_task_capable(pid, LUME_CAP_SERVICE_CONTROL), request);
}

void lume_supervision_release(unsigned pid)
{
    supervision_release(&bus, pid);
}

bool lume_supervision_manager(unsigned pid)
{
    return pid && bus.manager == pid && lume_task_capable(pid, LUME_CAP_SUPERVISOR);
}
