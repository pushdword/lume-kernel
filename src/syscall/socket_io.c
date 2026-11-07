#include "lume/socket_io.h"
#include "lume/socket_file.h"
#include "lume/datagram.h"
#include "lume/socket_buffer.h"
#include "lume/sandbox_policy.h"
#include "lume/worker.h"
#include "lume/work.h"
#include "lume/io_redirect.h"
#include "lume/early.h"

static bool received_descriptors(const struct lume_socket_message *message)
{
    unsigned count = message->rights_count;
    if (count > LUME_SOCKET_RIGHTS_MAX)
        count = LUME_SOCKET_RIGHTS_MAX;
    for (unsigned i = 0; i < count; ++i)
        if (message->rights[i] >= 0)
            return true;
    return false;
}

static void rollback_descriptors(struct lume_files *files, struct lume_socket_message *message)
{
    unsigned count = message->rights_count;
    if (count > LUME_SOCKET_RIGHTS_MAX)
        count = LUME_SOCKET_RIGHTS_MAX;
    for (unsigned i = 0; i < count; ++i)
    {
        int fd = message->rights[i];
        message->rights[i] = -1;
        if (fd >= 0)
            (void)lume_vfs_close(files, (unsigned)fd);
    }
    message->rights_count = 0;
}

int64_t lume_socket_user_io(struct lume_files *files, unsigned pid, unsigned operation,
                            uint64_t address, uint64_t bytes)
{
    bool message_call =
        operation == LUME_SOCKET_SEND_MESSAGE || operation == LUME_SOCKET_RECEIVE_MESSAGE;
    bool receiving = operation == LUME_SOCKET_RECEIVE_MESSAGE || operation == LUME_SOCKET_RECVFROM;
    if ((!message_call && operation != LUME_SOCKET_SENDTO && operation != LUME_SOCKET_RECVFROM) ||
        bytes != sizeof(struct lume_socket_request))
        return -LUME_EINVAL;
    struct lume_user_buffer_token request_token = {0}, message_token = {0}, payload_token = {0};
    struct lume_socket_request request;
    struct lume_socket_message message = {0};
    bool completed = false, rollback = false;
    int64_t result = lume_user_buffer_acquire(&request_token, pid, address, (size_t)bytes, true);
    if (result)
        goto out;
    if (!lume_user_buffer_copy(&request_token, &request, sizeof(request), false))
    {
        result = -LUME_EFAULT;
        goto out;
    }
    if (request.version != LUME_SOCKET_VERSION || request.bytes != sizeof(request) ||
        request.reserved || request.reserved2)
    {
        result = -LUME_EINVAL;
        goto out;
    }
    if (message_call &&
        (request.other || request.family || request.type || request.flags || request.port ||
         request.address[0] || request.address[1] || request.address[2] || request.address[3] ||
         request.count != sizeof(message)))
    {
        result = -LUME_EINVAL;
        goto out;
    }
    result = lume_sandbox_socket(files, operation, &request, NULL);
    if (result)
        goto out;
    if ((lume_socket_network_io(files, (unsigned)request.fd) || !message_call) &&
        lume_worker_current() != LUME_WORK_NETWORK)
    {
        result = LUME_IO_RETRY_NETWORK;
        goto out;
    }
    uint64_t payload_address;
    size_t payload_bytes;
    if (message_call)
    {
        result =
            lume_user_buffer_acquire(&message_token, pid, request.buffer, sizeof(message), true);
        if (result)
            goto out;
        if (!lume_user_buffer_copy(&message_token, &message, sizeof(message), false))
        {
            result = -LUME_EFAULT;
            goto out;
        }
        if (message.count > LUME_FILE_IO_MAX)
        {
            result = -LUME_EINVAL;
            goto out;
        }
        if (receiving)
            for (unsigned i = 0; i < LUME_SOCKET_RIGHTS_MAX; ++i)
                message.rights[i] = -1;
        payload_address = message.buffer;
        payload_bytes = message.count;
    }
    else
    {
        if (operation == LUME_SOCKET_SENDTO && request.count > LUME_SOCKET_DATAGRAM_MAX)
        {
            result = -LUME_EMSGSIZE;
            goto out;
        }
        if (request.count > LUME_SOCKET_DATAGRAM_MAX)
            request.count = LUME_SOCKET_DATAGRAM_MAX;
        payload_address = request.buffer;
        payload_bytes = request.count;
    }
    if (payload_bytes)
    {
        result = lume_user_buffer_acquire(&payload_token, pid, payload_address, payload_bytes,
                                          receiving);
        if (result)
            goto out;
    }
    struct lume_socket_buffer buffer = {.payload = payload_bytes ? &payload_token : NULL,
                                        .request = &request_token,
                                        .message = message_call ? &message_token : NULL};
    struct lume_socket_buffer metadata = buffer;
    metadata.payload = NULL;
    if (!lume_socket_buffer_check(&metadata, 0, 0, true) ||
        !lume_socket_buffer_check(&buffer, 0, payload_bytes, receiving))
    {
        result = -LUME_EFAULT;
        goto out;
    }
    if (message_call)
        result = lume_socket_message_call_buffer(files, operation, request.fd, &message, &request,
                                                 &buffer);
    else if (lume_datagram_file(files, (unsigned)request.fd))
        result = lume_datagram_call_buffer(files, operation, &request, &buffer);
    else
        result = lume_socket_call(files, operation, &request);
    completed = true;
    if (receiving && message_call && result < 0 && received_descriptors(&message))
    {
        rollback = true;
        goto out;
    }
    struct lume_socket_buffer outputs = buffer;
    if (!receiving || result <= 0)
        outputs.payload = NULL;
    size_t copied = receiving && result > 0
                        ? ((uint64_t)result < payload_bytes ? (size_t)result : payload_bytes)
                        : 0;
    if (!lume_socket_buffer_check(&outputs, 0, copied, true))
    {
        result = -LUME_EFAULT;
        rollback = true;
        goto out;
    }
    /* All output identities are checked before the first final metadata write.
     * These physical copies contain no callback, allocator or suspension point. */
    if ((message_call && !lume_user_buffer_copy(&message_token, &message, sizeof(message), true)) ||
        !lume_user_buffer_copy(&request_token, &request, sizeof(request), true))
    {
        result = -LUME_EFAULT;
        rollback = true;
    }
out:
    if (completed && receiving && message_call && rollback)
        rollback_descriptors(files, &message);
    lume_user_buffer_release(&payload_token);
    lume_user_buffer_release(&message_token);
    lume_user_buffer_release(&request_token);
    return result;
}
