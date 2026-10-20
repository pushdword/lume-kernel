#ifndef LUME_SERVICE_H
#define LUME_SERVICE_H
#include <stdint.h>

/* Single bounded local service channel. No pointers or credentials. */
enum
{
    LUME_SERVICE_GET = 0,
    LUME_SERVICE_SEND = 1,
    LUME_SERVICE_RECEIVE = 2,
    LUME_SERVICE_PUBLISH = 3
};

#define LUME_SERVICE_BYTES 512u

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
    NETD_RELOAD
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
_Static_assert(sizeof(struct lume_net_service_status) == 512, "service status ABI");
#endif
