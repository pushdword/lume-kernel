#ifndef LUME_AMD64_QUERY_H
#define LUME_AMD64_QUERY_H
#define LUME_AMD64_QUERY_SYSCALL_READ 2
#define LUME_AMD64_QUERY_SYSCALL_WRITE 3
#define LUME_AMD64_QUERY_SYSCALL_PID 11
#define LUME_AMD64_QUERY_SYSCALL_THREAD 39
#define LUME_AMD64_QUERY_SYSCALL_TTY 53
#define LUME_AMD64_ENTRY_STACK 0
#define LUME_AMD64_ENTRY_USER_SP 8
#define LUME_AMD64_ENTRY_CPU 16

#ifndef __ASSEMBLER__
#include <stdbool.h>
#include <stdint.h>
#include "lume/query.h"
void lume_amd64_query_start(bool);
bool lume_amd64_query_try(uint64_t *);
#endif
#endif
