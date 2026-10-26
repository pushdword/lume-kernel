#ifndef LUME_IO_COST_H
#define LUME_IO_COST_H
/* queue_lock protects state at admission, control, record and snapshot. A
 * saved epoch can cross an unlocked/suspended operation without joining a
 * later measurement session. No user buffer or device is accessed here. */
#include <api.h>
#include <io.h>
#include <net.h>
#include <crypto.h>

struct io_cost_state
{
    uint32_t enabled, epoch, pid_filter;
    struct lume_io_syscall_cost syscall[LUME_IO_SYSCALL_COUNT];
};

static uint32_t io_cost_add(uint32_t before, uint32_t amount)
{
    return UINT32_MAX - before < amount ? UINT32_MAX : before + amount;
}

static int io_cost_operation(unsigned operation)
{
    return operation > 0 && operation < LUME_IO_SYSCALL_COUNT && operation != LUME_IO &&
           operation != LUME_PERF;
}

static uint32_t io_cost_begin(const struct io_cost_state *s, unsigned operation, uint32_t pid)
{
    return s->enabled && (!s->pid_filter || s->pid_filter == pid) && io_cost_operation(operation)
               ? s->epoch
               : 0;
}

static void io_cost_control(struct io_cost_state *s, unsigned enabled, uint32_t pid_filter)
{
    s->enabled = enabled;
    if (enabled)
    {
        s->pid_filter = pid_filter;
        if (!++s->epoch)
            ++s->epoch;
        for (unsigned i = 0; i < LUME_IO_SYSCALL_COUNT; i++)
            s->syscall[i] = (struct lume_io_syscall_cost){0};
    }
}

static uint32_t io_cost_bytes(unsigned operation, unsigned action, int32_t result)
{
    if (result <= 0)
        return 0;
    if (operation == LUME_WRITE || operation == LUME_READFILE || operation == LUME_READ ||
        operation == LUME_FDWRITE)
        return (uint32_t)result;
    if (operation == LUME_NET && (action == LUME_NET_SEND || action == LUME_NET_RECV ||
                                  action == LUME_NET_SENDTO || action == LUME_NET_RECVFROM))
        return (uint32_t)result;
    if (operation == LUME_CRYPTO && action == LUME_CRYPTO_RANDOM)
        return (uint32_t)result;
    return 0;
}

static void io_cost_record(struct io_cost_state *s, uint32_t epoch, unsigned operation,
                           uint32_t ticks, int completed, int32_t result, uint32_t bytes)
{
    if (!epoch || !s->enabled || epoch != s->epoch || !io_cost_operation(operation))
        return;
    struct lume_io_syscall_cost *m = s->syscall + operation;
    m->calls = io_cost_add(m->calls, 1);
    if (completed)
    {
        m->errors = io_cost_add(m->errors, result < 0);
        m->bytes = io_cost_add(m->bytes, bytes);
    }
    uint32_t low = m->ticks_low + ticks;
    if (low < m->ticks_low)
    {
        if (m->ticks_high == UINT32_MAX)
            low = UINT32_MAX;
        else
            m->ticks_high++;
    }
    m->ticks_low = low;
    if (ticks > m->max_ticks)
        m->max_ticks = ticks;
}
#endif
