#define main user_io_lifetime_main
#include "user_io_lifetime_test.c"
#undef main

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
    CHECK(result == -LUME_ENOMEM && small_calls == calls);
    run_transfer(false);
    CHECK(result == 513 && small_calls == calls + 1);
    shared_cleanup();
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
