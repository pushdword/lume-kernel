#ifndef LUME_SERVICE_H
#define LUME_SERVICE_H
#include <stdint.h>

/* Single bounded local service channel. No pointers or credentials. */
enum
{
    LUME_SERVICE_GET = 0,
    LUME_SERVICE_SEND = 1,
    LUME_SERVICE_RECEIVE = 2,
    LUME_SERVICE_PUBLISH = 3,
    LUME_SERVICE_SUBMIT = 4,
    LUME_SERVICE_RESULT = 5,
    LUME_SERVICE_WAIT = 6,
    LUME_SERVICE_CANCEL = 7
};

#define LUME_SERVICE_BYTES 512u
#define LUME_SERVICE_VERSION 1u
#define LUME_SERVICE_SIGNAL_EPOCH 1u

/* SUBMIT retains one result per task until RESULT consumes it.
 * CANCEL removes a queued request or a completed result. Received requests return EBUSY. */
struct lume_service_result
{
    uint32_t version, bytes;
    uint64_t sequence;
    int32_t result;
    uint32_t reserved;
};

/* Sequence zero waits for a manager request. Other sequences wait for the caller's result.
 * WAIT returns one when ready and zero at its deadline. It does not consume a result. */
struct lume_service_wait
{
    uint32_t version, bytes;
    uint64_t sequence, timeout_ms, signal_epoch;
    uint32_t flags, reserved;
};

enum
{
    NETD_IMPORT = 1,
    NETD_FORGET,
    NETD_PREFER,
    NETD_AUTO_ON,
    NETD_AUTO_OFF,
    NETD_CONNECT,
    NETD_PAUSE,
    NETD_RECONNECT,
    NETD_RELOAD,
    NETD_IMPORT_FILE
};

enum
{
    NETD_STARTING = 0,
    NETD_OFF,
    NETD_WAITING,
    NETD_CONNECTING,
    NETD_ONLINE,
    NETD_ERROR
};

struct lume_service_message
{
    uint64_t sequence;
    uint32_t version, command;
    char name[16];
    uint8_t reserved[32];
};

struct lume_net_service_status
{
    uint32_t version, pid, state, ifindex;
    uint64_t generation, acknowledged, retry_at;
    int32_t result;
    uint32_t attempts, automatic, profiles;
    char active[16];

    struct
    {
        char name[16];
        uint32_t priority, reserved;
    } profile[4];

    uint8_t reserved[344];
};

_Static_assert(sizeof(struct lume_service_message) == 64, "service message ABI");
_Static_assert(sizeof(struct lume_service_result) == 24, "service result ABI");
_Static_assert(sizeof(struct lume_service_wait) == 40, "service wait ABI");
_Static_assert(sizeof(struct lume_net_service_status) == 512, "service status ABI");
#endif
