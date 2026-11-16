#include "lume/worker.h"

__attribute__((weak)) void lume_worker_checkpoint(unsigned bytes)
{
    (void)bytes;
}

__attribute__((weak)) void lume_worker_domain(void (*run)(void *), void *context)
{
    run(context);
}
