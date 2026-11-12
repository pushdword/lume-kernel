/* SPDX-License-Identifier: BSD-3-Clause */
#include "lume/abi.h"
#include "lume/vfs.h"
#include "lume/vm.h"
#include "lume/early.h"
#include "lume/io_redirect.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static struct lume_files files;
static struct lume_vm_request input, observed;
static bool storage, denied, deny_write, mutate;
static unsigned calls, context_checks;

bool lume_user_buffer(uint64_t pointer, uint64_t bytes, bool writing)
{
    return !denied && !(writing && deny_write) && pointer == (uintptr_t)&input &&
           bytes == sizeof(input);
}

bool lume_task_storage_context(void)
{
    ++context_checks;
    if (mutate)
        input.flags = LUME_MAP_ANONYMOUS;
    return storage;
}

bool lume_vfs_memory_io(struct lume_files *table, uint64_t fd)
{
    assert(table == &files);
    return fd == 9;
}

bool lume_vm_file_call(unsigned operation, const struct lume_vm_request *request)
{
    return operation == LUME_VM_MAP && !(request->flags & LUME_MAP_ANONYMOUS);
}

int64_t lume_vm_call(struct lume_files *table, unsigned pid, unsigned operation,
                     struct lume_vm_request *request)
{
    assert(table == &files && pid == 3 && request != &input);
    observed = *request;
    ++calls;
    if (operation == LUME_VM_INFO)
    {
        request->address = 0x12345000;
        return 0;
    }
    return 0x23456000;
}

#include "process_vm.h"

static int64_t invoke(uint64_t operation)
{
    return process_vm_call(&files, 3, operation, (uintptr_t)&input, sizeof(input));
}

int main(void)
{
    input = (struct lume_vm_request){
        .version = LUME_VM_VERSION, .length = 8192, .flags = LUME_MAP_PRIVATE | LUME_MAP_ANONYMOUS};
    assert(invoke(LUME_VM_MAP) == 0x23456000 && calls == 1 && !context_checks);
    assert(observed.flags == input.flags);
    input.flags = LUME_MAP_SHARED;
    input.fd = 9;
    assert(invoke(LUME_VM_MAP) == 0x23456000 && calls == 2 && !context_checks);
    input.flags = LUME_MAP_PRIVATE;
    input.fd = 3;
    assert(invoke(LUME_VM_MAP) == LUME_IO_RETRY_STORAGE && calls == 2 && context_checks == 1);
    storage = mutate = true;
    assert(invoke(LUME_VM_MAP) == 0x23456000 && calls == 3);
    assert(observed.flags == LUME_MAP_PRIVATE && input.flags == LUME_MAP_ANONYMOUS);
    storage = mutate = false;
    deny_write = true;
    assert(invoke(LUME_VM_INFO) == -LUME_EFAULT && calls == 3);
    deny_write = false;
    assert(!invoke(LUME_VM_INFO) && input.address == 0x12345000 && calls == 4);
    denied = true;
    assert(invoke(LUME_VM_MAP) == -LUME_EFAULT && calls == 4);
    assert(invoke(UINT64_MAX) == -LUME_EINVAL && calls == 4);
    assert(process_vm_call(&files, 3, LUME_VM_MAP, (uintptr_t)&input, sizeof(input) - 1) ==
           -LUME_EINVAL);
    puts("PASS_VM_CALL copied requests, RAM admission, storage redirect and output validation");
    return 0;
}
