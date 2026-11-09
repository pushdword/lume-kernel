#ifndef LUME_I686_QUERY_H
#define LUME_I686_QUERY_H

#define LUME_I686_QUERY_SYSCALL_PID 11
#define LUME_I686_QUERY_SYSCALL_THREAD 39

#ifndef __ASSEMBLER__
#include <stdbool.h>
#include "query.h"

void lume_i686_query_start(void);
bool lume_i686_query_try(uint32_t *);
#endif
#endif
