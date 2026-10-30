#include "lume/early.h"
#include "lume/abi.h"
#include "lume/io_call.h"
#include "lume/boot_storage.h"
#include "lume/net.h"
#include "lume/process.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static bool capable, readable = true, writable = true;
static unsigned boot_calls, stream_calls, operation_seen, owner_seen;
static uintptr_t user_pointer;

bool lume_task_capable(unsigned pid, uint64_t capabilities)
{
    assert(pid == 7 && capabilities == LUME_CAP_BOOT_UPDATE);
    return capable;
}

bool lume_user_buffer(uint64_t pointer, uint64_t bytes, bool write)
{
    assert(pointer == user_pointer && bytes >= LUME_IO_BYTES);
    return write ? writable : readable;
}

int lume_boot_call(unsigned pid, unsigned operation, struct lume_boot_request *request)
{
    assert((uintptr_t)request != user_pointer && request->version == LUME_IO_VERSION);
    ++boot_calls;
    owner_seen = pid;
    operation_seen = operation;
    request->total = LUME_BOOT_CAP_RESTART_GUARD;
    request->data[0] = 0x7a;
    return operation == LUME_BOOT_WRITE ? 17 : 0;
}

int lume_stream_call(unsigned pid, unsigned operation, struct lume_stream_request *request)
{
    assert((uintptr_t)request != user_pointer && request->version == LUME_IO_VERSION);
    ++stream_calls;
    owner_seen = pid;
    operation_seen = operation;
    request->data[0] = 0x37;
    return 23;
}

int main(void)
{
    struct lume_boot_request boot = {.version = LUME_IO_VERSION};
    user_pointer = (uintptr_t)&boot;
    assert(lume_io_call(7, false, LUME_BOOT_CAPABILITIES, user_pointer, sizeof(boot)) == 0);
    assert(boot_calls == 1 && boot.total == LUME_BOOT_CAP_RESTART_GUARD && boot.data[0] == 0x7a);
    assert(owner_seen == 7 && operation_seen == LUME_BOOT_CAPABILITIES);
    assert(lume_io_call(7, false, LUME_BOOT_INFO, user_pointer, sizeof(boot)) == 0);
    for (unsigned operation = LUME_BOOT_BEGIN; operation <= LUME_BOOT_ABORT; ++operation)
        assert(lume_io_call(7, false, operation, user_pointer, sizeof(boot)) == -LUME_EPERM);
    assert(boot_calls == 2);
    capable = true;
    assert(lume_io_call(7, false, LUME_BOOT_WRITE, user_pointer, sizeof(boot)) == 17);
    unsigned before = boot_calls;
    assert(lume_io_call(7, false, LUME_BOOT_CAPABILITIES + 1, user_pointer, sizeof(boot)) ==
           -LUME_EINVAL);
    assert(lume_io_call(7, false, UINT64_MAX, user_pointer, sizeof(boot)) == -LUME_EINVAL);
    assert(lume_io_call(7, false, LUME_BOOT_INFO, user_pointer, sizeof(boot) - 1) == -LUME_EINVAL);
    readable = false;
    assert(lume_io_call(7, false, LUME_BOOT_INFO, user_pointer, sizeof(boot)) == -LUME_EFAULT);
    readable = true;
    writable = false;
    assert(lume_io_call(7, false, LUME_BOOT_CAPABILITIES, user_pointer, sizeof(boot)) ==
           -LUME_EFAULT);
    writable = true;
    assert(boot_calls == before);
    struct lume_stream_request stream = {.version = LUME_IO_VERSION};
    user_pointer = (uintptr_t)&stream;
    capable = false;
    assert(lume_io_call(7, true, LUME_STREAM_SEND, user_pointer, sizeof(stream)) == 23);
    assert(stream_calls == 1 && stream.data[0] == 0x37 && owner_seen == 7);
    assert(lume_io_call(7, true, LUME_ICMP_ECHO_CLOSE, user_pointer, sizeof(stream)) == 23);
    assert(lume_io_call(7, true, LUME_ICMP_ECHO_CLOSE + 1, user_pointer, sizeof(stream)) ==
           -LUME_EINVAL);
    assert(stream_calls == 2);
    puts("PASS boot and stream operation bounds, capabilities, permissions and request copies");
    return 0;
}
