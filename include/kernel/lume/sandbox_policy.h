#ifndef LUME_SANDBOX_POLICY_H
#define LUME_SANDBOX_POLICY_H
#include "lume/context.h"
#include "lume/vfs.h"
#include "lume/socket.h"
#include "lume/sandbox.h"

int64_t lume_sandbox_call(struct lume_files *, unsigned, uint64_t, uint64_t, uint64_t);
int lume_sandbox_check(struct lume_files *, unsigned, const struct lume_context *);
int lume_sandbox_open(struct lume_files *, const char *, uint64_t);
int lume_sandbox_path(struct lume_files *, const char *);
int lume_sandbox_socket(struct lume_files *, unsigned, const struct lume_socket_request *,
                        const char *);
bool lume_task_sandbox_ready(unsigned);
#endif
