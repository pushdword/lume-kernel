#include "lume/early.h"
#include "lume/abi.h"
#include "lume/thread.h"
#include "lume/context.h"
#include "lume/i686_query.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#define BASE UINT32_C(0x41000000)
static uint64_t raw, raw_step = 2000, frequency = 2000000000;
static unsigned translations, frequency_reads;
static bool user_page[3] = {true, true, false};
static bool writable_page[3] = {true, true, false};
static bool terminal_handled;
static unsigned terminal_calls;
static unsigned pipe_calls;
static bool pipe_handled;
static int64_t pipe_result;

bool lume_task_pipe_try(unsigned owner, uint64_t fd, uint64_t address, uint64_t bytes, bool writing,
                        int64_t *result)
{
    assert(owner == 7 && fd == 9 && address == BASE + 64 && bytes == 17 && writing);
    ++pipe_calls;
    *result = pipe_result;
    return pipe_handled;
}

bool lume_task_pipe_terminal_query(unsigned owner, uint64_t fd)
{
    assert(owner == 7 && fd == 9);
    ++terminal_calls;
    return terminal_handled;
}

uint64_t lume_tsc_read(void)
{
    raw += raw_step;
    return raw;
}

uint64_t lume_q35_tsc_frequency(void)
{
    ++frequency_reads;
    return frequency;
}

uint64_t lume_mmu_task_root(unsigned owner)
{
    assert(owner < LUME_TASK_LIMIT);
    return UINT64_C(0x123000);
}

bool lume_mmu_translate(uint64_t root, uint64_t address, bool write, bool user, bool execute,
                        uint64_t *physical)
{
    ++translations;
    assert(root == 0x123000 && user && !execute);
    if (address < BASE || address - BASE >= 12288 ||
        (!user_page[(address - BASE) / 4096] || (write && !writable_page[(address - BASE) / 4096])))
        return false;
    *physical = address;
    return true;
}

_Noreturn void lume_fail(const char *reason)
{
    fprintf(stderr, "%s\n", reason);
    abort();
}

#include "../../../arch/i686/context/query.h"

static void frame(uint32_t registers[15], unsigned vector, uint32_t argument)
{
    for (unsigned i = 0; i < 15; ++i)
        registers[i] = UINT32_C(0xa1a20000) + i;
    registers[7] = argument;
    registers[8] = vector;
    registers[10] = LUME_USER_BASE + 4096;
    registers[11] = 0x1b;
    registers[12] = UINT32_MAX;
    registers[13] = LUME_USER_LIMIT - 4096;
}

static void denied(uint32_t registers[15])
{
    uint32_t before[15];
    memcpy(before, registers, sizeof(before));
    assert(!lume_i686_query_try(registers));
    assert(!memcmp(before, registers, sizeof(before)));
}

static void accepted(uint32_t registers[15], uint64_t value)
{
    uint32_t before[15];
    memcpy(before, registers, sizeof(before));
    assert(lume_i686_query_try(registers));
    for (unsigned i = 0; i < 15; ++i)
        assert(registers[i] == (i == 7    ? (uint32_t)value
                                : i == 5  ? (uint32_t)(value >> 32)
                                : i == 12 ? ((before[i] & UINT32_C(0x240dd5)) | 0x202)
                                : i == 14 ? 0x23
                                          : before[i]));
}

static void terminal_queries(void)
{
    uint32_t registers[15];
    uint64_t *request = (void *)(uintptr_t)BASE;
    struct lume_tty_request mode = {.version = LUME_TTY_VERSION,
                                    .bytes = sizeof(mode),
                                    .fd = 9,
                                    .other = -1,
                                    .value = UINT32_MAX,
                                    .rows = UINT32_MAX};
    request[0] = LUME_SYS_TTY;
    request[1] = LUME_TTY_GET;
    request[2] = BASE + 128;
    request[3] = sizeof(mode);
    memcpy((void *)(uintptr_t)request[2], &mode, sizeof(mode));
    lume_arch_query_publish(7, 3, 0x1408, LUME_QUERY_PIPE);
    frame(registers, 128, BASE);
    denied(registers);
    terminal_handled = true;
    frame(registers, 128, BASE);
    accepted(registers, (uint64_t)-25);
    assert(!memcmp((void *)(uintptr_t)request[2], &mode, sizeof(mode)));
    request[3] = sizeof(mode) - 1;
    frame(registers, 128, BASE);
    denied(registers);
    request[3] = UINT64_C(1) << 32 | sizeof(mode);
    denied(registers);
    request[3] = sizeof(mode);
    request[1] = LUME_TTY_SET;
    denied(registers);
    request[1] = UINT64_C(1) << 32 | LUME_TTY_GET;
    denied(registers);
    request[1] = LUME_TTY_GET;
    struct lume_tty_request *user = (void *)(uintptr_t)request[2];
    ++user->version;
    denied(registers);
    user->version = LUME_TTY_VERSION;
    --user->bytes;
    denied(registers);
    user->bytes = sizeof(mode);
    user->reserved = 1;
    denied(registers);
    user->reserved = 0;
    writable_page[0] = false;
    denied(registers);
    writable_page[0] = true;
    unsigned before = terminal_calls;
    request[2] = BASE + 4096 - 17;
    memcpy((void *)(uintptr_t)request[2], &mode, sizeof(mode));
    frame(registers, 128, BASE);
    accepted(registers, (uint64_t)-25);
    assert(!memcmp((void *)(uintptr_t)request[2], &mode, sizeof(mode)));
    writable_page[1] = false;
    frame(registers, 128, BASE);
    denied(registers);
    writable_page[1] = true;
    user_page[1] = false;
    denied(registers);
    user_page[1] = true;
    request[2] = BASE + 8192 - 16;
    denied(registers);
    request[2] = UINT64_C(1) << 32 | BASE;
    denied(registers);
    assert(terminal_calls == before + 1);
    struct lume_query_account account;
    lume_arch_query_drain(&account);
    assert(account.calls == 2 && account.mask == (UINT64_C(1) << LUME_SYS_TTY));
}

static void drain_accounting(void)
{
    uint32_t registers[15];
    struct lume_query_account account;
    unsigned reads = frequency_reads;
    lume_arch_query_drain(&account);
    assert(!account.calls && !account.ticks && !account.mask && frequency_reads == reads);
    frequency = 1000000;
    lume_arch_query_publish(7, 3, 0x1408, LUME_QUERY_PID);
    raw_step = 1001;
    for (unsigned i = 0; i < 3; ++i)
    {
        frame(registers, 129, LUME_SYS_GETPID);
        accepted(registers, 3);
    }
    lume_arch_query_drain(&account);
    assert(account.owner == 7 && account.calls == 3 && account.ticks == 3003);
    assert(frequency_reads == reads + 1);
    lume_arch_query_publish(7, 3, 0x1408, LUME_QUERY_PID);
    for (unsigned i = 0; i < 65; ++i)
    {
        raw_step = i == 64 ? 4000 : 1001;
        frame(registers, 129, LUME_SYS_GETPID);
        accepted(registers, 3);
    }
    lume_arch_query_drain(&account);
    assert(account.calls == 65 && account.ticks == 162532);
    assert(account.mask == (UINT64_C(1) << LUME_SYS_GETPID));
    frequency = 0;
    lume_arch_query_publish(7, 3, 0x1408, LUME_QUERY_PID);
    frame(registers, 129, LUME_SYS_GETPID);
    accepted(registers, 3);
    lume_arch_query_drain(&account);
    assert(account.calls == 1 && !account.ticks);
    frequency = 123456789;
    raw_step = 1001;
    lume_arch_query_publish(7, 3, 0x1408, LUME_QUERY_PID);
    frame(registers, 129, LUME_SYS_GETPID);
    accepted(registers, 3);
    lume_arch_query_drain(&account);
    assert(account.calls == 1 && account.ticks == 8);
    reads = frequency_reads;
    lume_arch_query_drain(&account);
    assert(!account.calls && !account.ticks && !account.mask && frequency_reads == reads);
}

int main(void)
{
#ifdef _WIN32
    void *allocation = VirtualAlloc((void *)(uintptr_t)BASE, 12288, MEM_RESERVE, PAGE_NOACCESS);
    assert(allocation == (void *)(uintptr_t)BASE);
    assert(VirtualAlloc(allocation, 8192, MEM_COMMIT, PAGE_READWRITE));
#else
    void *allocation = mmap((void *)(uintptr_t)BASE, 12288, PROT_NONE,
                            MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    assert(allocation == (void *)(uintptr_t)BASE);
    assert(!mprotect(allocation, 8192, PROT_READ | PROT_WRITE));
#endif
    uint32_t registers[15];
    uint64_t *request = allocation;
    struct lume_query_account account;
    struct lume_context decoded = {.pc = LUME_USER_BASE + 8192, .raw = {[7] = LUME_SYS_GETPID}};
    assert(query_decode(&decoded));
    assert(decoded.syscall_pc == decoded.pc && decoded.reg[8] == LUME_SYS_GETPID);
    decoded.reg[0] = UINT64_C(0xabcdef0198765432);
    lume_context_finish_syscall(&decoded);
    assert(decoded.raw[7] == 0x98765432 && decoded.raw[5] == 0xabcdef01 && !decoded.syscall_pc);
    decoded.raw[7] = LUME_SYS_WRITE;
    assert(!query_decode(&decoded) && decoded.reg[0] == (uint64_t)-LUME_ENOSYS);
    lume_context_finish_syscall(&decoded);
    assert(decoded.raw[7] == (uint32_t)-LUME_ENOSYS && decoded.raw[5] == UINT32_MAX);
    lume_i686_query_start();
    frame(registers, 129, LUME_SYS_GETPID);
    denied(registers);
    lume_arch_query_publish(7, 3, UINT64_C(0x123456789abcdef0), LUME_QUERY_PID | LUME_QUERY_TID);
    unsigned before = translations;
    accepted(registers, 3);
    assert(translations == before);
    request[0] = LUME_SYS_GETPID;
    request[1] = request[2] = request[3] = UINT64_MAX;
    frame(registers, 128, BASE);
    accepted(registers, 3);
    request[0] = LUME_SYS_THREAD;
    request[1] = LUME_THREAD_SELF;
    request[2] = request[3] = 0;
    frame(registers, 128, BASE);
    accepted(registers, UINT64_C(0x123456789abcdef0));
    lume_arch_query_drain(&account);
    assert(account.owner == 7 && account.calls == 3 && account.ticks == 3);
    assert(account.mask == ((UINT64_C(1) << LUME_SYS_GETPID) | (UINT64_C(1) << LUME_SYS_THREAD)));
    denied(registers);

    lume_arch_query_publish(7, 3, 0x1408, LUME_QUERY_PID | LUME_QUERY_TID);
    for (unsigned i = 0; i < 130; ++i)
    {
        frame(registers, 129, LUME_SYS_GETPID);
        accepted(registers, 3);
    }
    lume_arch_query_drain(&account);
    assert(account.calls == 130 && account.ticks == 130);
    lume_arch_query_publish(7, 3, 0x1408, LUME_QUERY_PID | LUME_QUERY_TID);
    lume_arch_query_revoke(4);
    frame(registers, 129, LUME_SYS_GETPID);
    accepted(registers, 3);
    lume_arch_query_revoke(2);
    frame(registers, 129, LUME_SYS_GETPID);
    denied(registers);
    lume_arch_query_drain(&account);
    assert(account.calls == 1);

    lume_arch_query_publish(7, 3, 0x1408, LUME_QUERY_PIPE);
    request[0] = LUME_SYS_WRITE;
    request[1] = 9;
    request[2] = BASE + 64;
    request[3] = 17;
    pipe_result = 17;
    frame(registers, 128, BASE);
    denied(registers);
    pipe_handled = true;
    accepted(registers, 17);
    pipe_result = -LUME_EPIPE;
    frame(registers, 128, BASE);
    accepted(registers, (uint64_t)-LUME_EPIPE);
    lume_arch_query_drain(&account);
    assert(pipe_calls == 3 && account.calls == 2 &&
           account.mask == (UINT64_C(1) << LUME_SYS_WRITE));
    request[0] = LUME_SYS_THREAD;
    request[1] = LUME_THREAD_SELF;
    request[2] = request[3] = 0;

    for (unsigned mask = 0; mask < 4; ++mask)
    {
        lume_arch_query_publish(7, 3, 0x1408, mask);
        frame(registers, 129, LUME_SYS_GETPID);
        if (mask & LUME_QUERY_PID)
            accepted(registers, 3);
        else
            denied(registers);
        frame(registers, 128, BASE);
        if (mask & LUME_QUERY_TID)
            accepted(registers, 0x1408);
        else
            denied(registers);
        lume_arch_query_drain(&account);
    }
    lume_arch_query_publish(7, 3, 0x1408, LUME_QUERY_PID | LUME_QUERY_TID);
    const uint32_t invalid[] = {0, LUME_USER_BASE - 1, LUME_USER_LIMIT, UINT32_MAX};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
    {
        frame(registers, 128, invalid[i]);
        denied(registers);
        frame(registers, 129, LUME_SYS_GETPID);
        registers[10] = invalid[i];
        denied(registers);
        frame(registers, 129, LUME_SYS_GETPID);
        registers[13] = invalid[i];
        denied(registers);
    }
    frame(registers, 128, BASE + 8192 - 16);
    before = translations;
    denied(registers);
    assert(translations == before + 2);
    user_page[0] = false;
    frame(registers, 128, BASE);
    denied(registers);
    user_page[0] = true;
    request = (void *)(uintptr_t)(BASE + 4096 - 16);
    request[0] = LUME_SYS_GETPID;
    frame(registers, 128, BASE + 4096 - 16);
    before = translations;
    accepted(registers, 3);
    assert(translations == before + 2);
    uint64_t unaligned[4] = {LUME_SYS_GETPID, 0, 0, 0};
    memcpy((void *)(uintptr_t)(BASE + 4096 - 1), unaligned, sizeof(unaligned));
    frame(registers, 128, BASE + 4096 - 1);
    accepted(registers, 3);
    frame(registers, 128, BASE);
    registers[11] = 8;
    denied(registers);
    frame(registers, 32, BASE);
    denied(registers);
    frame(registers, 129, LUME_SYS_WRITE);
    denied(registers);
    request = allocation;
    request[0] = UINT64_C(0x10000000b);
    frame(registers, 128, BASE);
    denied(registers);
    request[0] = LUME_SYS_THREAD;
    request[2] = 1;
    denied(registers);
    lume_arch_query_drain(&account);
    lume_arch_query_publish(LUME_TASK_LIMIT, 0, 0, LUME_QUERY_PID);
    frame(registers, 129, LUME_SYS_GETPID);
    denied(registers);
    terminal_queries();
    drain_accounting();
#ifdef _WIN32
    assert(VirtualFree(allocation, 0, MEM_RELEASE));
#else
    assert(!munmap(allocation, 12288));
#endif
    puts(
        "PASS_I686_QUERY INT80 bounds, page permissions, INT81, terminal pipe queries, revocation, accounting and frame");
    return 0;
}
