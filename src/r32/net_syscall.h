/* Syscall37 marshalling. Validate/copy under queue_lock; the network owner
 * protects the driver while queue_lock is released around kernel-only copies.
 * The driver must never block for peer/timer/socket progress.
 * user_range/user_copy_* are supplied by the kernel or the focused host test.
 */
#ifndef LUME_NET_SYSCALL_H
#define LUME_NET_SYSCALL_H
#include <net.h>
#include "network.h"
#include "net_gate.h"

static int32_t lume_net_dispatch(unsigned owner, unsigned uid, uint32_t operation, uint32_t pointer,
                                 uint32_t bytes, unsigned char *data)
{
    if (bytes != sizeof(struct lume_net_request))
        return -LUME_NET_EINVAL;
    if (operation < LUME_NET_INFO || operation > LUME_NET_POLL)
        return -LUME_NET_EINVAL;
    if ((pointer & 3u) || pointer > UINT32_MAX - bytes || !user_range(owner, pointer, bytes, 0) ||
        !user_range(owner, pointer, bytes, 1))
        return -LUME_NET_EFAULT;
    struct lume_net_request request;
    user_copy_from(owner, &request, pointer, sizeof request);
    if (request.version != LUME_NET_VERSION || request.bytes != sizeof request || request.flags)
        return -LUME_NET_EINVAL;
    if (operation == LUME_NET_CONFIG && uid)
        return -LUME_NET_EACCES;
    if (operation == LUME_NET_SOCKET && request.protocol != LUME_NET_TCP &&
        request.protocol != LUME_NET_UDP)
        return -LUME_NET_EOPNOTSUPP;
    if ((operation == LUME_NET_BIND || operation == LUME_NET_CONNECT ||
         operation == LUME_NET_SENDTO) &&
        (request.port > 65535u || (operation != LUME_NET_BIND && !request.port)))
        return -LUME_NET_EINVAL;
    if (operation == LUME_NET_POLL &&
        (request.events & ~(LUME_NET_POLLIN | LUME_NET_POLLOUT | LUME_NET_POLLLISTEN)))
        return -LUME_NET_EINVAL;
    unsigned input =
        operation == LUME_NET_CONFIG || operation == LUME_NET_SEND || operation == LUME_NET_SENDTO;
    unsigned output =
        operation == LUME_NET_INFO || operation == LUME_NET_RECV || operation == LUME_NET_RECVFROM;
    uint32_t buffer = request.buffer, length = request.length;
    if (input || output)
    {
        if (length > LUME_NET_IO_MAX)
            return -LUME_NET_EMSGSIZE;
        if ((operation == LUME_NET_INFO && length != sizeof(struct lume_net_info)) ||
            (operation == LUME_NET_CONFIG && length != sizeof(struct lume_net_config)))
            return -LUME_NET_EINVAL;
        if (length && (buffer > UINT32_MAX - length || !user_range(owner, buffer, length, output)))
            return -LUME_NET_EFAULT;
        /* Copying the returned request must never overwrite received data. */
        if (output && length && buffer < pointer + bytes && pointer < buffer + length)
            return -LUME_NET_EINVAL;
        if (input && length)
            user_copy_from(owner, data, buffer, length);
        /* RECV returns only bytes initialized by receive_data(). Leave the
         * unused tail alone, especially on an EAGAIN retry. INFO retains its
         * fully initialized fixed-size output contract. */
        if (operation == LUME_NET_INFO)
            for (unsigned i = 0; i < length; i++)
                data[i] = 0;
    }
    if (operation == LUME_NET_CONFIG)
    {
        struct lume_net_config configuration;
        unsigned char *to = (unsigned char *)&configuration;
        for (unsigned i = 0; i < sizeof configuration; i++)
            to[i] = data[i];
        if (configuration.version != LUME_NET_VERSION ||
            configuration.bytes != sizeof configuration || configuration.flags ||
            configuration.reserved[0] || configuration.reserved[1])
            return -LUME_NET_EINVAL;
    }
    unsigned token = lume_net_begin();
    int32_t result = lume_net_call(owner, operation, &request, data);
    lume_net_end(token);
    if (result < 0)
        return result;
    if (operation == LUME_NET_SEND || operation == LUME_NET_RECV || operation == LUME_NET_SENDTO ||
        operation == LUME_NET_RECVFROM)
    {
        if ((uint32_t)result > length)
            return -LUME_NET_EIO;
    }
    else if (operation == LUME_NET_POLL)
    {
        if (result > 1 ||
            (request.events & ~(LUME_NET_POLLIN | LUME_NET_POLLOUT | LUME_NET_POLLERR |
                                LUME_NET_POLLHUP | LUME_NET_POLLLISTEN)))
            return -LUME_NET_EIO;
    }
    else if (operation != LUME_NET_SOCKET && operation != LUME_NET_ACCEPT && result)
        return -LUME_NET_EIO;
    if (operation == LUME_NET_INFO)
    {
        struct lume_net_info info;
        unsigned char *to = (unsigned char *)&info;
        for (unsigned i = 0; i < sizeof info; i++)
            to[i] = data[i];
        if (info.version != LUME_NET_VERSION || info.bytes != sizeof info)
            return -LUME_NET_EIO;
    }
    if (output)
    {
        unsigned count = operation == LUME_NET_INFO ? length : (unsigned)result;
        if (count)
            user_copy_to(owner, buffer, data, count);
    }
    /* A driver cannot redirect an already validated user-copy destination. */
    request.version = LUME_NET_VERSION;
    request.bytes = sizeof request;
    request.flags = 0;
    request.buffer = buffer;
    request.length = length;
    if (operation == LUME_NET_SOCKET || operation == LUME_NET_ACCEPT)
        request.handle = result;
    user_copy_to(owner, pointer, &request, sizeof request);
    return result;
}
#endif
