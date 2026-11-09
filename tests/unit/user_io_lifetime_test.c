#ifndef _WIN32
#define _XOPEN_SOURCE 700
#endif
#include "lume/early.h"
#include "lume/vfs.h"
#include "lume/external_file.h"
#include "lume/user_io.h"
#include "lume/user_buffer.h"
#include "lume/file_io_call.h"
#include "lume/file_io.h"
#include "lume/worker.h"
#include "lume/activity.h"
#include "lume/smp.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#include <malloc.h>
#else
#include <ucontext.h>
#endif

static unsigned checks;
#define CHECK(value)                                                                               \
    do                                                                                             \
    {                                                                                              \
        ++checks;                                                                                  \
        if (!(value))                                                                              \
        {                                                                                          \
            fprintf(stderr, "FAIL %u: %s\n", __LINE__, #value);                                    \
            abort();                                                                               \
        }                                                                                          \
    } while (0)

uint64_t __worker_stack[2], __worker_top[1];
#define LUME_ARCH_FIBER_H

struct lume_fiber
{
#ifdef _WIN32
    void *handle;
    void (*target)(void);
#else
    ucontext_t saved;
    void *memory;
#endif
};

#ifdef _WIN32
static VOID WINAPI host_entry(void *context)
{
    struct lume_fiber *fiber = context;
    fiber->target();
    abort();
}
#endif

static void release_fiber(struct lume_fiber *fiber)
{
#ifdef _WIN32
    if (fiber->handle)
        DeleteFiber(fiber->handle);
    fiber->handle = NULL;
#else
    free(fiber->memory);
    fiber->memory = NULL;
#endif
}

static void lume_fiber_init(struct lume_fiber *fiber, uintptr_t top, void (*target)(void))
{
    CHECK(top != 0);
    release_fiber(fiber);
#ifdef _WIN32
    fiber->target = target;
    fiber->handle = CreateFiberEx(65536, 1048576, FIBER_FLAG_FLOAT_SWITCH, host_entry, fiber);
    CHECK(fiber->handle != NULL);
#else
    CHECK(getcontext(&fiber->saved) == 0);
    fiber->memory = malloc(1048576);
    CHECK(fiber->memory != NULL);
    fiber->saved.uc_stack.ss_sp = fiber->memory;
    fiber->saved.uc_stack.ss_size = 1048576;
    fiber->saved.uc_stack.ss_flags = 0;
    fiber->saved.uc_link = NULL;
    makecontext(&fiber->saved, target, 0);
#endif
}

static void lume_fiber_switch(struct lume_fiber *from, const struct lume_fiber *to)
{
#ifdef _WIN32
    if (!from->handle)
        from->handle = GetCurrentFiber();
    CHECK(to->handle && to->handle != GetCurrentFiber());
    SwitchToFiber(to->handle);
#else
    CHECK(swapcontext(&from->saved, &to->saved) == 0);
#endif
}

#define LUME_COUNTER_H
static uint64_t ticks = 100;

static uint64_t lume_counter_read(void)
{
    return ticks;
}

static uint64_t lume_counter_frequency(void)
{
    return 1000000;
}

static bool locked = true;
static uint64_t turn;
static unsigned cpu_index;

unsigned lume_cpu_index(void)
{
    return cpu_index;
}

unsigned lume_cpu_count(void)
{
    return 4;
}

void lume_smp_lock(void)
{
    CHECK(!locked);
    locked = true;
    ++turn;
}

void lume_smp_unlock(void)
{
    CHECK(locked);
    locked = false;
}

bool lume_smp_owned(void)
{
    return locked;
}

bool lume_smp_contended(void)
{
    return false;
}

uint64_t lume_smp_lock_turn(void)
{
    return turn;
}

uint64_t lume_smp_wait_total(void)
{
    return 0;
}

void lume_smp_wake(void)
{
}

uint64_t lume_irq_save(void)
{
    return 0;
}

void lume_irq_restore(uint64_t flags)
{
    CHECK(!flags);
}

void lume_value(const char *name, uint64_t value)
{
    (void)name;
    (void)value;
}

_Noreturn void lume_fail(const char *why)
{
    fprintf(stderr, "PRODUCT FAIL: %s\n", why);
    abort();
}

#include "../../src/core/activity.c"
#include "../../src/core/worker.c"
#undef stack
#undef ENOTSUP
#include "../../src/mm/vm.c"

static struct
{
    void *pointer;
    size_t bytes;
} allocations[1024];

static unsigned allocation_live, baseline_metadata;
static int allocation_budget = -1;
static struct lume_file_io_request wire;

void *lume_kernel_allocate(size_t bytes)
{
    if (!allocation_budget)
        return NULL;
    if (allocation_budget > 0)
        --allocation_budget;
    for (unsigned i = 0; i < 1024; ++i)
        if (!allocations[i].pointer)
        {
            void *pointer = calloc(1, bytes);
            CHECK(pointer);
            allocations[i].pointer = pointer;
            allocations[i].bytes = bytes;
            ++allocation_live;
            return pointer;
        }
    abort();
}

void lume_kernel_free(void *pointer)
{
    if (!pointer)
        return;
    for (unsigned i = 0; i < 1024; ++i)
        if (allocations[i].pointer == pointer)
        {
            free(pointer);
            allocations[i].pointer = NULL;
            allocations[i].bytes = 0;
            CHECK(allocation_live);
            --allocation_live;
            return;
        }
    abort();
}

static uint64_t frames[2048];

static bool kernel_buffer(const void *pointer, size_t bytes)
{
    for (unsigned i = 0; i < 2048; ++i)
        if (frames[i] && (uintptr_t)pointer < frames[i] + 4096u &&
            frames[i] < (uintptr_t)pointer + bytes)
            return false;
    for (unsigned i = 0; i < 1024; ++i)
        if (allocations[i].pointer == pointer && allocations[i].bytes >= bytes)
            return true;
    return pointer != NULL && bytes <= LUME_FILE_IO_MAX;
}

static unsigned frame_live, baseline_frames;
static void *reusable_frame;
static bool reuse_frames;

uint64_t lume_kernel_allocation_pages(const void *pointer)
{
    for (unsigned i = 0; i < 1024; ++i)
        if (allocations[i].pointer == pointer)
            return (allocations[i].bytes + 4095u) / 4096u;
    return 0;
}

uint64_t lume_pages_total(void)
{
    return 65536;
}

uint64_t lume_page_alloc_uninitialized(uint64_t minimum)
{
    return lume_page_alloc(minimum);
}

bool lume_task_storage_context(void)
{
    return lume_worker_current() == LUME_WORK_STORAGE;
}

void lume_task_vm_ready(void)
{
}

void lume_task_vm_buffer_wait(void)
{
    abort();
}

static struct
{
    uint64_t base, live;
    unsigned pages;
} spans[64];

uint64_t lume_page_alloc(uint64_t minimum)
{
    CHECK(!minimum);
    void *pointer = reusable_frame;
    reusable_frame = NULL;
    if (!pointer)
    {
#ifdef _WIN32
        pointer = _aligned_malloc(4096, 4096);
#else
        pointer = aligned_alloc(4096, 4096);
#endif
    }
    CHECK(pointer);
    memset(pointer, 0, 4096);
    for (unsigned i = 0; i < 2048; ++i)
        if (!frames[i])
        {
            frames[i] = (uintptr_t)pointer;
            ++frame_live;
            return frames[i];
        }
    abort();
}

static struct
{
    unsigned task;
    uint64_t address, physical;
    bool writable[2], executable[2], live;
} mappings[2048];

static uint64_t active_root = 2;

bool lume_page_free(uint64_t physical)
{
    for (unsigned i = 0; i < 2048; ++i)
        if (mappings[i].live)
            CHECK(mappings[i].physical != physical);
    for (unsigned i = 0; i < 2048; ++i)
        if (frames[i] == physical)
        {
            frames[i] = 0;
            --frame_live;
            for (unsigned span = 0; span < 64; ++span)
                if (spans[span].base && physical >= spans[span].base &&
                    physical - spans[span].base < (uint64_t)spans[span].pages * 4096u)
                {
                    unsigned page = (unsigned)((physical - spans[span].base) / 4096u);
                    CHECK(spans[span].live & (UINT64_C(1) << page));
                    spans[span].live &= ~(UINT64_C(1) << page);
                    if (!spans[span].live)
                    {
#ifdef _WIN32
                        _aligned_free((void *)(uintptr_t)spans[span].base);
#else
                        free((void *)(uintptr_t)spans[span].base);
#endif
                        spans[span].base = 0;
                    }
                    return true;
                }
            if (reuse_frames && !reusable_frame)
                reusable_frame = (void *)(uintptr_t)physical;
            else
            {
#ifdef _WIN32
                _aligned_free((void *)(uintptr_t)physical);
#else
                free((void *)(uintptr_t)physical);
#endif
            }
            return true;
        }
    return false;
}

uint64_t lume_pages_alloc_span(unsigned count)
{
    /* Lifetime worker stacks are released after the final host fiber return. */
    return (uintptr_t)calloc(count, 4096);
}

uint64_t lume_pages_alloc_span_uninitialized(unsigned count)
{
    CHECK(count && count <= 64);
#ifdef _WIN32
    void *memory = _aligned_malloc((size_t)count * 4096u, 4096);
#else
    void *memory = aligned_alloc(4096, (size_t)count * 4096u);
#endif
    if (!memory)
        return 0;
    for (unsigned span = 0; span < 64; ++span)
        if (!spans[span].base)
        {
            spans[span].base = (uintptr_t)memory;
            spans[span].pages = count;
            spans[span].live = count == 64 ? UINT64_MAX : (UINT64_C(1) << count) - 1;
            unsigned page = 0;
            for (unsigned i = 0; i < 2048 && page < count; ++i)
                if (!frames[i])
                    frames[i] = (uintptr_t)memory + (uint64_t)page++ * 4096u;
            CHECK(page == count);
            frame_live += count;
            memset(memory, 0xa5, (size_t)count * 4096u);
            return (uintptr_t)memory;
        }
    abort();
}

uint64_t lume_mmu_task_view_root(unsigned task, bool writer)
{
    return (task + 1) * 2 + writer;
}

uint64_t lume_mmu_task_root(unsigned task)
{
    return lume_mmu_task_view_root(task, false);
}

bool lume_mmu_root_active(uint64_t root)
{
    return root == active_root;
}

bool lume_mmu_root_matches(unsigned task, uint64_t root)
{
    return root / 2 == task + 1;
}

bool lume_mmu_jit_active(unsigned task)
{
    return active_root == lume_mmu_task_view_root(task, true);
}

bool lume_mmu_translate(uint64_t root, uint64_t address, bool write, bool user, bool execute,
                        uint64_t *physical)
{
    (void)user;
    if (root == 2 && !write && !execute && address >= (uintptr_t)&wire &&
        address - (uintptr_t)&wire < sizeof(wire))
    {
        *physical = address;
        return true;
    }
    unsigned task = (unsigned)(root / 2) - 1, view = (unsigned)root & 1;
    for (unsigned i = 0; i < 2048; ++i)
        if (mappings[i].live && mappings[i].task == task &&
            mappings[i].address == (address & ~UINT64_C(4095)))
        {
            if ((write && !mappings[i].writable[view]) ||
                (execute && !mappings[i].executable[view]))
                return false;
            *physical = mappings[i].physical + address % 4096;
            return true;
        }
    return false;
}

static bool map_page(unsigned task, uint64_t address, uint64_t physical, bool write, bool execute,
                     bool jit)
{
    lume_user_buffer_invalidate(task, address, 4096);
    for (unsigned i = 0; i < 2048; ++i)
        if (!mappings[i].live)
        {
            mappings[i].task = task;
            mappings[i].address = address;
            mappings[i].physical = physical;
            mappings[i].live = true;
            for (unsigned view = 0; view < 2; ++view)
            {
                mappings[i].writable[view] = write && (!jit || !execute || view == 1);
                mappings[i].executable[view] = execute && (!jit || view == 0);
            }
            return true;
        }
    return false;
}

bool lume_mmu_map_user(unsigned task, uint64_t address, uint64_t physical, bool write, bool execute)
{
    return map_page(task, address, physical, write, execute, false);
}

bool lume_mmu_map_user_jit(unsigned task, uint64_t address, uint64_t physical, bool write,
                           bool execute)
{
    return map_page(task, address, physical, write, execute, true);
}

bool lume_mmu_unmap_user_batch(unsigned task, const uint64_t *addresses, unsigned count)
{
    for (unsigned n = 0; n < count; ++n)
    {
        lume_user_buffer_invalidate(task, addresses[n], 4096);
        for (unsigned i = 0; i < 2048; ++i)
            if (mappings[i].live && mappings[i].task == task && mappings[i].address == addresses[n])
                mappings[i].live = false;
    }
    return true;
}

void lume_sync_code(uint64_t address, size_t bytes)
{
    (void)address;
    (void)bytes;
}

void lume_sync_code_defer(uint64_t address, size_t bytes)
{
    (void)address;
    (void)bytes;
}

void lume_sync_code_clean_finish(void)
{
}

void lume_sync_code_finish(void)
{
}

int64_t lume_task_protect_image(unsigned task, uint64_t address, uint64_t bytes, unsigned prot)
{
    (void)task;
    (void)address;
    (void)bytes;
    (void)prot;
    return -LUME_ENOMEM;
}

bool lume_user_buffer(uint64_t address, uint64_t bytes, bool write)
{
    if (address == (uintptr_t)&wire)
        return bytes == sizeof(wire) && !write;
    unsigned task = (unsigned)(active_root / 2) - 1;
    while (bytes)
    {
        uint64_t physical;
        if (!lume_mmu_translate(active_root, address, write, true, false, &physical) &&
            lume_vm_fault(task, address, write, false) != 1)
            return false;
        uint64_t part = 4096 - address % 4096;
        if (part > bytes)
            part = bytes;
        bytes -= part;
        address += part;
    }
    return true;
}

void lume_console_write(const char *text, size_t bytes)
{
    (void)text;
    (void)bytes;
}

int lume_console_getc(void)
{
    return -LUME_EAGAIN;
}

bool lume_console_ready(void)
{
    return false;
}

static struct lume_files files[3];
static int image_fd, shared_fd, other_fd;
static uint64_t destination;
static unsigned char image_bytes[65536], received_bytes[65536];
static unsigned io_calls, unsafe_borrows, pauses;
static unsigned short_count;
static int io_error;
static bool pause_io;
static volatile uint32_t resume_io;

static bool walk(lume_storage_visit visit, void *context)
{
    return visit("/data/image", false, sizeof(image_bytes), context);
}

static int device_io(const char *path, uint64_t offset, void *data, size_t bytes, bool writing)
{
    CHECK(!strcmp(path, "/data/image") && offset <= sizeof(image_bytes));
    ++io_calls;
    if (pause_io)
    {
        ++pauses;
        CHECK(lume_worker_wait_bits(&resume_io, 1, ticks + 1000000));
    }
    if (io_error)
        return io_error;
    if (short_count && bytes > short_count)
        bytes = short_count;
    if (bytes > sizeof(image_bytes) - offset)
        bytes = sizeof(image_bytes) - offset;
    if (!kernel_buffer(data, bytes))
    {
        uint64_t physical;
        if (!lume_mmu_translate(2, (uintptr_t)data, !writing, true, false, &physical))
            ++unsafe_borrows;
        return -LUME_EFAULT;
    }
    if (writing)
        memcpy(received_bytes, data, bytes);
    else
        memcpy(data, image_bytes + offset, bytes);
    return (int)bytes;
}

static int read_file(const char *path, uint64_t offset, void *data, size_t bytes)
{
    return device_io(path, offset, data, bytes, false);
}

static int write_file(const char *path, uint64_t offset, const void *data, size_t bytes)
{
    return device_io(path, offset, (void *)data, bytes, true);
}

static int64_t vm(unsigned task, unsigned operation, uint64_t address, uint64_t length,
                  unsigned protection, unsigned flags, int fd)
{
    struct lume_vm_request request = {.version = LUME_VM_VERSION,
                                      .address = address,
                                      .length = length,
                                      .protection = protection,
                                      .flags = flags,
                                      .fd = fd,
                                      .name = "lifetime"};
    return lume_vm_call(&files[task], task + 1, operation, &request);
}

static unsigned operation;
static int64_t result;
static size_t transfer_bytes = 8192;

static void transfer_operation(void *unused)
{
    (void)unused;
    if (operation < 2)
        result = lume_user_io_call(&files[0], 1, image_fd, destination, transfer_bytes,
                                   operation == 1, false, 0);
    else
    {
        wire = (struct lume_file_io_request){.version = LUME_FILE_IO_VERSION,
                                             .bytes = sizeof(wire),
                                             .fd = image_fd,
                                             .buffer = destination,
                                             .count = transfer_bytes,
                                             .offset = 17};
        result = lume_file_io_call(&files[0], 1,
                                   operation == 3 ? LUME_FILE_IO_PWRITE : LUME_FILE_IO_PREAD,
                                   (uintptr_t)&wire, sizeof(wire));
    }
}

static void shared_setup(void)
{
    active_root = 2;
    shared_fd = (int)vm(0, LUME_VM_CREATE, 0, 0, 0, LUME_MEMFD_ALLOW_SEALING, -1);
    CHECK(shared_fd >= 3);
    CHECK(lume_vfs_resize(&files[0], shared_fd, 8192) == 0);
    int object = lume_vfs_hold(&files[0], shared_fd);
    CHECK(object >= 0);
    other_fd = lume_vfs_install(&files[1], object);
    CHECK(other_fd >= 3);
    destination = (uint64_t)vm(0, LUME_VM_MAP, 0, 8192, 3, LUME_MAP_SHARED, shared_fd);
    CHECK(destination >= LUME_VM_BASE);
    CHECK(lume_user_buffer(destination, 8192, true));
    for (unsigned offset = 0; offset < 8192; offset += 4096)
    {
        uint64_t physical;
        CHECK(lume_mmu_translate(2, destination + offset, true, true, false, &physical));
        memset((void *)(uintptr_t)physical, 0xa7, 4096);
    }
}

static void shared_cleanup(void)
{
    active_root = 2;
    CHECK(vm(0, LUME_VM_UNMAP, destination, 8192, 0, 0, -1) == 0);
    CHECK(lume_vfs_close(&files[0], shared_fd) == 0);
    lume_files_release(&files[1]);
    lume_files_init(&files[1]);
    while (retired)
        reap(NULL);
    CHECK(frame_live == baseline_frames);
}

static void run_transfer(bool paused)
{
    pause_io = paused;
    resume_io = 0;
    active_root = 2;
    CHECK(lume_worker_submit_on(LUME_WORK_STORAGE, transfer_operation, NULL));
    CHECK(lume_worker_resume_on(LUME_WORK_STORAGE));
    CHECK(lume_worker_busy_on(LUME_WORK_STORAGE) == paused);
}

static void truncate_operation(void *unused)
{
    (void)unused;
    CHECK(vm(1, LUME_VM_RESIZE, 0, 0, 0, 0, other_fd) == 0);
}

static void finish_transfer(void)
{
    resume_io = 1;
    active_root = 2;
    CHECK(lume_worker_resume_on(LUME_WORK_STORAGE));
    CHECK(!lume_worker_busy_on(LUME_WORK_STORAGE));
}

static void fill_destination(unsigned char value)
{
    for (unsigned offset = 0; offset < 8192; offset += 4096)
    {
        uint64_t physical;
        CHECK(lume_mmu_translate(2, destination + offset, true, true, false, &physical));
        memset((void *)(uintptr_t)physical, value, 4096);
    }
}

static void expect_destination(unsigned char value)
{
    for (unsigned offset = 0; offset < 8192; offset += 4096)
    {
        uint64_t physical;
        CHECK(lume_mmu_translate(2, destination + offset, true, true, false, &physical));
        for (unsigned i = 0; i < 4096; ++i)
            CHECK(((unsigned char *)(uintptr_t)physical)[i] == value);
    }
}

static void replacement_cases(void)
{
    operation = 0;
    for (unsigned kind = 0; kind < 3; ++kind)
    {
        shared_setup();
        run_transfer(true);
        if (!kind)
        {
            CHECK(vm(0, LUME_VM_UNMAP, destination, 8192, 0, 0, -1) == 0);
            CHECK(vm(0, LUME_VM_MAP, destination, 8192, 3, LUME_MAP_SHARED | LUME_MAP_FIXED,
                     shared_fd) == (int64_t)destination);
        }
        else if (kind == 1)
        {
            reuse_frames = true;
            active_root = 4;
            CHECK(vm(1, LUME_VM_RESIZE, 0, 0, 0, 0, other_fd) == 0);
            CHECK(vm(1, LUME_VM_RESIZE, 0, 8192, 0, 0, other_fd) == 0);
            active_root = 2;
        }
        else
        {
            CHECK(vm(0, LUME_VM_PROTECT, destination, 8192, LUME_PROT_READ, 0, -1) == 0);
            CHECK(vm(0, LUME_VM_PROTECT, destination, 8192, 3, 0, -1) == 0);
        }
        CHECK(lume_user_buffer(destination, 8192, true));
        fill_destination(0x39);
        finish_transfer();
        CHECK(result == -LUME_EFAULT);
        expect_destination(0x39);
        reuse_frames = false;
        shared_cleanup();
    }
}

static void result_cases(void)
{
    for (operation = 0; operation < 4; ++operation)
    {
        shared_setup();
        CHECK(lume_vfs_seek(&files[0], image_fd, 0, LUME_SEEK_SET) == 0);
        unsigned before = io_calls;
        short_count = 137;
        run_transfer(false);
        CHECK(result == 137 && io_calls == before + 1);
        short_count = 0;
        io_error = -LUME_EIO;
        run_transfer(false);
        CHECK(result == -LUME_EIO && io_calls == before + 2);
        io_error = 0;
        before = io_calls;
        unsigned allocations_before = allocation_live;
        allocation_budget = 0;
        run_transfer(false);
        CHECK(result == 8192 && io_calls == ++before && allocation_live == allocations_before);
        allocation_budget = -1;
        uint64_t saved = destination;
        destination = 4096;
        run_transfer(false);
        CHECK(result == -LUME_EFAULT && io_calls == before);
        destination = saved;
        shared_cleanup();
    }
}

static void release_and_bounds_cases(void)
{
    shared_setup();
    operation = 0;
    unsigned before = allocation_live;
    run_transfer(true);
    unsigned retained = allocation_live;
    CHECK(retained == before);
    lume_user_buffer_invalidate_root(0);
    CHECK(allocation_live == retained);
    finish_transfer();
    CHECK(result == -LUME_EFAULT && allocation_live == before);
    expect_destination(0xa7);
    shared_cleanup();

    uint64_t mapped = (uint64_t)vm(0, LUME_VM_MAP, 0, LUME_FILE_IO_MAX + 4096, 3,
                                   LUME_MAP_PRIVATE | LUME_MAP_ANONYMOUS, -1);
    CHECK(mapped >= LUME_VM_BASE);
    destination = mapped + 3;
    transfer_bytes = LUME_FILE_IO_MAX;
    CHECK(lume_user_buffer(destination, transfer_bytes, true));
    operation = 1;
    CHECK(lume_vfs_seek(&files[0], image_fd, 0, LUME_SEEK_SET) == 0);
    run_transfer(false);
    CHECK(result == LUME_FILE_IO_MAX);
    unsigned calls = io_calls;
    transfer_bytes = LUME_FILE_IO_MAX + 1;
    run_transfer(false);
    CHECK(result == -LUME_EINVAL && io_calls == calls);
    transfer_bytes = 0;
    before = allocation_live;
    run_transfer(false);
    CHECK(result == 0 && allocation_live == before);
    transfer_bytes = 8192;
    CHECK(vm(0, LUME_VM_UNMAP, mapped, LUME_FILE_IO_MAX + 4096, 0, 0, -1) == 0);
    while (retired)
        reap(NULL);
}

static void request_capture_cases(void)
{
    active_root = 2;
    uint64_t base =
        (uint64_t)vm(0, LUME_VM_MAP, 0, 8192, 3, LUME_MAP_PRIVATE | LUME_MAP_ANONYMOUS, -1);
    CHECK(base >= LUME_VM_BASE);
    uint64_t address = base + 4096 - sizeof(wire) / 2;
    wire = (struct lume_file_io_request){.version = LUME_FILE_IO_VERSION,
                                         .bytes = sizeof(wire),
                                         .fd = image_fd,
                                         .offset = 31,
                                         .whence = LUME_SEEK_SET};
    CHECK(lume_user_buffer(address, sizeof(wire), true));
    size_t done = 0;
    while (done < sizeof(wire))
    {
        uint64_t physical, next = address + done;
        CHECK(lume_mmu_translate(2, next, true, true, false, &physical));
        size_t part = 4096 - (size_t)(next % 4096);
        if (part > sizeof(wire) - done)
            part = sizeof(wire) - done;
        memcpy((void *)(uintptr_t)physical, (unsigned char *)&wire + done, part);
        done += part;
    }
    CHECK(lume_file_io_call(&files[0], 1, LUME_FILE_IO_SEEK, address, sizeof(wire)) == 31);
    CHECK(vm(0, LUME_VM_PROTECT, base + 4096, 4096, 0, 0, -1) == 0);
    CHECK(lume_file_io_call(&files[0], 1, LUME_FILE_IO_SEEK, address, sizeof(wire)) ==
          -LUME_EFAULT);
    CHECK(lume_vfs_seek(&files[0], image_fd, 0, LUME_SEEK_CUR) == 31);
    CHECK(vm(0, LUME_VM_PROTECT, base, 8192, 1, 0, -1) == 0);
    CHECK(lume_file_io_call(&files[0], 1, LUME_FILE_IO_SEEK, address, sizeof(wire)) == 31);
    CHECK(vm(0, LUME_VM_UNMAP, base, 8192, 0, 0, -1) == 0);
    while (retired)
        reap(NULL);
    CHECK(frame_live == baseline_frames);
}

static void identity_cases(void)
{
    unsigned char data[8192] = {0};
    struct lume_user_buffer_token token;
    shared_setup();
    CHECK(lume_user_buffer_prepare(&token, 1, destination, 8192, true));
    unsigned char offset_data[17];
    memset(offset_data, 0x51, sizeof(offset_data));
    CHECK(lume_user_buffer_copy_at(&token, 4093, offset_data, sizeof(offset_data), true));
    memset(offset_data, 0, sizeof(offset_data));
    CHECK(lume_user_buffer_copy_at(&token, 4093, offset_data, sizeof(offset_data), false));
    for (unsigned i = 0; i < sizeof(offset_data); ++i)
        CHECK(offset_data[i] == 0x51);
    CHECK(!lume_user_buffer_copy_at(&token, SIZE_MAX, offset_data, 1, true));
    CHECK(!lume_user_buffer_check(&token, 8192, 1, true));
    active_root = 4;
    CHECK(!lume_user_buffer_copy(&token, data, 8192, true));
    active_root = 2;
    CHECK(lume_user_buffer_copy(&token, data, 8192, true));
    struct vm_object *object = find_area(0, destination)->object;
    object->changing = true;
    CHECK(!lume_user_buffer_copy(&token, data, 8192, true));
    object->changing = false;
    CHECK(lume_user_buffer_copy(&token, data, 8192, true));
    lume_user_buffer_invalidate_root(0);
    CHECK(!lume_user_buffer_copy(&token, data, 8192, true));
    lume_user_buffer_release(&token);
    shared_cleanup();

    destination = (uint64_t)vm(0, LUME_VM_MAP, 0, 8192, 7,
                               LUME_MAP_PRIVATE | LUME_MAP_ANONYMOUS | LUME_MAP_JIT, -1);
    CHECK(destination >= LUME_VM_BASE);
    CHECK(!lume_user_buffer_prepare(&token, 1, destination, 8192, true));
    active_root = 3;
    CHECK(lume_user_buffer_prepare(&token, 1, destination, 8192, true));
    CHECK(lume_user_buffer_copy(&token, data, 8192, true));
    active_root = 2;
    CHECK(!lume_user_buffer_copy(&token, data, 8192, true));
    lume_user_buffer_release(&token);
    CHECK(vm(0, LUME_VM_UNMAP, destination, 8192, 0, 0, -1) == 0);
    while (retired)
        reap(NULL);

    shared_setup();
    struct lume_user_buffer_token bound[LUME_TASK_LIMIT];
    for (unsigned i = 0; i < LUME_TASK_LIMIT; ++i)
        CHECK(lume_user_buffer_prepare(bound + i, 1, destination, 1, true));
    CHECK(!lume_user_buffer_prepare(&token, 1, destination, 1, true));
    CHECK(lume_user_buffer_acquire(&token, 1, destination, 1, true) == -LUME_ENOMEM);
    for (unsigned i = 0; i < LUME_TASK_LIMIT; ++i)
        lume_user_buffer_release(bound + i);
    CHECK(lume_user_buffer_prepare(&token, 1, destination, 1, true));
    lume_user_buffer_release(&token);
    CHECK(lume_user_buffer_prepare(&token, 1, destination, 1, false));
    CHECK(!lume_user_buffer_copy_at(&token, 0, data, 1, true));
    CHECK(lume_user_buffer_copy_at(&token, 0, data, 1, false));
    lume_user_buffer_release(&token);
    shared_cleanup();
}

int main(int argc, char **argv)
{
#ifdef _WIN32
    CHECK(ConvertThreadToFiber(NULL));
#endif
    static const struct lume_storage_ops storage_ops = {
        .walk = walk, .read = read_file, .write = write_file};
    lume_vfs_init(NULL, 0);
    CHECK(lume_vfs_mount_storage(&storage_ops));
    baseline_metadata = allocation_live;
    for (unsigned i = 0; i < 3; ++i)
        lume_files_init(&files[i]);
    image_fd = (int)lume_vfs_open(&files[0], "/data/image", LUME_O_RDWR);
    CHECK(image_fd >= 3);
    for (unsigned i = 0; i < sizeof(image_bytes); ++i)
        image_bytes[i] = (unsigned char)(i * 13u + 7u);
    baseline_frames = frame_live;
    lume_worker_start();
    if (argc == 2 && !strcmp(argv[1], "truncate-pread"))
    {
        shared_setup();
        operation = 2;
        run_transfer(true);
        active_root = 4;
        CHECK(lume_worker_submit_on(LUME_WORK_GENERAL, truncate_operation, NULL));
        CHECK(lume_worker_resume_on(LUME_WORK_GENERAL));
        CHECK(!lume_worker_busy_on(LUME_WORK_GENERAL));
        finish_transfer();
        CHECK(result == -LUME_EFAULT && !unsafe_borrows);
        shared_cleanup();
        puts("PASS_TRUNCATE_PREAD_BUFFER_OWNERSHIP");
        return 0;
    }
    CHECK(argc == 1);
    for (operation = 0; operation < 4; ++operation)
    {
        shared_setup();
        uint64_t cursor = (uint64_t)lume_vfs_seek(&files[0], image_fd, 23, LUME_SEEK_SET);
        unsigned before = io_calls;
        run_transfer(false);
        CHECK(result == 8192 && io_calls == before + 1 && !unsafe_borrows);
        CHECK(lume_vfs_seek(&files[0], image_fd, 0, LUME_SEEK_CUR) ==
              (int64_t)(operation < 2 ? cursor + 8192 : cursor));
        if (operation & 1)
            for (unsigned i = 0; i < 8192; ++i)
                CHECK(received_bytes[i] == 0xa7);
        shared_cleanup();
    }
    for (operation = 0; operation < 4; ++operation)
    {
        shared_setup();
        CHECK(lume_vfs_seek(&files[0], image_fd, 23, LUME_SEEK_SET) == 23);
        unsigned before = io_calls, allocations_before = allocation_live;
        run_transfer(true);
        CHECK(allocation_live == allocations_before);
        active_root = 4;
        CHECK(lume_worker_submit_on(LUME_WORK_GENERAL, truncate_operation, NULL));
        CHECK(lume_worker_resume_on(LUME_WORK_GENERAL));
        CHECK(!lume_worker_busy_on(LUME_WORK_GENERAL));
        CHECK(frame_live == baseline_frames);
        finish_transfer();
        CHECK(result == (operation & 1 ? 8192 : -LUME_EFAULT));
        CHECK(io_calls == before + 1 && !unsafe_borrows);
        CHECK(lume_vfs_seek(&files[0], image_fd, 0, LUME_SEEK_CUR) == (operation < 2 ? 8215 : 23));
        shared_cleanup();
    }
    replacement_cases();
    result_cases();
    identity_cases();
    request_capture_cases();
    release_and_bounds_cases();
    for (unsigned i = 0; i < 3; ++i)
        lume_files_release(&files[i]);
    while (retired)
        reap(NULL);
    CHECK(allocation_live == baseline_metadata && frame_live == baseline_frames);
    for (unsigned i = 0; i < LUME_WORKER_LIMIT; ++i)
        release_fiber(&workers[i].fiber);
    for (unsigned i = 0; i < LUME_WORKER_LIMIT - 1; ++i)
        free(extra_stacks[i]);
    printf("PASS_USER_IO_LIFETIME checks=%u io_calls=%u pauses=%u unsafe_borrows=%u\n", checks,
           io_calls, pauses, unsafe_borrows);
    return 0;
}
