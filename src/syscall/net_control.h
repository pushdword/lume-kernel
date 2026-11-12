#ifndef LUME_NET_CONTROL_CALL_H
#define LUME_NET_CONTROL_CALL_H
#include "lume/user_buffer.h"

static int64_t net_control_call(unsigned pid, uint64_t operation, uint64_t address, uint64_t bytes)
{
    if ((operation == LUME_NET_ADMIN || operation == LUME_NET_WIFI_CONNECT ||
         operation == LUME_NET_WIFI_DISCONNECT || operation == LUME_NET_DHCP_START ||
         operation == LUME_NET_WIFI_SCAN || operation == LUME_NET_WIFI_CONNECT_BEGIN ||
         operation == LUME_NET_DHCP_BEGIN || operation == LUME_NET_IPV4 ||
         operation == LUME_NET_ROUTE_ADD || operation == LUME_NET_ROUTE_DELETE) &&
        !lume_task_capable(pid, LUME_CAP_NET_ADMIN))
        return -LUME_EPERM;
    if (operation > LUME_NET_ROUTE_DELETE || bytes != sizeof(struct lume_net_control))
        return -LUME_EINVAL;
    struct lume_user_buffer_token token;
    int result = lume_user_buffer_acquire(&token, pid, address, (size_t)bytes, true);
    if (result)
        return result;
    struct lume_net_control request = {0};
    int64_t status = -LUME_EFAULT;
    if (lume_user_buffer_copy(&token, &request, sizeof(request), false))
    {
        status = lume_net_control((unsigned)operation, &request);
        if (!lume_user_buffer_copy(&token, &request, sizeof(request), true))
            status = -LUME_EFAULT;
    }
    lume_user_buffer_release(&token);
    volatile unsigned char *wipe = (void *)&request;
    for (unsigned i = 0; i < sizeof(request); ++i)
        wipe[i] = 0;
    return status;
}
#endif
