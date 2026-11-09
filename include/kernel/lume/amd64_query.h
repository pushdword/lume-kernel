#ifndef LUME_AMD64_QUERY_H
#define LUME_AMD64_QUERY_H
#define LUME_AMD64_QUERY_SYSCALL_PID 11
#define LUME_AMD64_QUERY_SYSCALL_THREAD 39

#ifndef __ASSEMBLER__
#include <stdbool.h>
#include <stdint.h>

enum
{
    LUME_AMD64_QUERY_PID = 1u,
    LUME_AMD64_QUERY_TID = 2u
};

struct lume_amd64_query_account
{
    unsigned owner;
    uint64_t calls, ticks, mask;
};

void lume_amd64_query_revoke(unsigned);
void lume_amd64_query_start(bool);
void lume_amd64_query_publish(unsigned, unsigned, uint64_t, unsigned);
void lume_amd64_query_drain(struct lume_amd64_query_account *);
bool lume_amd64_query_try(uint64_t *);
#endif
#endif
