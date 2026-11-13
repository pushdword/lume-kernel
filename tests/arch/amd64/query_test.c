#include "lume/early.h"
#include "lume/smp.h"
#include "lume/heap.h"
#include "lume/elf.h"
#include "lume/vm.h"
#include "lume/abi.h"
#include "lume/thread.h"
#include "lume/amd64_query.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned cpu, cpu_reads;
static uint64_t raw;

unsigned lume_cpu_index(void)
{
    ++cpu_reads;
    return cpu;
}

uint64_t lume_tsc_read(void)
{
    raw += 2000;
    return raw;
}

uint64_t lume_q35_tsc_frequency(void)
{
    return 2000000000;
}

static unsigned clock_reads;

uint64_t lume_clock_read(void)
{
    ++clock_reads;
    return UINT64_C(0x123456789ab);
}

unsigned lume_cpu_count(void)
{
    return LUME_CPU_LIMIT;
}

_Noreturn void lume_fail(const char *message)
{
    fprintf(stderr, "%s\n", message);
    abort();
}

static bool pipe_ready;
static unsigned pipe_calls;
static bool expected_writing;
static int64_t pipe_result;

bool lume_task_pipe_try(unsigned owner, uint64_t fd, uint64_t address, uint64_t bytes, bool writing,
                        int64_t *result)
{
    assert(owner == 7 && fd == 3 && address == LUME_USER_BASE + 8192 && bytes == 17);
    assert(writing == expected_writing);
    ++pipe_calls;
    *result = pipe_result;
    return pipe_ready;
}

bool lume_task_pipe_terminal_request(unsigned owner, uint64_t address)
{
    assert(owner == 7 && address == LUME_USER_BASE + 8192);
    ++pipe_calls;
    return pipe_ready;
}

#include "../../../arch/amd64/context/query.h"

static void frame(uint64_t registers[22], uint64_t number)
{
    for (unsigned i = 0; i < 22; ++i)
        registers[i] = UINT64_C(0xa1a2000000000000) + i;
    registers[7] = number;
    registers[16] = 0;
    registers[17] = LUME_USER_BASE + 4096;
    registers[18] = 0x23;
    registers[19] = UINT64_MAX;
    registers[20] = LUME_USER_STACK;
    registers[21] = 0;
}

static void denied(uint64_t registers[22])
{
    uint64_t before[22];
    memcpy(before, registers, sizeof(before));
    assert(!lume_amd64_query_try(registers));
    assert(!memcmp(before, registers, sizeof(before)));
}

static void accepted(uint64_t registers[22], uint64_t expected)
{
    uint64_t before[22];
    memcpy(before, registers, sizeof(before));
    assert(lume_amd64_query_try(registers));
    for (unsigned i = 0; i < 22; ++i)
        if (i == 8)
            assert(registers[i] == expected);
        else if (i == 19)
            assert(registers[i] == ((before[i] & UINT64_C(0x240dd5)) | 0x202));
        else if (i == 21)
            assert(registers[i] == 0x1b);
        else
            assert(registers[i] == before[i]);
}

static void scalar_queries(void)
{
    uint64_t registers[22];
    struct lume_query_account account;
    for (cpu = 0; cpu < LUME_CPU_LIMIT; ++cpu)
    {
        lume_arch_query_drain(&account);
        for (unsigned kind = 0; kind < 3; ++kind)
        {
            unsigned flag = LUME_QUERY_CLOCK << kind;
            unsigned number = kind == 0 ? LUME_SYS_CLOCK : kind == 1 ? LUME_SYS_ABI : LUME_SYS_CPU;
            lume_arch_query_publish(7, 3, 99, flag);
            frame(registers, number);
            if (kind == 2)
            {
                registers[8] = registers[9] = registers[11] = 0;
                accepted(registers, LUME_CPU_LIMIT);
                registers[8] = 1;
                accepted(registers, cpu);
                registers[8] = 2;
                accepted(registers, LUME_SCHED_QUANTUM);
                registers[8] = 3;
                denied(registers);
                registers[8] = 0;
                registers[9] = 1;
                denied(registers);
                registers[9] = 0;
                registers[11] = 1;
                denied(registers);
            }
            else
                accepted(registers, kind == 0 ? UINT64_C(0x123456789ab) : LUME_ABI_VERSION);
            frame(registers, number);
            if (kind == 2)
                registers[8] = registers[9] = registers[11] = 0;
            lume_arch_query_revoke(2);
            denied(registers);
            lume_arch_query_drain(&account);
            assert(account.calls == (kind == 2 ? 3 : 1));
            assert(account.mask == UINT64_C(1) << number);
            lume_arch_query_publish(7, 3, 99, LUME_QUERY_PID);
            frame(registers, number);
            if (kind == 2)
                registers[8] = registers[9] = registers[11] = 0;
            unsigned before = clock_reads;
            denied(registers);
            assert(clock_reads == before);
        }
    }
    cpu = 0;
    assert(clock_reads == LUME_CPU_LIMIT);
}

int main(void)
{
    uint64_t registers[22];
    struct lume_query_account account;
    for (cpu = 0; cpu < LUME_CPU_LIMIT; ++cpu)
        lume_amd64_query_start(true);
    cpu = 0;
    frame(registers, LUME_SYS_GETPID);
    denied(registers);
    lume_arch_query_publish(7, 3, 0x1408, LUME_QUERY_PID | LUME_QUERY_TID);
    accepted(registers, 3);
    frame(registers, LUME_SYS_THREAD);
    registers[8] = LUME_THREAD_SELF;
    registers[9] = registers[11] = 0;
    accepted(registers, 0x1408);
    lume_arch_query_drain(&account);
    assert(account.owner == 7 && account.calls == 2 && account.ticks == 2);
    assert(account.mask == ((UINT64_C(1) << LUME_SYS_GETPID) | (UINT64_C(1) << LUME_SYS_THREAD)));
    denied(registers);
    lume_arch_query_drain(&account);
    assert(!account.calls && !account.ticks && !account.mask);

    for (unsigned mask = 0; mask < 4; ++mask)
    {
        lume_arch_query_publish(7, 3, 0x1408, mask);
        frame(registers, LUME_SYS_GETPID);
        if (mask & LUME_QUERY_PID)
            accepted(registers, 3);
        else
            denied(registers);
        frame(registers, LUME_SYS_THREAD);
        registers[8] = LUME_THREAD_SELF;
        registers[9] = registers[11] = 0;
        if (mask & LUME_QUERY_TID)
            accepted(registers, 0x1408);
        else
            denied(registers);
        lume_arch_query_drain(&account);
    }

    lume_arch_query_publish(7, 3, 0x1408, 3);
    lume_arch_query_revoke(2);
    frame(registers, LUME_SYS_GETPID);
    denied(registers);
    lume_arch_query_publish(7, 3, 0x1408, 3);
    const uint64_t invalid[] = {0, LUME_USER_BASE - 1, LUME_USER_LIMIT,
                                UINT64_C(0xffff800000000000), UINT64_MAX};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
    {
        frame(registers, LUME_SYS_GETPID);
        registers[17] = invalid[i];
        denied(registers);
        frame(registers, LUME_SYS_GETPID);
        registers[20] = invalid[i];
        denied(registers);
    }
    frame(registers, LUME_SYS_GETPID);
    registers[18] = 8;
    denied(registers);
    frame(registers, LUME_SYS_WRITE);
    denied(registers);
    frame(registers, LUME_SYS_THREAD);
    registers[8] = LUME_THREAD_SELF;
    registers[9] = 1;
    registers[11] = 0;
    denied(registers);
    registers[9] = 0;
    registers[11] = 1;
    denied(registers);
    registers[11] = 0;
    registers[8] = LUME_THREAD_CREATE;
    denied(registers);
    const uint64_t valid[] = {LUME_USER_BASE,
                              LUME_USER_LIMIT - 1,
                              LUME_LARGE_ELF_BASE,
                              LUME_LARGE_ELF_LIMIT - 1,
                              LUME_HEAP_EXTENDED_BASE,
                              LUME_HEAP_EXTENDED_LIMIT - 1,
                              LUME_VM_BASE,
                              LUME_VM_LIMIT - 1,
                              LUME_THREAD_VM_BASE,
                              LUME_THREAD_VM_BASE + 256 * LUME_THREAD_STRIDE - 1};
    for (unsigned i = 0; i < sizeof(valid) / sizeof(valid[0]); ++i)
    {
        frame(registers, LUME_SYS_GETPID);
        registers[17] = registers[20] = valid[i];
        accepted(registers, 3);
    }
    lume_arch_query_drain(&account);
    assert(account.owner == 7 && account.calls == sizeof(valid) / sizeof(valid[0]));
    cpu = LUME_CPU_LIMIT;
    denied(registers);

    cpu = 1;
    frame(registers, LUME_SYS_GETPID);
    denied(registers);
    lume_arch_query_publish(8, 5, 0x1809, 3);
    accepted(registers, 5);
    lume_arch_query_drain(&account);
    assert(account.owner == 8 && account.calls == 1);
    cpu = 0;
    lume_arch_query_publish(7, 3, 0x1408, 3);
    accepted(registers, 3);
    lume_arch_query_drain(&account);
    assert(account.owner == 7 && account.calls == 1);
    lume_arch_query_publish(9, 6, 0x220a, 3);
    frame(registers, LUME_SYS_GETPID);
    accepted(registers, 6);
    lume_arch_query_drain(&account);
    assert(account.owner == 9 && account.calls == 1);
    lume_arch_query_publish(9, 6, 0x220a, 3);
    accepted(registers, 6);
    cpu = 1;
    lume_arch_query_publish(10, 6, 0x220b, 3);
    accepted(registers, 6);
    cpu = 2;
    lume_arch_query_publish(11, 7, 0x220c, 3);
    lume_arch_query_revoke(5);
    cpu = 0;
    denied(registers);
    cpu = 1;
    denied(registers);
    cpu = 2;
    frame(registers, LUME_SYS_GETPID);
    accepted(registers, 7);
    lume_arch_query_drain(&account);
    assert(account.owner == 11 && account.calls == 1);
    cpu = 0;
    lume_arch_query_drain(&account);
    assert(account.owner == 9 && account.calls == 1);
    lume_arch_query_publish(9, 6, 0x220a, 3);
    accepted(registers, 6);
    cpu = 1;
    denied(registers);
    lume_arch_query_drain(&account);
    assert(account.owner == 10 && account.calls == 1);
    cpu = 0;
    lume_arch_query_drain(&account);
    lume_arch_query_publish(LUME_TASK_LIMIT, 0, 0, 0);
    denied(registers);

    lume_amd64_query_start(false);
    lume_arch_query_publish(9, 6, 0x220a, 3);
    denied(registers);
    lume_amd64_query_start(true);
    lume_arch_query_publish(12, 8, 0x300d, 3);
    frame(registers, LUME_SYS_GETPID);
    registers[16] = 1;
    uint64_t before_raw = raw;
    unsigned before_reads = cpu_reads;
    cpu = LUME_CPU_LIMIT;
    for (unsigned i = 0; i < 65; ++i)
        accepted(registers, 8);
    assert(cpu_reads == before_reads && raw - before_raw == 8000);
    cpu = 0;
    lume_arch_query_drain(&account);
    assert(account.owner == 12 && account.calls == 65 && account.ticks == 65);
    lume_arch_query_publish(13, 9, 0x300e, 3);
    before_raw = raw;
    accepted(registers, 9);
    lume_arch_query_drain(&account);
    assert(account.owner == 13 && account.calls == 1 && account.ticks == 1);
    assert(raw - before_raw == 4000);
    registers[16] = LUME_CPU_LIMIT + 1;
    denied(registers);
    cpu = 0;
    lume_arch_query_drain(&account);
    lume_arch_query_publish(7, 3, 0x1408, LUME_QUERY_PIPE);
    for (unsigned writing = 0; writing < 2; ++writing)
    {
        frame(registers, writing ? LUME_SYS_WRITE : LUME_SYS_READ);
        registers[8] = 3;
        registers[9] = LUME_USER_BASE + 8192;
        registers[11] = 17;
        expected_writing = writing != 0;
        pipe_ready = true;
        pipe_result = 17;
        accepted(registers, 17);
        frame(registers, writing ? LUME_SYS_WRITE : LUME_SYS_READ);
        registers[8] = 3;
        registers[9] = LUME_USER_BASE + 8192;
        registers[11] = 17;
        pipe_ready = false;
        denied(registers);
        pipe_ready = true;
        pipe_result = -LUME_EAGAIN;
        accepted(registers, (uint64_t)-LUME_EAGAIN);
    }
    assert(pipe_calls == 6);
    lume_arch_query_drain(&account);
    assert(account.owner == 7 && account.calls == 4);
    assert(account.mask == ((UINT64_C(1) << LUME_SYS_READ) | (UINT64_C(1) << LUME_SYS_WRITE)));
    lume_arch_query_publish(7, 3, 0x1408, LUME_QUERY_PID);
    frame(registers, LUME_SYS_WRITE);
    denied(registers);
    assert(pipe_calls == 6);
    lume_arch_query_publish(7, 3, 0x1408, LUME_QUERY_PIPE);
    frame(registers, LUME_SYS_TTY);
    registers[8] = LUME_TTY_GET;
    registers[9] = LUME_USER_BASE + 8192;
    registers[11] = sizeof(struct lume_tty_request);
    pipe_ready = false;
    denied(registers);
    pipe_ready = true;
    accepted(registers, (uint64_t)-25);
    registers[8] = LUME_TTY_SET;
    denied(registers);
    registers[8] = LUME_TTY_GET;
    registers[11]--;
    denied(registers);
    lume_arch_query_drain(&account);
    assert(account.calls == 1 && (account.mask & (UINT64_C(1) << LUME_SYS_TTY)));
    lume_arch_query_publish(7, 3, 0x1408, LUME_QUERY_PID);
    frame(registers, LUME_SYS_TTY);
    registers[8] = LUME_TTY_GET;
    registers[9] = LUME_USER_BASE + 8192;
    registers[11] = sizeof(struct lume_tty_request);
    denied(registers);
    scalar_queries();
    puts(
        "PASS_AMD64_QUERY epochs, CPU ownership, sampled accounting, eligibility, bounds and native frame");
    return 0;
}
