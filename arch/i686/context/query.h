#ifndef LUME_I686_CONTEXT_QUERY_H
#define LUME_I686_CONTEXT_QUERY_H

static struct
{
    uint64_t tid, calls, raw_ticks, mask, root;
    unsigned owner, pid, allowed, samples;
} query_cache;

struct __attribute__((packed, may_alias)) query_request
{
    uint32_t words[8];
};

_Static_assert(LUME_SYS_GETPID == LUME_I686_QUERY_SYSCALL_PID, "PID query syscall number");
_Static_assert(LUME_SYS_THREAD == LUME_I686_QUERY_SYSCALL_THREAD, "thread query syscall number");

static bool query_decode(struct lume_context *frame)
{
    frame->syscall_pc = (uint32_t)frame->pc;
    if (frame->raw[7] != LUME_SYS_GETPID)
    {
        frame->reg[0] = (uint64_t)-LUME_ENOSYS;
        return false;
    }
    frame->reg[8] = LUME_SYS_GETPID;
    frame->reg[0] = frame->reg[1] = frame->reg[2] = 0;
    return true;
}

static uint64_t query_stamp(void)
{
    __asm__ volatile("" : : : "memory");
    uint64_t stamp = lume_tsc_read();
    __asm__ volatile("" : : : "memory");
    return stamp;
}

void lume_i686_query_start(void)
{
    query_cache.owner = LUME_TASK_LIMIT;
    query_cache.allowed = 0;
    query_cache.calls = query_cache.raw_ticks = query_cache.mask = 0;
    query_cache.samples = 0;
}

void lume_arch_query_revoke(unsigned process)
{
    if (process >= LUME_TASK_LIMIT)
        lume_fail("query revocation process");
    if (query_cache.pid == process + 1)
        query_cache.allowed = 0;
}

void lume_arch_query_publish(unsigned owner, unsigned pid, uint64_t tid, unsigned allowed)
{
    query_cache.allowed = 0;
    if (query_cache.calls && query_cache.owner != owner)
        lume_fail("query accounting owner changed");
    query_cache.owner = owner;
    query_cache.pid = pid;
    query_cache.tid = tid;
    query_cache.root = owner < LUME_TASK_LIMIT ? lume_mmu_task_root(owner) : 0;
    if (owner < LUME_TASK_LIMIT && pid && pid <= LUME_TASK_LIMIT && query_cache.root)
        query_cache.allowed = allowed;
}

void lume_arch_query_drain(struct lume_query_account *out)
{
    query_cache.allowed = 0;
    uint64_t frequency = lume_q35_tsc_frequency();
    uint64_t raw = query_cache.samples
                       ? query_cache.raw_ticks / query_cache.samples * query_cache.calls +
                             query_cache.raw_ticks % query_cache.samples * query_cache.calls /
                                 query_cache.samples
                       : 0;
    uint64_t ticks = frequency ? raw / frequency * UINT64_C(1000000) +
                                     raw % frequency * UINT64_C(1000000) / frequency
                               : 0;
    *out =
        (struct lume_query_account){query_cache.owner, query_cache.calls, ticks, query_cache.mask};
    query_cache.calls = query_cache.raw_ticks = query_cache.mask = 0;
    query_cache.samples = 0;
}

/* The uniprocessor enters with IRQs masked. Published identity and page tables
 * remain stable until the query returns. Missing mappings use the normal fault path. */
bool lume_i686_query_try(uint32_t *registers)
{
    if (!query_cache.allowed || (registers[8] != 128 && registers[8] != 129) ||
        registers[11] != 0x1b || registers[10] < LUME_USER_BASE ||
        registers[10] >= LUME_USER_LIMIT || registers[13] < LUME_USER_BASE ||
        registers[13] >= LUME_USER_LIMIT)
        return false;
    bool sampled = !(query_cache.calls & 63u);
    uint64_t began = sampled ? query_stamp() : 0;
    uint32_t number = registers[7];
    uint64_t result;
    if (registers[8] == 129)
    {
        if (number != LUME_SYS_GETPID || !(query_cache.allowed & LUME_QUERY_PID))
            return false;
        result = query_cache.pid;
    }
    else
    {
        uint32_t pointer = registers[7];
        if (pointer < LUME_USER_BASE || pointer > LUME_USER_LIMIT - 32)
            return false;
        uint64_t physical;
        if (!lume_mmu_translate(query_cache.root, pointer, false, true, false, &physical) ||
            ((pointer & 4095u) > 4096u - 32 &&
             !lume_mmu_translate(query_cache.root, (pointer & ~UINT32_C(4095)) + 4096, false, true,
                                 false, &physical)))
            return false;
        const volatile struct query_request *request = (const void *)(uintptr_t)pointer;
        number = request->words[0];
        if (request->words[1])
            return false;
        if (number == LUME_SYS_GETPID && (query_cache.allowed & LUME_QUERY_PID))
            result = query_cache.pid;
        else if (number == LUME_SYS_THREAD && (query_cache.allowed & LUME_QUERY_TID) &&
                 request->words[2] == LUME_THREAD_SELF && !request->words[3] &&
                 !request->words[4] && !request->words[5] && !request->words[6] &&
                 !request->words[7])
            result = query_cache.tid;
        else
            return false;
    }
    registers[7] = (uint32_t)result;
    registers[5] = (uint32_t)(result >> 32);
    registers[12] = (registers[12] & UINT32_C(0x240dd5)) | 0x202;
    registers[14] = 0x23;
    ++query_cache.calls;
    query_cache.mask |= UINT64_C(1) << number;
    if (sampled)
    {
        query_cache.raw_ticks += query_stamp() - began;
        ++query_cache.samples;
    }
    return true;
}
#endif
