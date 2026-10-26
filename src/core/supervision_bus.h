#ifndef LUME_SUPERVISION_BUS_H
#define LUME_SUPERVISION_BUS_H
#include "lume/abi.h"
#include "lume/supervision.h"
#include <stdbool.h>
#include <string.h>

struct supervision_slot
{
    unsigned owner, state;
    struct lume_supervision_request request;
};

struct supervision_bus
{
    unsigned manager;
    uint64_t sequence;
    struct supervision_slot slot[LUME_SUPERVISION_COUNT];
};

static bool supervision_name(const char name[LUME_SUPERVISION_NAME])
{
    unsigned n = 0;
    for (; n < LUME_SUPERVISION_NAME && name[n]; ++n)
        if (!((name[n] >= 'a' && name[n] <= 'z') || (name[n] >= '0' && name[n] <= '9') ||
              name[n] == '-' || name[n] == '_'))
            return false;
    if (!n || n == LUME_SUPERVISION_NAME)
        return false;
    for (; n < LUME_SUPERVISION_NAME; ++n)
        if (name[n])
            return false;
    return true;
}

static void supervision_release(struct supervision_bus *bus, unsigned pid)
{
    if (bus->manager == pid)
    {
        bus->manager = 0;
        for (unsigned i = 0; i < LUME_SUPERVISION_COUNT; ++i)
            if (bus->slot[i].owner && bus->slot[i].state != 3)
            {
                bus->slot[i].request.result = -LUME_EPIPE;
                bus->slot[i].request.count = 0;
                bus->slot[i].state = 3;
            }
    }
    for (unsigned i = 0; i < LUME_SUPERVISION_COUNT; ++i)
        if (bus->slot[i].owner == pid)
            bus->slot[i] = (struct supervision_slot){0};
}

/* The caller serializes bus access and releases each process before PID reuse. */
static int64_t supervision_call(struct supervision_bus *bus, unsigned pid, unsigned op,
                                bool manager_capable, bool control_capable,
                                struct lume_supervision_request *request)
{
    if (!pid || request->version != LUME_SUPERVISION_VERSION)
        return -LUME_EINVAL;
    if (op == LUME_SV_REGISTER)
    {
        if (!manager_capable)
            return -LUME_EPERM;
        if (bus->manager && bus->manager != pid)
            return -LUME_EBUSY;
        bus->manager = pid;
        return 0;
    }
    if (op == LUME_SV_SUBMIT)
    {
        if (!control_capable || pid == bus->manager)
            return -LUME_EPERM;
        if (!bus->manager)
            return -LUME_ENOENT;
        if (request->command > LUME_SV_DISABLE || request->sequence ||
            (request->command == LUME_SV_LIST ? request->name[0] != 0
                                              : !supervision_name(request->name)))
            return -LUME_EINVAL;
        unsigned free_slot = LUME_SUPERVISION_COUNT;
        for (unsigned i = 0; i < LUME_SUPERVISION_COUNT; ++i)
        {
            if (bus->slot[i].owner == pid)
                return -LUME_EBUSY;
            if (!bus->slot[i].owner && free_slot == LUME_SUPERVISION_COUNT)
                free_slot = i;
        }
        if (free_slot == LUME_SUPERVISION_COUNT || bus->sequence == UINT64_MAX)
            return -LUME_EAGAIN;
        struct supervision_slot *slot = &bus->slot[free_slot];
        *slot = (struct supervision_slot){.owner = pid, .state = 1};
        slot->request.version = LUME_SUPERVISION_VERSION;
        slot->request.command = request->command;
        slot->request.sequence = ++bus->sequence;
        memcpy(slot->request.name, request->name, sizeof(request->name));
        request->sequence = slot->request.sequence;
        return 0;
    }
    if (op == LUME_SV_RECEIVE || op == LUME_SV_REPLY)
    {
        if (!manager_capable || bus->manager != pid)
            return -LUME_EPERM;
        if (op == LUME_SV_RECEIVE)
        {
            struct supervision_slot *oldest = NULL;
            for (unsigned i = 0; i < LUME_SUPERVISION_COUNT; ++i)
                if (bus->slot[i].state == 1 &&
                    (!oldest || bus->slot[i].request.sequence < oldest->request.sequence))
                    oldest = &bus->slot[i];
            if (!oldest)
                return -LUME_EAGAIN;
            oldest->state = 2;
            *request = oldest->request;
            return 0;
        }
        if (request->count > LUME_SUPERVISION_COUNT)
            return -LUME_EINVAL;
        for (unsigned i = 0; i < LUME_SUPERVISION_COUNT; ++i)
            if (bus->slot[i].state == 2 && bus->slot[i].request.sequence == request->sequence)
            {
                bus->slot[i].request = *request;
                bus->slot[i].state = 3;
                return 0;
            }
        return -LUME_ENOENT;
    }
    if (op != LUME_SV_RESULT && op != LUME_SV_CANCEL)
        return -LUME_EINVAL;
    for (unsigned i = 0; i < LUME_SUPERVISION_COUNT; ++i)
        if (bus->slot[i].owner == pid && bus->slot[i].request.sequence == request->sequence)
        {
            if (op == LUME_SV_RESULT && bus->slot[i].state != 3)
                return -LUME_EAGAIN;
            if (op == LUME_SV_RESULT)
                *request = bus->slot[i].request;
            bus->slot[i] = (struct supervision_slot){0};
            return 0;
        }
    return -LUME_ENOENT;
}
#endif
