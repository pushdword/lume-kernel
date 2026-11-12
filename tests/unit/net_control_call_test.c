#include "lume/abi.h"
#include "lume/process.h"
#include "lume/net.h"
#include "lume/user_buffer.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static bool capable, invalid_input, invalid_output;
static int acquire_error;
static unsigned acquired, released, called;
static struct lume_net_control input, output;

bool lume_task_capable(unsigned pid, uint64_t mask)
{
    assert(pid == 7 && mask == LUME_CAP_NET_ADMIN);
    return capable;
}

int lume_user_buffer_acquire(struct lume_user_buffer_token *token, unsigned pid, uint64_t address,
                             size_t bytes, bool write)
{
    assert(pid == 7 && address == 0x1000 && bytes == sizeof(input) && write);
    if (acquire_error)
        return acquire_error;
    ++acquired;
    *token = (struct lume_user_buffer_token){.registered = true, .valid = true};
    return 0;
}

bool lume_user_buffer_copy(struct lume_user_buffer_token *token, void *buffer, size_t bytes,
                           bool write)
{
    assert(token->registered && bytes == sizeof(input));
    if (write ? invalid_output : invalid_input)
        return false;
    if (write)
        memcpy(&output, buffer, bytes);
    else
        memcpy(buffer, &input, bytes);
    return true;
}

void lume_user_buffer_release(struct lume_user_buffer_token *token)
{
    assert(token->registered);
    token->registered = false;
    ++released;
}

int64_t lume_net_control(unsigned operation, struct lume_net_control *request)
{
    assert(operation == LUME_NET_GET && request->ifindex == 3);
    ++called;
    request->count = 4;
    return -LUME_EAGAIN;
}

#include "../../src/syscall/net_control.h"

int main(void)
{
    input.ifindex = 3;
    assert(net_control_call(7, UINT64_MAX, 0x1000, sizeof(input)) == -LUME_EINVAL);
    assert(net_control_call(7, LUME_NET_GET, 0x1000, 1) == -LUME_EINVAL);
    assert(net_control_call(7, LUME_NET_ADMIN, 0x1000, sizeof(input)) == -LUME_EPERM);
    acquire_error = -LUME_ENOMEM;
    assert(net_control_call(7, LUME_NET_GET, 0x1000, sizeof(input)) == -LUME_ENOMEM);
    assert(!acquired && !released && !called);
    acquire_error = 0;
    invalid_input = true;
    assert(net_control_call(7, LUME_NET_GET, 0x1000, sizeof(input)) == -LUME_EFAULT);
    assert(!called && acquired == 1 && released == 1);
    invalid_input = false;
    invalid_output = true;
    assert(net_control_call(7, LUME_NET_GET, 0x1000, sizeof(input)) == -LUME_EFAULT);
    assert(called == 1 && acquired == 2 && released == 2 && !output.count);
    invalid_output = false;
    assert(net_control_call(7, LUME_NET_GET, 0x1000, sizeof(input)) == -LUME_EAGAIN);
    assert(called == 2 && acquired == 3 && released == 3 && output.count == 4);
    puts("PASS NETCTL address validation, retained copies and mapping revocation");
    return 0;
}
