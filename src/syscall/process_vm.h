/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef LUME_PROCESS_VM_H
#define LUME_PROCESS_VM_H
#include "lume/abi.h"
#include "lume/early.h"
#include "lume/vm.h"
#include "lume/vfs.h"
#include "lume/io_redirect.h"

static int64_t process_vm_call(struct lume_files *files, unsigned pid, uint64_t operation,
                               uint64_t pointer, uint64_t bytes)
{
    if (operation > LUME_VM_SYNC || bytes != sizeof(struct lume_vm_request))
        return -LUME_EINVAL;
    if (!lume_user_buffer(pointer, bytes, false) ||
        (operation == LUME_VM_INFO && !lume_user_buffer(pointer, bytes, true)))
        return -LUME_EFAULT;
    struct lume_vm_request request = *(const struct lume_vm_request *)(uintptr_t)pointer;
    if (lume_vm_file_call((unsigned)operation, &request) &&
        !lume_vfs_memory_io(files, (unsigned)request.fd) && !lume_task_storage_context())
        return LUME_IO_RETRY_STORAGE;
    int64_t result = lume_vm_call(files, pid, (unsigned)operation, &request);
    if (operation == LUME_VM_INFO)
        *(struct lume_vm_request *)(uintptr_t)pointer = request;
    return result;
}
#endif
