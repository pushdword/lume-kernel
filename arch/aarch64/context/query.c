#include "lume/aarch64_query.h"
#include "lume/abi.h"
#include "lume/thread.h"
#include "lume/smp.h"
#include "lume/counter.h"

struct query_cache
{
    _Alignas(128) uint64_t epoch;
    uint64_t pid, tid, calls, ticks, mask, sample_ticks;
    unsigned owner, allowed;
};

static struct query_cache query_cpus[LUME_CPU_LIMIT];

static struct
{
    _Alignas(128) uint64_t value;
} query_epochs[LUME_TASK_LIMIT];

_Static_assert(sizeof(struct query_cache) == 128, "query CPU cache size");

static uint64_t query_epoch(unsigned process)
{
    return __atomic_load_n(&query_epochs[process].value, __ATOMIC_ACQUIRE) + 1;
}

void lume_arch_query_revoke(unsigned process)
{
    if (process >= LUME_TASK_LIMIT)
        lume_fail("query revocation process");
    if (__atomic_add_fetch(&query_epochs[process].value, 1, __ATOMIC_ACQ_REL) == UINT64_MAX)
        lume_fail("query revision exhausted");
}

void lume_arch_query_publish(unsigned owner, unsigned pid, uint64_t tid, unsigned allowed)
{
    struct query_cache *cache = query_cpus + lume_cpu_index();
    cache->epoch = 0;
    if (cache->calls && cache->owner != owner)
        lume_fail("query accounting owner changed");
    cache->owner = owner;
    cache->pid = pid;
    cache->tid = tid;
    cache->allowed = allowed;
    if (allowed && pid && pid <= LUME_TASK_LIMIT)
        cache->epoch = query_epoch(pid - 1);
}

void lume_arch_query_drain(struct lume_query_account *out)
{
    struct query_cache *cache = query_cpus + lume_cpu_index();
    cache->epoch = 0;
    *out = (struct lume_query_account){cache->owner, cache->calls, cache->ticks, cache->mask};
    cache->calls = cache->ticks = cache->mask = 0;
}

/* IRQs remain masked and this CPU owns its published snapshot. Other CPUs
 * revoke the process epoch before changing identity or query eligibility. */
bool lume_aarch64_query_try(struct lume_context *frame, uint64_t cause)
{
    if (!lume_arch_is_syscall(cause, true) || (frame->spsr & 15))
        return false;
    uint64_t number = frame->x[8];
    bool pid = number == LUME_SYS_GETPID;
    bool tid = number == LUME_SYS_THREAD && frame->x[0] == LUME_THREAD_SELF && !frame->x[1] &&
               !frame->x[2];
    if (!pid && !tid)
        return false;
    unsigned cpu = lume_cpu_index();
    if (cpu >= LUME_CPU_LIMIT)
        return false;
    struct query_cache *cache = query_cpus + cpu;
    bool sample = !(cache->calls & 63u);
    uint64_t began = sample ? lume_counter_read() : 0;
    unsigned required = pid ? LUME_QUERY_PID : LUME_QUERY_TID;
    if (!(cache->allowed & required) || !cache->epoch || cache->owner >= LUME_TASK_LIMIT ||
        !cache->pid || cache->pid > LUME_TASK_LIMIT ||
        cache->epoch != query_epoch((unsigned)cache->pid - 1))
        return false;
    frame->x[0] = pid ? cache->pid : cache->tid;
    ++cache->calls;
    cache->mask |= UINT64_C(1) << number;
    if (sample)
        cache->sample_ticks = lume_counter_read() - began;
    cache->ticks += cache->sample_ticks;
    return true;
}

/* Pipe transfers retain their resource and address-space checks under the
 * kernel lock. A completed short transfer does not need a scheduler turn. */
bool lume_aarch64_pipe_try(struct lume_context *frame, uint64_t cause, struct lume_context **next)
{
    if (!lume_arch_is_syscall(cause, true) || (frame->spsr & 15))
        return false;
    uint64_t number = frame->x[8];
    if (number != LUME_SYS_READ && number != LUME_SYS_WRITE)
        return false;
    unsigned cpu = lume_cpu_index();
    if (cpu >= LUME_CPU_LIMIT)
        return false;
    struct query_cache *cache = query_cpus + cpu;
    if (!(cache->allowed & LUME_QUERY_PIPE) || !cache->epoch || cache->owner >= LUME_TASK_LIMIT ||
        !cache->pid || cache->pid > LUME_TASK_LIMIT ||
        cache->epoch != query_epoch((unsigned)cache->pid - 1))
        return false;
    uint64_t began = lume_counter_read();
    int64_t result;
    if (!lume_task_pipe_try(cache->owner, frame->x[0], frame->x[1], frame->x[2],
                            number == LUME_SYS_WRITE, &result))
        return false;
    frame->x[0] = (uint64_t)result;
    *next = frame;
    ++cache->calls;
    cache->mask |= UINT64_C(1) << number;
    cache->ticks += lume_counter_read() - began;
    return true;
}
