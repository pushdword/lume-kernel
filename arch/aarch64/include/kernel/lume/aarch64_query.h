#ifndef LUME_AARCH64_QUERY_H
#define LUME_AARCH64_QUERY_H
#include "lume/query.h"
#include "context.h"

bool lume_aarch64_query_try(struct lume_context *, uint64_t);
bool lume_aarch64_pipe_try(struct lume_context *, uint64_t, struct lume_context **);
#endif
