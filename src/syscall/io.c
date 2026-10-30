#include "lume/early.h"
#include "lume/abi.h"
#include "lume/boot_storage.h"
#include "lume/net.h"
#include "lume/io_call.h"
#include "lume/process.h"

int64_t lume_io_call(unsigned pid, bool stream, uint64_t operation, uint64_t pointer, uint64_t n)
{
    if (!stream && operation != LUME_BOOT_INFO && operation != LUME_BOOT_CAPABILITIES &&
        !lume_task_capable(pid, LUME_CAP_BOOT_UPDATE))
        return -LUME_EPERM;
    size_t bytes = stream ? sizeof(struct lume_stream_request) : sizeof(struct lume_boot_request);
    if (n != bytes || operation > (stream ? LUME_ICMP_ECHO_CLOSE : LUME_BOOT_CONFIRM))
        return -LUME_EINVAL;
    if (!lume_user_buffer(pointer, n, false) || !lume_user_buffer(pointer, n, true))
        return -LUME_EFAULT;

    union
    {
        struct lume_stream_request stream;
        struct lume_boot_request boot;
    } request;

    unsigned char *p = (void *)&request;
    unsigned char *user = (void *)(uintptr_t)pointer;
    for (size_t i = 0; i < bytes; ++i)
        p[i] = user[i];
    int result = stream ? lume_stream_call(pid, (unsigned)operation, &request.stream)
                        : lume_boot_call(pid, (unsigned)operation, &request.boot);
    for (size_t i = 0; i < bytes; ++i)
        user[i] = p[i];
    return result;
}
