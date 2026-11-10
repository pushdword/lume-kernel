#define main user_io_lifetime_main
#include "user_io_lifetime_test.c"
#undef main
#include "fd.h"
#include "lume/wait.h"

static unsigned small_calls;
static int64_t small_result;

static int64_t small_transfer(void *data, uint64_t bytes, bool writing)
{
    CHECK(bytes && bytes <= 513 && (uintptr_t)data != destination);
    for (unsigned i = 0; i < 2048; ++i)
        if (frames[i])
            CHECK((uintptr_t)data < frames[i] || (uintptr_t)data - frames[i] >= 4096);
    ++small_calls;
    if (pause_io)
    {
        ++pauses;
        CHECK(lume_worker_wait_bits(&resume_io, 1, ticks + 1000000));
    }
    if (writing)
    {
        const unsigned char *input = data;
        for (uint64_t i = 0; i < bytes; ++i)
            CHECK(input[i] == 0xa7);
    }
    else
        memset(data, 0x5c, (size_t)bytes);
    return small_result ? small_result : (int64_t)bytes;
}

static int64_t small_read(void *context, void *data, uint64_t bytes)
{
    (void)context;
    return small_transfer(data, bytes, false);
}

static int64_t small_write(void *context, const void *data, uint64_t bytes)
{
    (void)context;
    return small_transfer((void *)data, bytes, true);
}

static void small_close(void *context)
{
    (void)context;
}

static unsigned small_poll(void *context)
{
    (void)context;
    return LUME_POLLIN | LUME_POLLOUT;
}

static const struct lume_file_ops small_ops = {.kind = LUME_NODE_CHAR,
                                               .close = small_close,
                                               .read = small_read,
                                               .write = small_write,
                                               .poll = small_poll,
                                               .memory_io = true};

static void resident_pipes(void)
{
    shared_setup();
    struct lume_fd_request request = {.version = LUME_FD_VERSION, .bytes = sizeof(request)};
    CHECK(lume_vfs_fd_call(&files[0], LUME_FD_PIPE, &request) == 0);
    int reading = request.fd, writing = request.other;
    unsigned before = allocation_live;
    allocation_budget = 0;
    int64_t transferred = 123;
    CHECK(!lume_user_io_pipe_try(&files[0], 1, image_fd, destination, 1, false, &transferred));
    CHECK(transferred == 123);
    CHECK(!lume_user_io_pipe_try(&files[0], 1, writing, destination, 513, true, &transferred));
    CHECK(!lume_user_io_pipe_try(&files[0], 1, writing, destination + 8191, 2, true, &transferred));
    CHECK(lume_user_io_pipe_try(&files[0], 1, reading, destination, 1, false, &transferred));
    CHECK(transferred == -LUME_EAGAIN);
    struct lume_wait_queue *queue = lume_vfs_pipe_queue(&files[0], reading, false);
    CHECK(lume_wait_subscribe(queue, 7));
    CHECK(
        lume_user_io_pipe_try(&files[0], 1, writing, destination + 4080, 512, true, &transferred));
    CHECK(transferred == 512 && lume_wait_take(queue, 7));
    fill_destination(0x42);
    uint64_t saved_root = active_root;
    active_root = 4;
    CHECK(!lume_user_io_pipe_try(&files[0], 1, reading, destination + 4080, 512, false,
                                 &transferred));
    CHECK(
        lume_user_io_pipe_read_ready(&files[0], 1, reading, destination + 4080, 512, &transferred));
    CHECK(transferred == 512);
    CHECK(active_root == 4);
    active_root = saved_root;
    CHECK(
        lume_user_io_pipe_try(&files[0], 1, writing, destination + 4080, 512, true, &transferred));
    CHECK(
        !lume_user_io_pipe_try(&files[0], 1, reading, destination + 8191, 2, false, &transferred));
    CHECK(vm(0, LUME_VM_PROTECT, destination, 8192, LUME_PROT_READ, 0, -1) == 0);
    uint64_t aliases[2];
    CHECK(!lume_user_buffer_resident(1, destination + 4080, 512, false, aliases));
    CHECK(lume_user_buffer(destination, 8192, false));
    CHECK(lume_user_buffer_resident(1, destination + 4080, 512, false, aliases));
    CHECK(!lume_user_io_pipe_try(&files[0], 1, reading, destination + 4080, 512, false,
                                 &transferred));
    CHECK(vm(0, LUME_VM_PROTECT, destination, 8192, 3, 0, -1) == 0);
    CHECK(lume_user_buffer(destination, 8192, true));
    CHECK(
        lume_user_io_pipe_try(&files[0], 1, reading, destination + 4080, 512, false, &transferred));
    CHECK(transferred == 512);
    uint64_t physical;
    CHECK(lume_mmu_translate(2, destination + 4080, false, true, false, &physical));
    CHECK(*(const unsigned char *)(uintptr_t)physical == 0xa7);
    CHECK(lume_mmu_translate(2, destination + 4591, false, true, false, &physical));
    CHECK(*(const unsigned char *)(uintptr_t)physical == 0xa7);
    CHECK(lume_user_io_pipe_try(&files[0], 1, reading, destination, 1, false, &transferred));
    CHECK(transferred == -LUME_EAGAIN);
    CHECK(lume_vfs_close(&files[0], writing) == 0);
    CHECK(lume_user_io_pipe_try(&files[0], 1, reading, destination, 1, false, &transferred));
    CHECK(transferred == 0);
    CHECK(lume_vfs_close(&files[0], reading) == 0);
    allocation_budget = -1;
    request = (struct lume_fd_request){.version = LUME_FD_VERSION, .bytes = sizeof(request)};
    CHECK(lume_vfs_fd_call(&files[0], LUME_FD_PIPE, &request) == 0);
    CHECK(lume_vfs_close(&files[0], request.fd) == 0);
    CHECK(lume_user_io_pipe_try(&files[0], 1, request.other, destination, 1, true, &transferred));
    CHECK(transferred == -LUME_EPIPE);
    CHECK(lume_user_io_pipe_try(&files[0], 1, request.other, 0, 0, true, &transferred));
    CHECK(transferred == 0);
    CHECK(lume_vfs_close(&files[0], request.other) == 0);
    CHECK(allocation_live == before);
    shared_cleanup();
}

static void admitted_vectors(void)
{
    static unsigned char data[LUME_FILE_IO_MAX];
    shared_setup();
    struct lume_fd_request pipe = {.version = LUME_FD_VERSION, .bytes = sizeof(pipe)};
    CHECK(lume_vfs_fd_call(&files[0], LUME_FD_PIPE, &pipe) == 0);
    lume_vfs_pipe_grow(&files[0], pipe.other, sizeof(data));
    CHECK(lume_vfs_pipe_accept(&files[0], pipe.other, sizeof(data), true) == sizeof(data));
    memset(data, 0xa7, sizeof(data));
    CHECK(lume_vfs_write(&files[0], pipe.other, data, sizeof(data)) == sizeof(data));
    unsigned frames_before = frame_live, allocations_before = allocation_live;
    allocation_budget = 0;
    CHECK(lume_user_io_call(&files[0], 1, pipe.other, UINT64_MAX, sizeof(data), true, false, 0) ==
          -LUME_EAGAIN);
    CHECK(frame_live == frames_before && allocation_live == allocations_before);
    CHECK(lume_vfs_read(&files[0], pipe.fd, data, 512) == 512);
    CHECK(lume_user_io_call(&files[0], 1, pipe.other, UINT64_MAX, 1024, true, false, 0) ==
          -LUME_EAGAIN);
    CHECK(lume_user_io_call(&files[0], 1, pipe.other, destination + 8192 - 512, sizeof(data), true,
                            false, 0) == 512);
    CHECK(lume_vfs_read(&files[0], pipe.fd, data, sizeof(data)) == sizeof(data));
    for (unsigned i = 0; i < sizeof(data); ++i)
        CHECK(data[i] == 0xa7);
    uint64_t physical;
    CHECK(lume_mmu_translate(2, destination, true, true, false, &physical));
    struct lume_io_vector *vectors = (void *)(uintptr_t)(physical + 64);
    vectors[0] = (struct lume_io_vector){destination + 256, 17};
    vectors[1] = (struct lume_io_vector){destination + 4096, 31};
    CHECK(lume_user_io_vector_call(&files[0], 1, pipe.other, destination + 64, 2, true) == 48);
    memset(data, 0, 48);
    CHECK(lume_vfs_read(&files[0], pipe.fd, data, 48) == 48);
    for (unsigned i = 0; i < 48; ++i)
        CHECK(data[i] == 0xa7);
    memset(data, 0x3c, 48);
    CHECK(lume_vfs_write(&files[0], pipe.other, data, 48) == 48);
    vectors[1].buffer = destination + 8192;
    CHECK(lume_user_io_vector_call(&files[0], 1, pipe.fd, destination + 64, 2, false) ==
          -LUME_EFAULT);
    CHECK(lume_vfs_pipe_accept(&files[0], pipe.fd, sizeof(data), false) == 48);
    vectors[1].buffer = destination + 4096;
    CHECK(lume_user_io_vector_call(&files[0], 1, pipe.fd, destination + 64, 2, false) == 48);
    CHECK(((unsigned char *)(uintptr_t)physical)[256] == 0x3c);
    CHECK(lume_mmu_translate(2, destination + 4096, true, true, false, &physical));
    CHECK(*(const unsigned char *)(uintptr_t)physical == 0x3c);
    CHECK(lume_user_io_vector_call(&files[0], 1, pipe.fd, 0, LUME_FILE_IO_VECTOR_MAX + 1, false) ==
          -LUME_EINVAL);
    allocation_budget = -1;
    CHECK(lume_vfs_close(&files[0], pipe.fd) == 0);
    CHECK(lume_vfs_close(&files[0], pipe.other) == 0);
    shared_cleanup();
}

int main(void)
{
#ifdef _WIN32
    CHECK(ConvertThreadToFiber(NULL));
#endif
    lume_vfs_init(NULL, 0);
    baseline_metadata = allocation_live;
    for (unsigned i = 0; i < 3; ++i)
        lume_files_init(&files[i]);
    unsigned initial_frames = frame_live;
    lume_worker_start();
    image_fd = lume_vfs_attach(&files[0], &small_ops, NULL, 0);
    CHECK(image_fd >= 3);
    baseline_frames = frame_live;
    const unsigned sizes[] = {1, 17, 511, 512};
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i)
        for (operation = 0; operation < 2; ++operation)
        {
            shared_setup();
            transfer_bytes = sizes[i];
            unsigned before = allocation_live, calls = small_calls;
            allocation_budget = 0;
            run_transfer(false);
            allocation_budget = -1;
            CHECK(result == (int64_t)transfer_bytes && small_calls == calls + 1);
            CHECK(allocation_live == before);
            if (!operation)
            {
                uint64_t physical;
                CHECK(lume_mmu_translate(2, destination, true, true, false, &physical));
                for (unsigned byte = 0; byte < 4096; ++byte)
                    CHECK(((const unsigned char *)(uintptr_t)physical)[byte] ==
                          (byte < transfer_bytes ? 0x5c : 0xa7));
            }
            shared_cleanup();
        }
    const int64_t read_results[] = {17, -LUME_EIO};
    for (unsigned i = 0; i < sizeof(read_results) / sizeof(read_results[0]); ++i)
    {
        shared_setup();
        operation = 0;
        transfer_bytes = 512;
        small_result = read_results[i];
        run_transfer(false);
        CHECK(result == small_result);
        uint64_t physical;
        CHECK(lume_mmu_translate(2, destination, true, true, false, &physical));
        for (unsigned byte = 0; byte < 4096; ++byte)
            CHECK(((const unsigned char *)(uintptr_t)physical)[byte] ==
                  (small_result > 0 && byte < (uint64_t)small_result ? 0x5c : 0xa7));
        shared_cleanup();
    }
    small_result = 0;
    for (operation = 0; operation < 2; ++operation)
    {
        shared_setup();
        transfer_bytes = 512;
        unsigned before = allocation_live;
        allocation_budget = 0;
        run_transfer(true);
        allocation_budget = -1;
        CHECK(allocation_live == before);
        active_root = 4;
        CHECK(lume_worker_submit_on(LUME_WORK_GENERAL, truncate_operation, NULL));
        CHECK(lume_worker_resume_on(LUME_WORK_GENERAL));
        CHECK(!lume_worker_busy_on(LUME_WORK_GENERAL));
        finish_transfer();
        CHECK(result == (operation ? 512 : -LUME_EFAULT));
        shared_cleanup();
    }
    shared_setup();
    transfer_bytes = 513;
    operation = 0;
    allocation_budget = 0;
    unsigned calls = small_calls;
    run_transfer(false);
    allocation_budget = -1;
    CHECK(result == 513 && small_calls == calls + 1);
    run_transfer(false);
    CHECK(result == 513 && small_calls == calls + 2);
    shared_cleanup();
    resident_pipes();
    admitted_vectors();
    for (unsigned i = 0; i < 3; ++i)
        lume_files_release(&files[i]);
    while (retired)
        reap(NULL);
    CHECK(frame_live == initial_frames && allocation_live == baseline_metadata);
    for (unsigned i = 0; i < LUME_WORKER_LIMIT; ++i)
        release_fiber(&workers[i].fiber);
    for (unsigned i = 0; i < LUME_WORKER_LIMIT - 1; ++i)
        free(extra_stacks[i]);
    printf("PASS_SMALL_IO checks=%u callbacks=%u pauses=%u\n", checks, small_calls, pauses);
    return 0;
}
