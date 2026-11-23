#ifndef LUME_USER_STACK_H
#define LUME_USER_STACK_H
#include "early.h"

bool lume_stack_build(const uint64_t *, unsigned, const char *const *, unsigned,
                      const char *const *, uint64_t, uint64_t *);
bool lume_stack_build_exec(const uint64_t *, unsigned, const char *const *, unsigned,
                           const char *const *, uint64_t, bool secure, uint64_t *);
#endif
