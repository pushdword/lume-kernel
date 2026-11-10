#ifndef LUME_QUERY_H
#define LUME_QUERY_H
#include <stdbool.h>
#include <stdint.h>

enum
{
    LUME_QUERY_PID = 1u,
    LUME_QUERY_TID = 2u,
    LUME_QUERY_PIPE = 4u
};

struct lume_query_account
{
    unsigned owner;
    uint64_t calls, ticks, mask;
};

/* Calls and syscall masks are exact. Fast paths may estimate elapsed ticks from
 * periodic samples. The scheduler bounds them to the running task's interval. */
void lume_arch_query_revoke(unsigned);
void lume_arch_query_publish(unsigned, unsigned, uint64_t, unsigned);
void lume_arch_query_drain(struct lume_query_account *);
bool lume_task_pipe_try(unsigned, uint64_t, uint64_t, uint64_t, bool, int64_t *);
bool lume_task_pipe_terminal_query(unsigned, uint64_t);
bool lume_task_pipe_terminal_request(unsigned, uint64_t);
#endif
