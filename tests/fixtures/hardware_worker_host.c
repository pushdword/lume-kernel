#include "lume/worker.h"

void (*lume_hardware_compute_hook)(void);

__attribute__((weak)) void lume_task_memory_prepare(void (*run)(void *), void *context)
{
    run(context);
}

__attribute__((weak)) void lume_worker_compute(void (*run)(void *), void *context)
{
    if (lume_hardware_compute_hook)
    {
        void (*hook)(void) = lume_hardware_compute_hook;
        lume_hardware_compute_hook = 0;
        hook();
    }
    run(context);
}
