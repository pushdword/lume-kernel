#include "lume/worker.h"

__attribute__((weak)) void lume_task_memory_ready(unsigned owner)
{
    (void)owner;
}

__attribute__((weak)) unsigned lume_task_memory_owner(void)
{
    return 1;
}

__attribute__((weak)) void lume_task_memory_prepare(void (*run)(void *), void *context)
{
    run(context);
}

__attribute__((weak)) unsigned lume_worker_current(void)
{
    return LUME_WORKER_LIMIT;
}

__attribute__((weak)) bool lume_worker_pause_ms(unsigned milliseconds)
{
    (void)milliseconds;
    return false;
}

__attribute__((weak)) bool lume_worker_wait_bits(volatile uint32_t *value, uint32_t mask,
                                                 uint64_t deadline)
{
    (void)value;
    (void)mask;
    (void)deadline;
    return false;
}

__attribute__((weak)) void lume_worker_checkpoint(unsigned bytes)
{
    (void)bytes;
}
