#ifndef LUME_SUPERVISION_ABI_H
#define LUME_SUPERVISION_ABI_H
#include <stdint.h>

#define LUME_SUPERVISION_VERSION 1u
#define LUME_SUPERVISION_COUNT 8u
#define LUME_SUPERVISION_NAME 24u

enum
{
    LUME_SV_REGISTER,
    LUME_SV_SUBMIT,
    LUME_SV_RECEIVE,
    LUME_SV_REPLY,
    LUME_SV_RESULT,
    LUME_SV_CANCEL,
    LUME_SV_STOP_CHILD
};

enum
{
    LUME_SV_LIST,
    LUME_SV_STATUS,
    LUME_SV_START,
    LUME_SV_STOP,
    LUME_SV_RESTART,
    LUME_SV_ENABLE,
    LUME_SV_DISABLE
};

enum
{
    LUME_SV_STOPPED,
    LUME_SV_RUNNING,
    LUME_SV_STOPPING,
    LUME_SV_BACKOFF,
    LUME_SV_FAILED
};

struct lume_service_status
{
    char name[LUME_SUPERVISION_NAME];
    uint32_t pid, state, enabled, restarts;
    int32_t exit_code, error;
};

struct lume_supervision_request
{
    uint32_t version, command;
    uint64_t sequence;
    char name[LUME_SUPERVISION_NAME];
    int32_t result;
    uint32_t count;
    struct lume_service_status service[LUME_SUPERVISION_COUNT];
};

/* STOP_CHILD accepts an unreaped direct child PID. It requests termination of
 * that process and its descendants. The caller must reap the resulting exits.
 * EBUSY means a kernel operation must finish before termination can begin. */
_Static_assert(sizeof(struct lume_service_status) == 48, "service status ABI");
_Static_assert(sizeof(struct lume_supervision_request) == 432, "supervision request ABI");
#endif
