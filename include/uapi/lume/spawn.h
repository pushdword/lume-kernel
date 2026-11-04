#ifndef LUME_SPAWN_ACTIONS_H
#define LUME_SPAWN_ACTIONS_H
#include <stdint.h>
#define LUME_SPAWN_VERSION 1u
#define LUME_SPAWN_OPTIONS_VERSION 2u
#define LUME_SPAWN_ACTION_MAX 32u

enum
{
    LUME_SPAWN_CLOSE = 1,
    LUME_SPAWN_DUP2,
    LUME_SPAWN_OPEN,
    LUME_SPAWN_CHDIR
};

enum
{
    LUME_SPAWN_SIGDEFAULT = 1u,
    LUME_SPAWN_SIGMASK = 2u,
    LUME_SPAWN_SETSID = 4u,
    LUME_SPAWN_CLOEXEC_DEFAULT = 8u
};

/* Pointer fields contain unsigned virtual addresses. OPEN uses native Lume open flags.
 * Actions run in order in the child before CLOEXEC descriptors are closed. */
struct lume_spawn_action
{
    uint64_t path;
    uint32_t operation, fd, target, flags;
};

struct lume_spawn_actions_request
{
    uint64_t path, argv, envp;
    uint32_t argc, envc;
    uint64_t actions;
    uint32_t count, version;
};

struct lume_spawn_options_request
{
    struct lume_spawn_actions_request base;
    uint32_t flags, signal_default, signal_mask, reserved;
};

_Static_assert(sizeof(struct lume_spawn_action) == 24, "spawn action ABI");
_Static_assert(sizeof(struct lume_spawn_actions_request) == 48, "spawn actions ABI");
_Static_assert(sizeof(struct lume_spawn_options_request) == 64, "spawn options ABI");
#endif
