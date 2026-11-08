#define main user_io_lifetime_main
#include "user_io_lifetime_test.c"
#undef main
#undef active
#include "lume/graphics_device.h"
#include "lume/platform_driver.h"
#include "lume/process.h"

static bool claimed, revoke_claim;
static unsigned backend_calls, backend_reads, mutations;
static unsigned mutation;
static uint64_t request_address;
static int replacement_fd;
static struct lume_graphics_request graphics_request;
static bool user_call;
static int graphics_result;

enum
{
    MUTATE_NONE,
    MUTATE_TRUNCATE,
    MUTATE_REQUEST,
    MUTATE_REPLACE,
    MUTATE_RELEASE
};

static struct lume_graphics_request graphics_request_init(void)
{
    return (struct lume_graphics_request){
        .version = LUME_GRAPHICS_VERSION, .bytes = sizeof(struct lume_graphics_request), .fd = -1};
}

bool lume_task_capable(unsigned owner, uint64_t capabilities)
{
    CHECK(owner == 1 && capabilities == LUME_CAP_DRIVER_ADMIN);
    return true;
}

void lume_gpu_release(unsigned owner)
{
    CHECK(owner == 1);
}

int lume_platform_graphics_query(struct lume_graphics_request *request)
{
    CHECK(locked);
    request->width = 512;
    request->height = 64;
    request->pitch = 2048;
    request->format = LUME_GRAPHICS_XRGB8888;
    return 0;
}

int lume_platform_graphics_claim(bool acquire)
{
    CHECK(locked && acquire != claimed);
    claimed = acquire;
    if (acquire && revoke_claim)
    {
        revoke_claim = false;
        CHECK(vm(0, LUME_VM_PROTECT, request_address, 4096, 1, 0, -1) == 0);
    }
    return 0;
}

#include "../../src/device/graphics.c"

static void copy_request(bool to_user)
{
    struct lume_user_buffer_token token = {0};
    CHECK(lume_user_buffer_acquire(&token, 1, request_address, sizeof(graphics_request), to_user) ==
          0);
    CHECK(lume_user_buffer_copy(&token, &graphics_request, sizeof(graphics_request), to_user));
    lume_user_buffer_release(&token);
}

static int acquire_graphics(void)
{
    struct lume_graphics_request request = graphics_request_init();
    CHECK(lume_graphics_call(&files[0], 1, LUME_GRAPHICS_ACQUIRE, &request) == 0);
    CHECK(request.fd >= 3 && claimed && active);
    return request.fd;
}

/* A second CPU mutates mappings while the display worker owns only its compact snapshot. */
static void mutate_mappings(unsigned action)
{
    CHECK(!locked && !cpu_index && workers[LUME_WORK_DISPLAY].computing);
    cpu_index = 1;
    active_root = 4;
    lume_smp_lock();
    if (action == MUTATE_TRUNCATE)
    {
        CHECK(vm(1, LUME_VM_RESIZE, 0, 0, 0, 0, other_fd) == 0);
        CHECK(frame_live == baseline_frames);
    }
    else if (action == MUTATE_REQUEST)
        CHECK(vm(0, LUME_VM_PROTECT, request_address, 4096, 1, 0, -1) == 0);
    else
    {
        struct session *previous = active;
        uint64_t generation = previous->generation;
        CHECK(lume_vfs_close(&files[0], graphics_request.fd) == 0);
        CHECK(!claimed && !active);
        if (action == MUTATE_REPLACE)
        {
            replacement_fd = acquire_graphics();
            CHECK(replacement_fd == graphics_request.fd && active == previous);
            CHECK(active->generation != generation);
        }
    }
    lume_smp_unlock();
    cpu_index = 0;
    active_root = 2;
    ++mutations;
}

static void backend_compute(void *context)
{
    const struct lume_graphics_request *request = context;
    CHECK(!locked && request->pitch == request->width * 4);
    size_t bytes = (size_t)request->height * request->pitch;
    CHECK(bytes <= LUME_FILE_IO_MAX);
    CHECK(kernel_buffer((void *)(uintptr_t)request->pixels, bytes));
    CHECK(request->pixels != destination);
    if (mutation)
    {
        unsigned action = mutation;
        mutation = MUTATE_NONE;
        mutate_mappings(action);
    }
    for (unsigned i = 0; i < bytes; ++i)
        CHECK(((const unsigned char *)(uintptr_t)request->pixels)[i] == 0xa7);
    ++backend_reads;
}

int lume_platform_graphics_present(const struct lume_graphics_request *request)
{
    CHECK(locked && claimed);
    ++backend_calls;
    lume_worker_compute(backend_compute, (void *)request);
    return 0;
}

static void graphics_operation(void *context)
{
    (void)context;
    graphics_result =
        user_call ? lume_graphics_user_call(&files[0], 1, LUME_GRAPHICS_PRESENT, request_address,
                                            sizeof(graphics_request))
                  : lume_graphics_call(&files[0], 1, LUME_GRAPHICS_PRESENT, &graphics_request);
}

static void present(unsigned fd, unsigned height, unsigned change, bool through_user)
{
    graphics_request = graphics_request_init();
    graphics_request.fd = (int)fd;
    graphics_request.pixels = destination;
    graphics_request.width = 1;
    graphics_request.height = height;
    graphics_request.pitch = 4;
    graphics_request.format = LUME_GRAPHICS_XRGB8888;
    mutation = change;
    user_call = through_user;
    if (through_user)
        copy_request(true);
    CHECK(lume_worker_submit_on(LUME_WORK_DISPLAY, graphics_operation, NULL));
    unsigned budget = 100;
    while (lume_worker_busy_on(LUME_WORK_DISPLAY))
    {
        CHECK(budget--);
        CHECK(lume_worker_resume_on(LUME_WORK_DISPLAY));
    }
    CHECK(locked && !cpu_index && !present_buffers);
}

static void ownership_cases(void)
{
    shared_setup();
    unsigned fd = (unsigned)acquire_graphics();
    unsigned before = allocation_live;
    present(fd, 16, MUTATE_TRUNCATE, false);
    CHECK(graphics_result == 0 && backend_reads == 1 && mutations == 1);
    CHECK(allocation_live < before);
    CHECK(lume_vfs_close(&files[0], fd) == 0);
    shared_cleanup();

    shared_setup();
    fd = (unsigned)acquire_graphics();
    unsigned calls = backend_calls;
    present(fd, 32, MUTATE_TRUNCATE, false);
    CHECK(graphics_result == -LUME_EFAULT && backend_calls == calls + 1);
    CHECK(lume_vfs_close(&files[0], fd) == 0);
    shared_cleanup();

    shared_setup();
    fd = (unsigned)acquire_graphics();
    before = allocation_live;
    allocation_budget = 0;
    calls = backend_calls;
    present(fd, 16, MUTATE_NONE, false);
    CHECK(graphics_result == -LUME_ENOMEM && backend_calls == calls);
    CHECK(allocation_live == before);
    allocation_budget = -1;
    CHECK(lume_vfs_close(&files[0], fd) == 0);
    shared_cleanup();
}

static void request_cases(void)
{
    graphics_request = graphics_request_init();
    copy_request(true);
    revoke_claim = true;
    CHECK(lume_graphics_user_call(&files[0], 1, LUME_GRAPHICS_ACQUIRE, request_address,
                                  sizeof(graphics_request)) == -LUME_EFAULT);
    CHECK(!claimed && !active && !present_buffers);
    copy_request(false);
    CHECK(graphics_request.fd == -1);
    CHECK(vm(0, LUME_VM_PROTECT, request_address, 4096, 3, 0, -1) == 0);

    shared_setup();
    unsigned fd = (unsigned)acquire_graphics();
    unsigned before = allocation_live;
    present(fd, 16, MUTATE_REQUEST, true);
    CHECK(graphics_result == -LUME_EFAULT && claimed && active);
    CHECK(allocation_live == before);
    CHECK(lume_vfs_close(&files[0], fd) == 0);
    shared_cleanup();
    CHECK(vm(0, LUME_VM_PROTECT, request_address, 4096, 3, 0, -1) == 0);
}

static void session_cases(void)
{
    for (unsigned height = 16; height <= 32; height += 16)
    {
        shared_setup();
        unsigned fd = (unsigned)acquire_graphics();
        present(fd, height, MUTATE_REPLACE, false);
        CHECK(graphics_result == -LUME_EBADF && replacement_fd == (int)fd);
        CHECK(lume_vfs_close(&files[0], (unsigned)replacement_fd) == 0);
        shared_cleanup();
    }
    shared_setup();
    unsigned fd = (unsigned)acquire_graphics();
    present(fd, 16, MUTATE_RELEASE, false);
    CHECK(graphics_result == -LUME_EBADF && !claimed && !active);
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
    request_address =
        (uint64_t)vm(0, LUME_VM_MAP, 0, 4096, 3, LUME_MAP_PRIVATE | LUME_MAP_ANONYMOUS, -1);
    CHECK(request_address >= LUME_VM_BASE && lume_user_buffer(request_address, 4096, true));
    baseline_frames = frame_live;
    ownership_cases();
    request_cases();
    session_cases();

    struct lume_user_buffer_token tokens[LUME_TASK_LIMIT], extra;
    for (unsigned i = 0; i < LUME_TASK_LIMIT; ++i)
        CHECK(lume_user_buffer_acquire(tokens + i, 1, request_address, 1, false) == 0);
    CHECK(lume_user_buffer_acquire(&extra, 1, request_address, 1, false) == -LUME_ENOMEM);
    for (unsigned i = 0; i < LUME_TASK_LIMIT; ++i)
        lume_user_buffer_release(tokens + i);
    CHECK(vm(0, LUME_VM_UNMAP, request_address, 4096, 0, 0, -1) == 0);
    for (unsigned i = 0; i < 3; ++i)
        lume_files_release(&files[i]);
    while (retired)
        reap(NULL);
    CHECK(frame_live == initial_frames);
    CHECK(allocation_live == baseline_metadata);
    CHECK(!claimed && !active);
    for (unsigned i = 0; i < LUME_WORKER_LIMIT; ++i)
        release_fiber(&workers[i].fiber);
    for (unsigned i = 0; i < LUME_WORKER_LIMIT - 1; ++i)
        free(extra_stacks[i]);
    printf("PASS_GRAPHICS_LIFETIME checks=%u compact_reads=%u mutations=%u\n", checks,
           backend_reads, mutations);
    return 0;
}
