#ifndef LUME_AMD64_CONTEXT_QUERY_H
#define LUME_AMD64_CONTEXT_QUERY_H

static bool user_context_address(uint64_t address)
{
    return (address >= LUME_USER_BASE && address < LUME_USER_LIMIT) ||
           (address >= LUME_LARGE_ELF_BASE && address < LUME_LARGE_ELF_LIMIT) ||
           (address >= LUME_HEAP_EXTENDED_BASE && address < LUME_HEAP_EXTENDED_LIMIT) ||
           lume_vm_address(address);
}

struct amd64_query_cache
{
    _Alignas(128) uint64_t epoch;
    uint64_t pid, tid, calls, raw_ticks, mask, sample_raw;
    unsigned owner, allowed;
    bool enabled;
};

static struct amd64_query_cache query_cpus[LUME_CPU_LIMIT];

static struct
{
    _Alignas(128) uint64_t value;
} query_epochs[LUME_TASK_LIMIT];

_Static_assert(sizeof(struct amd64_query_cache) == 128, "query CPU cache size");
_Static_assert(LUME_SYS_GETPID == LUME_AMD64_QUERY_SYSCALL_PID, "PID query syscall number");
_Static_assert(LUME_SYS_THREAD == LUME_AMD64_QUERY_SYSCALL_THREAD, "thread query syscall number");

static uint64_t query_epoch(unsigned process)
{
    return __atomic_load_n(&query_epochs[process].value, __ATOMIC_ACQUIRE) + 1;
}

void lume_arch_query_revoke(unsigned process)
{
    if (process >= LUME_TASK_LIMIT)
        lume_fail("query revocation process");
    uint64_t revision = __atomic_add_fetch(&query_epochs[process].value, 1, __ATOMIC_ACQ_REL);
    if (revision == UINT64_MAX)
        lume_fail("query revision exhausted");
}

static uint64_t query_stamp(void)
{
    __asm__ volatile("lfence" : : : "memory");
    return lume_tsc_read();
}

void lume_amd64_query_start(bool enabled)
{
    struct amd64_query_cache *cache = query_cpus + lume_cpu_index();
    *cache = (struct amd64_query_cache){.owner = LUME_TASK_LIMIT, .enabled = enabled};
}

void lume_arch_query_publish(unsigned owner, unsigned pid, uint64_t tid, unsigned allowed)
{
    struct amd64_query_cache *cache = query_cpus + lume_cpu_index();
    cache->epoch = 0;
    if (cache->calls && cache->owner != owner)
        lume_fail("query accounting owner changed");
    cache->owner = owner;
    cache->pid = pid;
    cache->tid = tid;
    cache->allowed = cache->enabled ? allowed : 0;
    if (cache->allowed && pid && pid <= LUME_TASK_LIMIT)
        cache->epoch = query_epoch(pid - 1);
}

void lume_arch_query_drain(struct lume_query_account *out)
{
    struct amd64_query_cache *cache = query_cpus + lume_cpu_index();
    cache->epoch = 0;
    uint64_t frequency = lume_q35_tsc_frequency();
    uint64_t ticks = frequency ? cache->raw_ticks / frequency * UINT64_C(1000000) +
                                     cache->raw_ticks % frequency * UINT64_C(1000000) / frequency
                               : 0;
    *out = (struct lume_query_account){cache->owner, cache->calls, ticks, cache->mask};
    cache->calls = cache->raw_ticks = cache->mask = 0;
}

/* IRQs stay masked throughout the query. Only the local CPU owns this cache.
 * Process state changes revoke its snapshots before identity or eligibility changes.
 * Revisions persist across task slot reuse. Unrelated kernel work preserves them. */
bool lume_amd64_query_try(uint64_t *registers)
{
    uint64_t number = registers[7];
    bool pid = number == LUME_SYS_GETPID;
    bool tid = number == LUME_SYS_THREAD && registers[8] == LUME_THREAD_SELF && !registers[9] &&
               !registers[11];
    if (!pid && !tid)
        return false;
    uint64_t marker = registers[16];
    if (marker > LUME_CPU_LIMIT)
        return false;
    unsigned cpu = marker ? (unsigned)marker - 1 : lume_cpu_index();
    if (cpu >= LUME_CPU_LIMIT)
        return false;
    struct amd64_query_cache *cache = query_cpus + cpu;
    bool sample = !(cache->calls & 63u);
    uint64_t began = sample ? query_stamp() : 0;
    unsigned required = pid ? LUME_QUERY_PID : LUME_QUERY_TID;
    if (!(cache->allowed & required) || !cache->epoch || cache->owner >= LUME_TASK_LIMIT ||
        !cache->pid || cache->pid > LUME_TASK_LIMIT ||
        cache->epoch != query_epoch(cache->pid - 1) || registers[18] != 0x23 ||
        !user_context_address(registers[17]) || !user_context_address(registers[20]))
        return false;
    registers[8] = pid ? cache->pid : cache->tid;
    registers[19] = (registers[19] & UINT64_C(0x240dd5)) | 0x202;
    registers[21] = 0x1b;
    ++cache->calls;
    cache->mask |= UINT64_C(1) << number;
    /* One measured call estimates each batch of 64 fast queries. The first
     * call after a drain measures the residual batch for its current owner. */
    if (sample)
        cache->sample_raw = query_stamp() - began;
    cache->raw_ticks += cache->sample_raw;
    return true;
}

#endif
