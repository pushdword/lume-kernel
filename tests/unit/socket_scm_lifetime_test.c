#define LUME_SOCKET_NETWORK_LIFETIME_INCLUDE
#include "socket_network_lifetime_test.c"
#include "lume/socket_io.h"
#include "lume/file_lock.h"
#include "lume/io_redirect.h"

#define lume_user_buffer_check scm_real_check
#define lume_user_buffer_copy_at scm_real_copy_at
#define lume_user_buffer_copy scm_real_copy
#include "../../src/mm/user_buffer.c"
#undef lume_user_buffer_check
#undef lume_user_buffer_copy_at
#undef lume_user_buffer_copy

static uint64_t metadata_base, payload_base, revoke_address;
static bool metadata_present[2];
static unsigned descriptor_baseline, revocations;
static int lock_fd, probe_fd;

static unsigned descriptor_count(void)
{
    unsigned count = 0;
    for (unsigned i = 0; i < files[0].identity.limit[LUME_LIMIT_FILES].soft; ++i)
        count += files[0].fd[i].object >= 0;
    return count;
}

/* The boundary hook revokes an actual mapping only after the endpoint installed
 * a new descriptor. The token implementation still validates every final copy. */
static void revoke_metadata(const struct lume_user_buffer_token *token, bool output)
{
    if (!output || !revoke_address || token->first != revoke_address ||
        descriptor_count() <= descriptor_baseline)
        return;
    uint64_t address = revoke_address;
    revoke_address = 0;
    CHECK(vm(0, LUME_VM_UNMAP, address, 4096, 0, 0, -1) == 0);
    metadata_present[(address - metadata_base) / 4096] = false;
    ++revocations;
}

bool lume_user_buffer_check(const struct lume_user_buffer_token *token, size_t offset, size_t bytes,
                            bool output)
{
    revoke_metadata(token, output);
    return scm_real_check(token, offset, bytes, output);
}

bool lume_user_buffer_copy_at(struct lume_user_buffer_token *token, size_t offset, void *data,
                              size_t bytes, bool output)
{
    revoke_metadata(token, output);
    return scm_real_copy_at(token, offset, data, bytes, output);
}

bool lume_user_buffer_copy(struct lume_user_buffer_token *token, void *data, size_t bytes,
                           bool output)
{
    revoke_metadata(token, output);
    return scm_real_copy(token, data, bytes, output);
}

int lume_sandbox_socket(struct lume_files *owner, unsigned operation,
                        const struct lume_socket_request *request, const char *path)
{
    CHECK(owner == &files[0] && owner->identity.sandbox_profile == LUME_SANDBOX_NONE);
    CHECK(request && !path);
    CHECK(operation == LUME_SOCKET_SEND_MESSAGE || operation == LUME_SOCKET_RECEIVE_MESSAGE ||
          operation == LUME_SOCKET_SENDTO || operation == LUME_SOCKET_RECVFROM);
    return 0;
}

static void vm_bytes(uint64_t address, void *data, size_t bytes, bool output)
{
    while (bytes)
    {
        uint64_t physical;
        CHECK(lume_mmu_translate(2, address, output, true, false, &physical));
        size_t part = 4096 - (size_t)(address & 4095);
        if (part > bytes)
            part = bytes;
        if (output)
            memcpy((void *)(uintptr_t)physical, data, part);
        else
            memcpy(data, (void *)(uintptr_t)physical, part);
        data = (unsigned char *)data + part;
        address += part;
        bytes -= part;
    }
}

static void metadata_setup(void)
{
    CHECK(!metadata_base);
    metadata_base =
        (uint64_t)vm(0, LUME_VM_MAP, 0, 8192, 3, LUME_MAP_PRIVATE | LUME_MAP_ANONYMOUS, -1);
    CHECK(metadata_base >= LUME_VM_BASE);
    CHECK(lume_user_buffer(metadata_base, 8192, true));
    metadata_present[0] = metadata_present[1] = true;
}

static void metadata_cleanup(void)
{
    CHECK(!token_count);
    for (unsigned i = 0; i < 2; ++i)
        if (metadata_present[i])
            CHECK(vm(0, LUME_VM_UNMAP, metadata_base + i * 4096, 4096, 0, 0, -1) == 0);
    metadata_base = revoke_address = 0;
}

static int64_t call_message_bytes(int fd, unsigned operation, unsigned capacity, unsigned flags,
                                  unsigned invalid, unsigned bytes)
{
    unsigned before_tokens = token_count;
    struct lume_socket_message message = {.version = LUME_SOCKET_MESSAGE_VERSION,
                                          .bytes = sizeof(message),
                                          .buffer = payload_base,
                                          .count = bytes,
                                          .rights_count = capacity,
                                          .flags = flags};
    for (unsigned i = 0; i < LUME_SOCKET_RIGHTS_MAX; ++i)
        message.rights[i] = lock_fd;
    if (invalid == 1)
        ++message.version;
    if (invalid == 2)
        message.received_flags = LUME_SOCKET_MESSAGE_CTRUNC;
    struct lume_socket_request request = {.version = LUME_SOCKET_VERSION,
                                          .bytes = sizeof(request),
                                          .fd = fd,
                                          .buffer = metadata_base + 4096,
                                          .count = sizeof(message)};
    vm_bytes(metadata_base, &request, sizeof(request), true);
    vm_bytes(metadata_base + 4096, &message, sizeof(message), true);
    int64_t result = lume_socket_user_io(&files[0], 1, operation, metadata_base, sizeof(request));
    CHECK(token_count == before_tokens);
    return result;
}

static int64_t call_message(int fd, unsigned operation, unsigned capacity, unsigned flags,
                            unsigned invalid)
{
    return call_message_bytes(fd, operation, capacity, flags, invalid, 64);
}

static struct lume_socket_message read_message(void)
{
    struct lume_socket_message message;
    vm_bytes(metadata_base + 4096, &message, sizeof(message), false);
    return message;
}

static void expect_lock(void)
{
    struct lume_file_lock_request query = {.version = LUME_FILE_LOCK_VERSION,
                                           .bytes = sizeof(query),
                                           .fd = probe_fd,
                                           .type = LUME_FILE_LOCK_WRITE,
                                           .length = 1};
    CHECK(lume_vfs_posix_lock(&files[2], 3, true, &query) == 0);
    CHECK(query.type == LUME_FILE_LOCK_WRITE && query.pid == 1);
    struct lume_stat info;
    CHECK(lume_vfs_stat(&files[0], (unsigned)lock_fd, &info) == 0);
}

static struct lume_socket_request socket_pair(void)
{
    struct lume_socket_request pair = {
        .version = LUME_SOCKET_VERSION, .bytes = sizeof(pair), .family = 1, .type = 1};
    CHECK(lume_socket_call(&files[0], LUME_SOCKET_PAIR, &pair) == 0);
    return pair;
}

static void send_rights(int fd, unsigned count)
{
    unsigned char data[64];
    memset(data, 0x53, sizeof(data));
    struct lume_socket_message message = {.version = LUME_SOCKET_MESSAGE_VERSION,
                                          .bytes = sizeof(message),
                                          .buffer = (uintptr_t)data,
                                          .count = sizeof(data),
                                          .rights_count = count};
    for (unsigned i = 0; i < count; ++i)
        message.rights[i] = lock_fd;
    CHECK(lume_socket_message_call(&files[0], LUME_SOCKET_SEND_MESSAGE, fd, &message) == 64);
}

static void invalid_inputs(void)
{
    struct lume_socket_request pair = socket_pair();
    metadata_setup();
    unsigned before = descriptor_count();
    for (unsigned invalid = 1; invalid <= 2; ++invalid)
    {
        CHECK(call_message(pair.other, LUME_SOCKET_RECEIVE_MESSAGE, 3, 0, invalid) == -LUME_EINVAL);
        CHECK(descriptor_count() == before);
        expect_lock();
    }
    metadata_cleanup();
    CHECK(lume_vfs_close(&files[0], (unsigned)pair.fd) == 0);
    CHECK(lume_vfs_close(&files[0], (unsigned)pair.other) == 0);
}

static void failed_metadata(bool peek, bool request_output, bool truncated)
{
    struct lume_socket_request pair = socket_pair();
    send_rights(pair.fd, truncated ? 2 : 1);
    metadata_setup();
    descriptor_baseline = descriptor_count();
    unsigned before_revocations = revocations;
    revoke_address = metadata_base + (request_output ? 0 : 4096);
    CHECK(call_message(pair.other, LUME_SOCKET_RECEIVE_MESSAGE, 1,
                       peek ? LUME_SOCKET_MESSAGE_PEEK : 0, 0) == -LUME_EFAULT);
    CHECK(revocations == before_revocations + 1);
    CHECK(descriptor_count() == descriptor_baseline);
    expect_lock();
    metadata_cleanup();
    metadata_setup();
    int64_t result = call_message(pair.other, LUME_SOCKET_RECEIVE_MESSAGE, 2, 0, 0);
    if (peek)
    {
        CHECK(result == 64);
        struct lume_socket_message message = read_message();
        CHECK(message.rights_count == (truncated ? 2u : 1u));
        for (unsigned i = 0; i < message.rights_count; ++i)
            CHECK(lume_vfs_close(&files[0], (unsigned)message.rights[i]) == 0);
    }
    else
        CHECK(result == -LUME_EAGAIN);
    CHECK(descriptor_count() == descriptor_baseline);
    expect_lock();
    metadata_cleanup();
    CHECK(lume_vfs_close(&files[0], (unsigned)pair.fd) == 0);
    CHECK(lume_vfs_close(&files[0], (unsigned)pair.other) == 0);
}

static void no_heap_payload(void)
{
    struct lume_socket_request pair = socket_pair();
    metadata_setup();
    unsigned char data[64];
    memset(data, 0x71, sizeof(data));
    vm_bytes(payload_base, data, sizeof(data), true);
    unsigned before = allocation_live;
    allocation_budget = 0;
    CHECK(call_message(pair.fd, LUME_SOCKET_SEND_MESSAGE, 0, 0, 0) == 64);
    memset(data, 0, sizeof(data));
    vm_bytes(payload_base, data, sizeof(data), true);
    CHECK(call_message(pair.other, LUME_SOCKET_RECEIVE_MESSAGE, 0, 0, 0) == 64);
    allocation_budget = -1;
    CHECK(allocation_live == before);
    vm_bytes(payload_base, data, sizeof(data), false);
    for (unsigned i = 0; i < sizeof(data); ++i)
        CHECK(data[i] == 0x71);
    metadata_cleanup();
    CHECK(lume_vfs_close(&files[0], (unsigned)pair.fd) == 0);
    CHECK(lume_vfs_close(&files[0], (unsigned)pair.other) == 0);
}

static void partial_acquire_unwind(void)
{
    struct lume_socket_request pair = socket_pair();
    metadata_setup();
    struct lume_user_buffer_token bound[LUME_TASK_LIMIT - 2];
    for (unsigned i = 0; i < LUME_TASK_LIMIT - 2; ++i)
        CHECK(lume_user_buffer_acquire(bound + i, 1, payload_base, 1, false) == 0);
    CHECK(token_count == LUME_TASK_LIMIT - 2);
    unsigned before = descriptor_count(), before_allocations = allocation_live;
    allocation_budget = 0;
    CHECK(call_message(pair.fd, LUME_SOCKET_SEND_MESSAGE, 0, 0, 0) == -LUME_ENOMEM);
    allocation_budget = -1;
    CHECK(token_count == LUME_TASK_LIMIT - 2);
    CHECK(descriptor_count() == before && allocation_live == before_allocations);
    for (unsigned i = 0; i < LUME_TASK_LIMIT - 2; ++i)
        lume_user_buffer_release(bound + i);
    CHECK(!token_count);
    CHECK(call_message(pair.other, LUME_SOCKET_RECEIVE_MESSAGE, 0, 0, 0) == -LUME_EAGAIN);
    expect_lock();
    metadata_cleanup();
    CHECK(lume_vfs_close(&files[0], (unsigned)pair.fd) == 0);
    CHECK(lume_vfs_close(&files[0], (unsigned)pair.other) == 0);
}

static void empty_payload(void)
{
    struct lume_socket_request pair = socket_pair();
    metadata_setup();
    uint64_t saved_payload = payload_base;
    payload_base = UINT64_MAX;
    unsigned before = descriptor_count();
    CHECK(call_message_bytes(pair.fd, LUME_SOCKET_SEND_MESSAGE, 0, 0, 0, 0) == 0);
    CHECK(call_message_bytes(pair.other, LUME_SOCKET_RECEIVE_MESSAGE, 0, 0, 0, 0) == 0);
    CHECK(descriptor_count() == before && !token_count);
    payload_base = saved_payload;
    expect_lock();
    metadata_cleanup();
    CHECK(lume_vfs_close(&files[0], (unsigned)pair.fd) == 0);
    CHECK(lume_vfs_close(&files[0], (unsigned)pair.other) == 0);
}

int main(void)
{
#ifdef _WIN32
    CHECK(ConvertThreadToFiber(NULL));
#endif
    static const struct lume_storage_ops storage_ops = {
        .walk = walk, .read = read_file, .write = write_file};
    lume_vfs_init(NULL, 0);
    CHECK(lume_vfs_mount_storage(&storage_ops));
    for (unsigned i = 0; i < 3; ++i)
        lume_files_init(&files[i]);
    baseline_frames = frame_live;
    lume_worker_start();
    active_root = 2;
    payload_base =
        (uint64_t)vm(0, LUME_VM_MAP, 0, 4096, 3, LUME_MAP_PRIVATE | LUME_MAP_ANONYMOUS, -1);
    CHECK(payload_base >= LUME_VM_BASE && lume_user_buffer(payload_base, 4096, true));
    lock_fd = (int)lume_vfs_open(&files[0], "/data/image", LUME_O_RDWR);
    probe_fd = (int)lume_vfs_open(&files[2], "/data/image", LUME_O_RDWR);
    CHECK(lock_fd >= 3 && probe_fd >= 3);
    struct lume_file_lock_request lock = {.version = LUME_FILE_LOCK_VERSION,
                                          .bytes = sizeof(lock),
                                          .fd = lock_fd,
                                          .type = LUME_FILE_LOCK_WRITE,
                                          .length = 1};
    CHECK(lume_vfs_posix_lock(&files[0], 1, false, &lock) == 0);
    expect_lock();
    invalid_inputs();
    for (unsigned peek = 0; peek < 2; ++peek)
        for (unsigned request_output = 0; request_output < 2; ++request_output)
            for (unsigned truncated = 0; truncated < 2; ++truncated)
                failed_metadata(peek != 0, request_output != 0, truncated != 0);
    no_heap_payload();
    partial_acquire_unwind();
    empty_payload();
    lock.type = LUME_FILE_LOCK_UNLOCK;
    CHECK(lume_vfs_posix_lock(&files[0], 1, false, &lock) == 0);
    CHECK(vm(0, LUME_VM_UNMAP, payload_base, 4096, 0, 0, -1) == 0);
    CHECK(!token_count && !unsafe_borrows);
    for (unsigned i = 0; i < 3; ++i)
        lume_files_release(&files[i]);
    while (retired)
        reap(NULL);
    CHECK(frame_live == baseline_frames);
    for (unsigned i = 0; i < LUME_WORKER_LIMIT; ++i)
        release_fiber(&workers[i].fiber);
    for (unsigned i = 0; i < LUME_WORKER_LIMIT - 1; ++i)
        free(extra_stacks[i]);
    printf("PASS_SOCKET_SCM_LIFETIME checks=%u metadata_revocations=%u\n", checks, revocations);
    return 0;
}
