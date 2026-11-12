#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <malloc.h>
#endif
#undef ENOTSUP
#ifndef LUME_VM_TEST_SOURCE
#define LUME_VM_TEST_SOURCE "../../src/mm/vm.c"
#endif
#include LUME_VM_TEST_SOURCE

static unsigned metadata_live, physical_live, mappings_live, syncs, checks;
static unsigned image_protect_calls;
static int metadata_budget = -1, physical_budget = -1;
static int mapping_budget = -1;
static void (*allocation_pause)(void);
static bool writer_view[LUME_TASK_LIMIT];
static uint64_t root_offset[LUME_TASK_LIMIT];

static struct
{
    unsigned task;
    uint64_t address, physical;
    bool write, execute, live;
} mappings[4096];

static uint64_t frames[4096];
static unsigned frame_span[4096], span_calls, file_holds;
static bool span_unavailable;

static struct
{
    void *data;
    unsigned references;
} frame_spans[4096];

static struct
{
    const struct lume_file_ops *ops;
    void *context;
    unsigned mode;
} descriptors[64];

#define CHECK(test)                                                                                \
    do                                                                                             \
    {                                                                                              \
        ++checks;                                                                                  \
        if (!(test))                                                                               \
        {                                                                                          \
            fprintf(stderr, "FAIL line %u: %s\n", __LINE__, #test);                                \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

int64_t lume_task_protect_image(unsigned task, uint64_t first, uint64_t length, unsigned protection)
{
    (void)task;
    (void)first;
    (void)length;
    (void)protection;
    ++image_protect_calls;
    return -LUME_ENOMEM;
}

void *lume_kernel_allocate(size_t size)
{
    if (!metadata_budget)
        return NULL;
    if (metadata_budget > 0)
        --metadata_budget;
    void *value = malloc(size);
    if (value)
    {
        ++metadata_live;
        if (size == sizeof(struct vm_chunk) && allocation_pause)
        {
            void (*pause)(void) = allocation_pause;
            allocation_pause = NULL;
            pause();
        }
    }
    return value;
}

uint64_t lume_kernel_allocation_pages(const void *p)
{
    return p ? 1 : 0;
}

uint64_t lume_pages_total(void)
{
    return 65536;
}

uint64_t lume_page_alloc_uninitialized(uint64_t minimum)
{
    return lume_page_alloc(minimum);
}

uint64_t lume_pages_alloc_span_uninitialized(unsigned n)
{
    return lume_pages_alloc_span(n);
}

static bool storage_context = true;

bool lume_task_storage_context(void)
{
    return storage_context;
}

void lume_task_vm_ready(void)
{
}

void lume_task_vm_buffer_wait(void)
{
}

unsigned lume_worker_current(void)
{
    return LUME_WORK_STORAGE;
}

bool lume_work_cancel(struct lume_work *w)
{
    bool queued = w->queued;
    w->queued = false;
    return queued;
}

static void (*mapping_notify)(uint64_t, uint64_t, uint64_t);

void lume_vfs_mapping_notify(void (*p)(uint64_t, uint64_t, uint64_t))
{
    mapping_notify = p;
}

int lume_vfs_object_retain(int fd)
{
    (void)fd;
    ++file_holds;
    return 0;
}

void lume_kernel_free(void *value)
{
    if (value)
    {
        CHECK(metadata_live);
        --metadata_live;
        free(value);
    }
}

uint64_t lume_page_alloc(unsigned unused)
{
    (void)unused;
    if (!physical_budget)
        return 0;
    if (physical_budget > 0)
        --physical_budget;
#if defined(_WIN32)
    void *value = _aligned_malloc(4096, 4096);
#else
    void *value = aligned_alloc(4096, 4096);
#endif
    CHECK(value);
    memset(value, 0, 4096);
    for (unsigned i = 0; i < 4096; ++i)
        if (!frames[i])
        {
            frames[i] = (uintptr_t)value;
            ++physical_live;
            return frames[i];
        }
    abort();
}

uint64_t lume_pages_alloc_span(unsigned count)
{
    ++span_calls;
    if (!count || span_unavailable || (physical_budget >= 0 && (unsigned)physical_budget < count))
        return 0;
    unsigned slots[16], found = 0, span = 0;
    CHECK(count <= 16);
    for (unsigned i = 0; i < 4096 && found < count; ++i)
        if (!frames[i])
            slots[found++] = i;
    if (found != count)
        return 0;
    while (span < 4096 && frame_spans[span].references)
        ++span;
    CHECK(span < 4096);
#if defined(_WIN32)
    void *data = _aligned_malloc((size_t)count * 4096, 4096);
#else
    void *data = aligned_alloc(4096, (size_t)count * 4096);
#endif
    CHECK(data);
    memset(data, 0, (size_t)count * 4096);
    frame_spans[span].data = data;
    frame_spans[span].references = count;
    for (unsigned i = 0; i < count; ++i)
    {
        frames[slots[i]] = (uintptr_t)data + (uint64_t)i * 4096;
        frame_span[slots[i]] = span + 1;
    }
    physical_live += count;
    if (physical_budget > 0)
        physical_budget -= (int)count;
    return (uintptr_t)data;
}

bool lume_page_free(uint64_t physical)
{
    for (unsigned i = 0; i < 4096; ++i)
        if (mappings[i].live)
            CHECK(mappings[i].physical != physical);
    for (unsigned i = 0; i < 4096; ++i)
        if (frames[i] == physical)
        {
            void *allocation = (void *)(uintptr_t)physical;
            if (frame_span[i])
            {
                unsigned span = frame_span[i] - 1;
                CHECK(frame_spans[span].references);
                allocation = --frame_spans[span].references ? NULL : frame_spans[span].data;
                if (!frame_spans[span].references)
                    frame_spans[span].data = NULL;
                frame_span[i] = 0;
            }
#if defined(_WIN32)
            _aligned_free(allocation);
#else
            free(allocation);
#endif
            frames[i] = 0;
            --physical_live;
            return true;
        }
    return false;
}

void lume_fail(const char *message)
{
    fprintf(stderr, "PRODUCT FAIL %s\n", message);
    abort();
}

uint64_t lume_mmu_task_root(unsigned task)
{
    return task + 1 + root_offset[task];
}

bool lume_mmu_translate(uint64_t root, uint64_t address, bool write, bool user, bool execute,
                        uint64_t *physical)
{
    (void)user;
    for (unsigned i = 0; i < 4096; ++i)
        if (mappings[i].live && mappings[i].task + 1 == root &&
            mappings[i].address == (address & ~UINT64_C(4095)))
        {
            if ((write && !mappings[i].write) || (execute && !mappings[i].execute))
                return false;
            *physical = mappings[i].physical + address % 4096;
            return true;
        }
    return false;
}

bool lume_mmu_jit_active(unsigned task)
{
    return writer_view[task];
}

uint64_t lume_mmu_task_view_root(unsigned task, bool writer)
{
    return lume_mmu_task_root(task) + (writer ? 2 * LUME_TASK_LIMIT : 0);
}

bool lume_mmu_root_matches(unsigned task, uint64_t root)
{
    return root == lume_mmu_task_view_root(task, false) ||
           root == lume_mmu_task_view_root(task, true);
}

bool lume_mmu_map_user_jit(unsigned task, uint64_t address, uint64_t physical, bool write,
                           bool execute)
{
    (void)task;
    (void)address;
    (void)physical;
    (void)write;
    (void)execute;
    return false;
}

bool lume_mmu_map_user(unsigned task, uint64_t address, uint64_t physical, bool write, bool execute)
{
    CHECK(!(write && execute));
    if (!mapping_budget)
        return false;
    if (mapping_budget > 0)
        --mapping_budget;
    for (unsigned i = 0; i < 4096; ++i)
        if (mappings[i].live && mappings[i].task == task && mappings[i].address == address)
        {
            mappings[i].physical = physical;
            mappings[i].write = write;
            mappings[i].execute = execute;
            return true;
        }
    for (unsigned i = 0; i < 4096; ++i)
        if (!mappings[i].live)
        {
            mappings[i].task = task;
            mappings[i].address = address;
            mappings[i].physical = physical;
            mappings[i].write = write;
            mappings[i].execute = execute;
            mappings[i].live = true;
            ++mappings_live;
            return true;
        }
    return false;
}

bool lume_mmu_unmap_user_batch(unsigned task, const uint64_t *addresses, unsigned count)
{
    for (unsigned n = 0; n < count; ++n)
        for (unsigned i = 0; i < 4096; ++i)
            if (mappings[i].live && mappings[i].task == task && mappings[i].address == addresses[n])
            {
                mappings[i].live = false;
                --mappings_live;
            }
    return true;
}

bool lume_user_buffer(uint64_t address, uint64_t bytes, bool write)
{
    while (bytes)
    {
        if (lume_vm_fault(0, address, write, false) != 1)
            return false;
        uint64_t part = 4096 - address % 4096;
        if (part > bytes)
            part = bytes;
        address += part;
        bytes -= part;
    }
    return true;
}

void lume_sync_code(uint64_t physical, size_t bytes)
{
    (void)physical;
    (void)bytes;
    ++syncs;
}

void lume_sync_code_defer(uint64_t physical, size_t bytes)
{
    (void)physical;
    (void)bytes;
    ++syncs;
}

void lume_sync_code_clean_finish(void)
{
}

void lume_sync_code_finish(void)
{
}

void lume_worker_checkpoint(unsigned bytes)
{
    (void)bytes;
}

void lume_worker_compute(void (*run)(void *), void *context)
{
    run(context);
}

bool lume_worker_pause_ms(unsigned ms)
{
    (void)ms;
    return true;
}

int lume_work_submit(struct lume_work *work)
{
    (void)work;
    return 0;
}

int lume_vfs_attach(struct lume_files *files, const struct lume_file_ops *ops, void *context,
                    unsigned flags)
{
    (void)files;
    (void)flags;
    for (unsigned fd = 0; fd < 64; ++fd)
        if (!descriptors[fd].ops)
        {
            descriptors[fd].ops = ops;
            descriptors[fd].context = context;
            descriptors[fd].mode = LUME_O_RDWR;
            return fd;
        }
    return -LUME_EMFILE;
}

void *lume_vfs_external(struct lume_files *files, unsigned fd, const struct lume_file_ops *ops)
{
    (void)files;
    return fd < 64 && descriptors[fd].ops == ops ? descriptors[fd].context : NULL;
}

const struct lume_vm_dma_pin *lume_vfs_memory(struct lume_files *files, unsigned fd, unsigned *mode)
{
    (void)files;
    if (fd >= 64 || !descriptors[fd].ops || !descriptors[fd].ops->memory || !mode)
        return NULL;
    *mode = descriptors[fd].mode;
    return descriptors[fd].ops->memory(descriptors[fd].context);
}

void lume_vfs_access_mode(struct lume_files *files, unsigned fd, unsigned mode)
{
    (void)files;
    descriptors[fd].mode = mode;
}

int lume_vfs_hold(struct lume_files *files, unsigned fd)
{
    (void)files;
    if (fd >= 64 || !descriptors[fd].ops)
        return -LUME_EBADF;
    ++file_holds;
    return (int)fd;
}

int64_t lume_vfs_stat(struct lume_files *files, unsigned fd, struct lume_stat *stat)
{
    (void)files;
    return fd < 64 && descriptors[fd].ops ? descriptors[fd].ops->stat(descriptors[fd].context, stat)
                                          : -LUME_EBADF;
}

int64_t lume_vfs_pread_object(int fd, uint64_t offset, void *data, uint64_t bytes)
{
    return descriptors[fd].ops->pread(descriptors[fd].context, data, bytes, offset);
}

void lume_vfs_drop(int fd)
{
    (void)fd;
    CHECK(file_holds);
    --file_holds;
}

int64_t lume_vfs_close(struct lume_files *files, unsigned fd)
{
    (void)files;
    CHECK(fd < 64 && descriptors[fd].ops);
    descriptors[fd].ops->close(descriptors[fd].context);
    descriptors[fd].ops = NULL;
    descriptors[fd].context = NULL;
    return 0;
}

static struct lume_files files;

static int64_t call(unsigned task, unsigned operation, uint64_t address, uint64_t bytes,
                    unsigned protection, unsigned flags, int fd)
{
    struct lume_vm_request r = {.version = LUME_VM_VERSION,
                                .address = address,
                                .length = bytes,
                                .protection = protection,
                                .flags = flags,
                                .fd = fd};
    return lume_vm_call(&files, task + 1, operation, &r);
}

static uint64_t physical(unsigned task, uint64_t address, bool write, bool execute)
{
    uint64_t value;
    int result = lume_vm_fault(task, address, write, execute);
    if (result != 1)
        fprintf(stderr, "fault task=%u address=%llx write=%u execute=%u result=%d checks=%u\n",
                task, (unsigned long long)address, write, execute, result, checks);
    CHECK(result == 1);
    CHECK(lume_mmu_translate(task + 1, address, write, true, execute, &value));
    return value;
}

static void drain(void)
{
    unsigned count = 0;
    while (retired)
    {
        CHECK(++count < 4096);
        reap(NULL);
    }
}

static void clean(void)
{
    metadata_budget = physical_budget = -1;
    mapping_budget = -1;
    allocation_pause = NULL;
    span_unavailable = false;
    memset(writer_view, 0, sizeof(writer_view));
    memset(root_offset, 0, sizeof(root_offset));
    for (unsigned i = 0; i < 64; ++i)
        lume_vm_release(i);
    drain();
    lume_vm_cache_reclaim();
    for (unsigned fd = 0; fd < 64; ++fd)
        if (descriptors[fd].ops)
            lume_vfs_close(&files, fd);
    drain();
    CHECK(!metadata_live && !physical_live && !mappings_live && !retired_threads && !file_holds);
}

static void dma_pin_cases(void);
static void write_cases(void);
static void fault_suspend_cases(void);
static void thread_region_cases(void);
static void file_mapping_cases(void);
static void shared_word_cases(void);
static int memory_file(void);

static void buffer_transaction_cases(void)
{
    int fd = memory_file();
    CHECK(!call(0, LUME_VM_RESIZE, 0, 2 * PAGE, 0, 0, fd));
    uint64_t base = call(0, LUME_VM_MAP, 0, 2 * PAGE, 3, LUME_MAP_SHARED, fd);
    uint64_t other = call(1, LUME_VM_MAP, 0, 2 * PAGE, 3, LUME_MAP_SHARED, fd);
    CHECK(base == LUME_VM_BASE && other == LUME_VM_BASE);
    CHECK(!lume_vm_buffer_busy(0, base, 128));
    CHECK(lume_vm_buffer_ready(0, base, 128));
    changing[0] = true;
    CHECK(lume_vm_buffer_busy(0, UINT64_C(0x123000), 128));
    CHECK(!lume_vm_buffer_ready(0, UINT64_C(0x123000), 128));
    CHECK(!lume_vm_buffer_busy(1, other, 128));
    CHECK(!lume_vm_buffer_busy(0, base, 0));
    CHECK(!lume_vm_buffer_busy(0, UINT64_MAX - 63, 128));
    CHECK(!lume_vm_buffer_busy(LUME_TASK_LIMIT, base, 128));
    changing[0] = false;
    struct vm_object *object = find_area(0, base)->object;
    object->changing = true;
    CHECK(lume_vm_buffer_busy(0, base + PAGE - 64, 128));
    CHECK(lume_vm_buffer_busy(1, other, 128));
    CHECK(!lume_vm_buffer_ready(1, other, 128));
    CHECK(!lume_vm_buffer_busy(0, base - 128, 128));
    CHECK(!lume_vm_buffer_busy(0, base + 2 * PAGE, 128));
    CHECK(!lume_vm_buffer_busy(0, UINT64_C(0x123000), 128));
    object->changing = false;
    CHECK(!lume_vm_buffer_busy(0, base, 128));
    CHECK(lume_vm_buffer_ready(0, base, 128));
    CHECK(!lume_vm_buffer_busy(0, base + 2 * PAGE, 128));
    CHECK(!lume_vm_buffer_ready(0, base + 2 * PAGE, 128));
    clean();
}

int main(void)
{
    dma_pin_cases();
    buffer_transaction_cases();
    const unsigned anonymous = LUME_MAP_PRIVATE | LUME_MAP_ANONYMOUS;
    const uint64_t span = UINT64_C(4) * 1024 * 1024 * 1024;
    CHECK(call(0, LUME_VM_PROTECT, LUME_VM_BASE - 4096, 4096, LUME_PROT_READ, 0, -1) ==
          -LUME_ENOMEM);
    CHECK(image_protect_calls == 1);
    uint64_t base = call(0, LUME_VM_MAP, 0, span, 0, anonymous, -1);
    CHECK(base == LUME_VM_BASE && !physical_live && !mappings_live && metadata_live == 2);
    CHECK(call(0, LUME_VM_PROTECT, base + 4096, 12288, 3, 0, -1) == 0);
    CHECK(lume_vm_fault(0, base, true, false) == 0);
    uint64_t a = physical(0, base + 4096, true, false), b = physical(0, base + 8192, true, false);
    for (unsigned i = 0; i < 4096; ++i)
        CHECK(!((unsigned char *)(uintptr_t)a)[i]);
    *(unsigned char *)(uintptr_t)a = 0x7b;
    CHECK(call(0, LUME_VM_PROTECT, base + 4096, 4096, 1, 0, -1) == 0);
    CHECK(lume_vm_fault(0, base + 4096, true, false) == 0);
    CHECK(physical(0, base + 4096, false, false) == a && *(unsigned char *)(uintptr_t)a == 0x7b);
    CHECK(call(0, LUME_VM_UNMAP, base + 4096, 4096, 0, 0, -1) == 0);
    CHECK(lume_vm_fault(0, base + 4096, false, false) == 0);
    CHECK(physical(0, base + 8192, true, false) == b && physical_live == 1);
    clean();

    base = call(0, LUME_VM_MAP, 0, 32768, 3, anonymous, -1);
    a = physical(0, base, true, false);
    *(unsigned char *)(uintptr_t)a = 0x4d;
    b = physical(0, base + 28672, true, false);
    unsigned before = metadata_live;
    for (int budget = 0; budget < 2; ++budget)
    {
        metadata_budget = budget;
        CHECK(call(0, LUME_VM_PROTECT, base + 4096, 24576, 1, 0, -1) == -LUME_ENOMEM);
        metadata_budget = -1;
        CHECK(metadata_live == before && spaces[0]->first == base &&
              spaces[0]->end == base + 32768 && !spaces[0]->next);
        CHECK(physical(0, base, true, false) == a && physical(0, base + 28672, true, false) == b);
    }
    CHECK(call(0, LUME_VM_MAP, base, 4096, 3, anonymous | LUME_MAP_FIXED_NOREPLACE, -1) ==
          -LUME_EEXIST);
    metadata_budget = 2;
    CHECK(call(0, LUME_VM_MAP, base + 4096, 8192, 3, anonymous | LUME_MAP_FIXED, -1) ==
          -LUME_ENOMEM);
    metadata_budget = -1;
    drain();
    CHECK(metadata_live == before && !spaces[0]->next && *(unsigned char *)(uintptr_t)a == 0x4d);
    CHECK(call(0, LUME_VM_PROTECT, base, 4096, 7, 0, -1) == -LUME_EACCES);
    CHECK(call(0, LUME_VM_MAP, 0, 4096, 7, anonymous, -1) == -LUME_EINVAL);
    CHECK(call(0, LUME_VM_PROTECT, base, 4096, 5, 0, -1) == 0);
    CHECK(lume_vm_fault(0, base, true, false) == 0);
    unsigned synced = syncs;
    CHECK(physical(0, base, false, true) == a && syncs > synced);
    clean();

    base = call(0, LUME_VM_MAP, 0, span, 3, anonymous, -1);
    a = physical(0, base, true, false);
    *(unsigned char *)(uintptr_t)a = 0xa6;
    CHECK(lume_vm_clone(0, 1));
    b = physical(1, base, true, false);
    CHECK(b != a && *(unsigned char *)(uintptr_t)b == 0xa6 && physical_live == 2);
    *(unsigned char *)(uintptr_t)b = 0x12;
    CHECK(*(unsigned char *)(uintptr_t)a == 0xa6);
    clean();

    base = call(0, LUME_VM_MAP, 0, 4096, 3, anonymous, -1);
    physical_budget = 0;
    CHECK(lume_vm_fault(0, base, true, false) == -1 && !physical_live && !mappings_live);
    physical_budget = -1;
    CHECK(physical(0, base, true, false));
    clean();

    int fd = (int)call(0, LUME_VM_CREATE, 0, 0, 0, LUME_MEMFD_ALLOW_SEALING, -1);
    CHECK(fd >= 0);
    CHECK(call(0, LUME_VM_RESIZE, 0, 12288, 0, 0, fd) == 0);
    base = call(0, LUME_VM_MAP, 0, 12288, 3, LUME_MAP_SHARED, fd);
    uint64_t other = call(1, LUME_VM_MAP, 0, 12288, 1, LUME_MAP_SHARED, fd);
    a = physical(0, base, true, false);
    *(unsigned char *)(uintptr_t)a = 0x39;
    b = physical(1, other, false, false);
    CHECK(a == b && *(unsigned char *)(uintptr_t)b == 0x39);
    CHECK(call(1, LUME_VM_MAP, 0, 12288, 5, LUME_MAP_SHARED, fd) == -LUME_EACCES);
    CHECK(call(0, LUME_VM_PROTECT, base, 12288, 1, 0, -1) == 0);
    uint64_t executable = call(1, LUME_VM_MAP, 0, 12288, 5, LUME_MAP_SHARED, fd);
    CHECK((int64_t)executable > 0);
    CHECK(call(0, LUME_VM_PROTECT, base, 12288, 3, 0, -1) == -LUME_EACCES);
    CHECK(!lume_vfs_close(&files, fd));
    CHECK(physical(1, other, false, false) == a);
    CHECK(call(1, LUME_VM_UNMAP, executable, 12288, 0, 0, -1) == 0);
    CHECK(call(0, LUME_VM_PROTECT, base, 12288, 3, 0, -1) == 0);
    CHECK(physical(0, base, true, false) == a);
    clean();
    write_cases();
    {
        int fd = memory_file();
        CHECK(!call(0, LUME_VM_RESIZE, 0, PAGE, 0, 0, fd));
        uint64_t writable = call(0, LUME_VM_MAP, 0, PAGE, 3, LUME_MAP_SHARED, fd);
        uint64_t executable = call(1, LUME_VM_MAP, 0, PAGE, 5, LUME_MAP_PRIVATE, fd);
        uint64_t mutable = physical(0, writable, true, false);
        *(unsigned char *)(uintptr_t)mutable = 0x71;
        uint64_t immutable = physical(1, executable, false, true);
        CHECK(mutable != immutable && *(unsigned char *)(uintptr_t)immutable == 0x71);
        *(unsigned char *)(uintptr_t)mutable = 0x92;
        CHECK(*(unsigned char *)(uintptr_t)immutable == 0x71);
        clean();
    }
    fault_suspend_cases();
    thread_region_cases();
    file_mapping_cases();
    shared_word_cases();
    printf("PASS_VM_PRODUCT_HOST checks=%u metadata=%u frames=%u ptes=%u\n", checks, metadata_live,
           physical_live, mappings_live);
    return 0;
}

static int memory_file(void)
{
    int fd = (int)call(0, LUME_VM_CREATE, 0, 0, 0, LUME_MEMFD_ALLOW_SEALING, -1);
    CHECK(fd >= 0);
    return fd;
}

static void shared_word_cases(void)
{
    int fd = memory_file();
    CHECK(!call(0, LUME_VM_RESIZE, 0, PAGE, 0, 0, fd));
    uint64_t first = call(0, LUME_VM_MAP, 0, PAGE, 3, LUME_MAP_SHARED, fd);
    uint64_t second = call(1, LUME_VM_MAP, LUME_VM_BASE + 8 * PAGE, PAGE, 3,
                           LUME_MAP_SHARED | LUME_MAP_FIXED, fd);
    CHECK((int64_t)first > 0 && (int64_t)second > 0 && first != second);
    struct lume_vm_word_key a, b, c;
    CHECK(!lume_vm_shared_word_key(0, first + 64, &a) && a.object && a.offset == 64);
    CHECK(!lume_vm_shared_word_key(1, second + 64, &b));
    CHECK(a.object == b.object && a.offset == b.offset);
    CHECK(!lume_vm_shared_word_key(1, second + 68, &c));
    CHECK(a.object == c.object && a.offset != c.offset);
    CHECK(lume_vm_shared_word_key(0, first + 1, &c) == -LUME_EINVAL);
    CHECK(lume_vm_shared_word_key(0, first + PAGE, &c) == -LUME_EFAULT);
    changing[1] = true;
    CHECK(lume_vm_shared_word_key(1, second + 64, &c) == -LUME_EAGAIN);
    changing[1] = false;
    uint64_t private = call(1, LUME_VM_MAP, 0, PAGE, 3, LUME_MAP_PRIVATE, fd);
    CHECK((int64_t)private > 0);
    CHECK(lume_vm_shared_word_key(1, private, &c) == -LUME_EOPNOTSUPP);
    CHECK(!lume_vfs_close(&files, fd));
    CHECK(!lume_vm_shared_word_key(1, second + 64, &b) && b.object == a.object);
    clean();
    fd = memory_file();
    CHECK(!call(0, LUME_VM_RESIZE, 0, PAGE, 0, 0, fd));
    first = call(0, LUME_VM_MAP, first, PAGE, 3, LUME_MAP_SHARED | LUME_MAP_FIXED, fd);
    CHECK((int64_t)first > 0);
    CHECK(!lume_vm_shared_word_key(0, first + 64, &b) && b.object != a.object);
    clean();
    uint64_t previous_serial = serial;
    serial = UINT64_MAX - UINT64_C(0x40000000) - 1;
    fd = memory_file();
    CHECK(!call(0, LUME_VM_RESIZE, 0, PAGE, 0, 0, fd));
    first = call(0, LUME_VM_MAP, 0, PAGE, 3, LUME_MAP_SHARED, fd);
    CHECK((int64_t)first > 0);
    CHECK(!lume_vm_shared_word_key(0, first, &b) && b.object == UINT64_MAX);
    CHECK(call(0, LUME_VM_CREATE, 0, 0, 0, LUME_MEMFD_ALLOW_SEALING, -1) == -LUME_ENOMEM);
    clean();
    serial = previous_serial;
}

static uint64_t file_size(int fd)
{
    struct lume_stat stat;
    CHECK(!descriptors[fd].ops->stat(descriptors[fd].context, &stat));
    return stat.size;
}

static uint64_t file_offset(int fd)
{
    int64_t offset = descriptors[fd].ops->seek(descriptors[fd].context, 0, 1);
    CHECK(offset >= 0);
    return (uint64_t)offset;
}

static void write_cases(void)
{
    unsigned char data[8192], prefix[8192], output[8192];
    memset(data, 0x63, sizeof(data));
    memset(prefix, 0x11, sizeof(prefix));
    int fd = memory_file();
    physical_budget = 1;
    int64_t done = descriptors[fd].ops->write(descriptors[fd].context, data, sizeof(data));
    printf("VM_SHORT_WRITE returned=%lld size=%llu\n", (long long)done,
           (unsigned long long)file_size(fd));
    CHECK(done == 4096);
    CHECK(file_size(fd) == 4096 && file_offset(fd) == 4096);
    CHECK(descriptors[fd].ops->pread(descriptors[fd].context, output, sizeof(output), 0) == 4096);
    CHECK(!memcmp(output, data, 4096));
    clean();

    fd = memory_file();
    physical_budget = 0;
    CHECK(descriptors[fd].ops->write(descriptors[fd].context, data, 1) == -LUME_ENOMEM);
    CHECK(!file_size(fd) && !file_offset(fd) && !physical_live);
    clean();

    fd = memory_file();
    unsigned allocated = metadata_live;
    metadata_budget = 0;
    CHECK(descriptors[fd].ops->write(descriptors[fd].context, data, 1) == -LUME_ENOMEM);
    CHECK(!file_size(fd) && !file_offset(fd) && !physical_live && metadata_live == allocated);
    clean();

    fd = memory_file();
    CHECK(descriptors[fd].ops->seek(descriptors[fd].context, 8192, 0) == 8192);
    allocated = metadata_live;
    metadata_budget = physical_budget = 0;
    CHECK(!descriptors[fd].ops->write(descriptors[fd].context, NULL, 0));
    CHECK(!file_size(fd) && file_offset(fd) == 8192 && !physical_live &&
          metadata_live == allocated);
    clean();

    fd = memory_file();
    CHECK(descriptors[fd].ops->write(descriptors[fd].context, prefix, 100) == 100);
    uint64_t base = call(0, LUME_VM_MAP, 0, 8192, 3, LUME_MAP_SHARED, fd);
    uint64_t page = physical(0, base, true, false);
    memset((unsigned char *)(uintptr_t)page + 100, 0xdd, 4096 - 100);
    CHECK(descriptors[fd].ops->seek(descriptors[fd].context, 5000, 0) == 5000);
    CHECK(descriptors[fd].ops->write(descriptors[fd].context, data, 10) == 10);
    CHECK(file_size(fd) == 5010 && file_offset(fd) == 5010);
    CHECK(descriptors[fd].ops->pread(descriptors[fd].context, output, sizeof(output), 0) == 5010);
    CHECK(!memcmp(output, prefix, 100) && !memcmp(output + 5000, data, 10));
    for (unsigned i = 100; i < 5000; ++i)
        CHECK(!output[i]);
    clean();

    fd = memory_file();
    CHECK(!call(0, LUME_VM_ADD_SEALS, 0, 0, 0, LUME_SEAL_SHRINK, fd));
    CHECK(descriptors[fd].ops->seek(descriptors[fd].context, 4096, 0) == 4096);
    physical_budget = 1;
    CHECK(descriptors[fd].ops->write(descriptors[fd].context, data, sizeof(data)) == 4096);
    CHECK(file_size(fd) == 8192 && file_offset(fd) == 8192);
    CHECK(descriptors[fd].ops->pread(descriptors[fd].context, output, sizeof(output), 0) == 8192);
    for (unsigned i = 0; i < 4096; ++i)
        CHECK(!output[i]);
    CHECK(!memcmp(output + 4096, data, 4096));
    clean();

    fd = memory_file();
    CHECK(descriptors[fd].ops->write(descriptors[fd].context, prefix, sizeof(prefix)) ==
          sizeof(prefix));
    CHECK(!call(0, LUME_VM_ADD_SEALS, 0, 0, 0, LUME_SEAL_GROW, fd));
    CHECK(descriptors[fd].ops->seek(descriptors[fd].context, 6144, 0) == 6144);
    CHECK(descriptors[fd].ops->write(descriptors[fd].context, data, 4096) == -LUME_EPERM);
    CHECK(file_size(fd) == 8192 && file_offset(fd) == 6144);
    CHECK(descriptors[fd].ops->pread(descriptors[fd].context, output, sizeof(output), 0) == 8192);
    CHECK(!memcmp(output, prefix, sizeof(prefix)));
    CHECK(descriptors[fd].ops->seek(descriptors[fd].context, 4096, 0) == 4096);
    CHECK(descriptors[fd].ops->write(descriptors[fd].context, data, 4096) == 4096);
    CHECK(file_size(fd) == 8192 && file_offset(fd) == 8192);
    CHECK(descriptors[fd].ops->pread(descriptors[fd].context, output, sizeof(output), 0) == 8192);
    CHECK(!memcmp(output, prefix, 4096) && !memcmp(output + 4096, data, 4096));
    clean();

    fd = memory_file();
    CHECK(!call(0, LUME_VM_ADD_SEALS, 0, 0, 0, LUME_SEAL_WRITE, fd));
    CHECK(descriptors[fd].ops->write(descriptors[fd].context, data, 1) == -LUME_EPERM);
    CHECK(!file_size(fd) && !file_offset(fd));
    clean();

    fd = memory_file();
    CHECK(descriptors[fd].ops->seek(descriptors[fd].context, 17, 0) == 17);
    physical_budget = 1;
    CHECK(descriptors[fd].ops->pwrite(descriptors[fd].context, data, sizeof(data), 4096) == 4096);
    CHECK(file_size(fd) == 8192 && file_offset(fd) == 17);
    clean();

    fd = memory_file();
    CHECK(descriptors[fd].ops->write(descriptors[fd].context, prefix, 5000) == 5000);
    CHECK(descriptors[fd].ops->seek(descriptors[fd].context, 4500, 0) == 4500);
    physical_budget = 0;
    CHECK(descriptors[fd].ops->write(descriptors[fd].context, data, sizeof(data)) == 3692);
    CHECK(file_size(fd) == 8192 && file_offset(fd) == 8192);
    CHECK(descriptors[fd].ops->pread(descriptors[fd].context, output, sizeof(output), 0) == 8192);
    CHECK(!memcmp(output, prefix, 4500) && !memcmp(output + 4500, data, 3692));
    clean();

    fd = memory_file();
    const uint64_t boundary = 512 * UINT64_C(4096);
    CHECK(descriptors[fd].ops->seek(descriptors[fd].context, boundary - 4096, 0) ==
          boundary - 4096);
    CHECK(descriptors[fd].ops->write(descriptors[fd].context, prefix, 4096) == 4096);
    CHECK(descriptors[fd].ops->seek(descriptors[fd].context, boundary - 4096, 0) ==
          boundary - 4096);
    allocated = metadata_live;
    metadata_budget = 0;
    CHECK(descriptors[fd].ops->write(descriptors[fd].context, data, sizeof(data)) == 4096);
    CHECK(file_size(fd) == boundary && file_offset(fd) == boundary && metadata_live == allocated);
    CHECK(descriptors[fd].ops->pread(descriptors[fd].context, output, 4096, boundary - 4096) ==
          4096);
    CHECK(!memcmp(output, data, 4096));
    clean();
}

static int pause_fd;
static uint64_t pause_base, pause_bytes, pause_other;
static struct vm_object *pause_object;
static int64_t pause_result;

static void resize_during_allocation(void)
{
    pause_result = call(1, LUME_VM_RESIZE, 0, pause_bytes, 0, 0, pause_fd);
    CHECK(!pause_result);
}

static void release_during_allocation(void)
{
    lume_vm_release(0);
    CHECK(!lume_vfs_close(&files, (unsigned)pause_fd));
    drain();
    CHECK(pause_object->references == 1);
}

static void remap_during_allocation(void)
{
    CHECK(!call(0, LUME_VM_UNMAP, pause_base, 8192, 0, 0, -1));
    CHECK(call(0, LUME_VM_MAP, pause_base, 8192, 3, LUME_MAP_SHARED | LUME_MAP_FIXED, pause_fd) ==
          (int64_t)pause_base);
}

static void protect_during_allocation(void)
{
    CHECK(!call(0, LUME_VM_PROTECT, pause_base, 8192, 0, 0, -1));
    CHECK(!call(0, LUME_VM_PROTECT, pause_base, 8192, 3, 0, -1));
}

static void switch_view_during_allocation(void)
{
    writer_view[0] = true;
}

static void switch_root_during_allocation(void)
{
    root_offset[0] = 4 * LUME_TASK_LIMIT;
}

static void change_object_during_allocation(void)
{
    pause_object->changing = true;
}

static void alias_fault_during_allocation(void)
{
    CHECK(lume_vm_fault(1, pause_other, false, false) == 1);
}

static void alias_retry_during_write(void)
{
    CHECK(lume_vm_fault(1, pause_other, false, false) == 2);
    struct lume_vm_dma_pin *pin = NULL;
    struct lume_vm_dma_span span;
    CHECK(lume_vm_dma_pin(&files, (unsigned)pause_fd, 0, PAGE, true, &pin, &span) == -LUME_EBUSY);
    CHECK(call(0, LUME_VM_RESIZE, 0, PAGE, 0, 0, pause_fd) == -LUME_EBUSY);
}

static void private_write_during_allocation(void)
{
    uint64_t page = physical(0, pause_base, true, false);
    *(unsigned char *)(uintptr_t)page = 0xa5;
}

static void fault_suspend_cases(void)
{
    const uint64_t sizes[] = {0, 4096, 4097, 16384};
    const uint64_t offsets[] = {0, 4096, 4096, 0};
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(*sizes); ++i)
    {
        pause_fd = memory_file();
        CHECK(!call(0, LUME_VM_RESIZE, 0, 8192, 0, 0, pause_fd));
        pause_base = call(0, LUME_VM_MAP, 0, 16384, 3, LUME_MAP_SHARED, pause_fd);
        pause_bytes = sizes[i];
        allocation_pause = resize_during_allocation;
        int result = lume_vm_fault(0, pause_base + offsets[i], true, false);
        CHECK(!allocation_pause && !pause_result && file_size(pause_fd) == sizes[i]);
        bool valid = offsets[i] < rounded(sizes[i]);
        CHECK(result == (valid ? 1 : -1));
        CHECK(mappings_live == (unsigned)valid && physical_live == (unsigned)valid);
        uint64_t physical;
        CHECK(lume_mmu_translate(1, pause_base + offsets[i], false, true, false, &physical) ==
              valid);
        if (!valid)
            CHECK(!((struct vm_handle *)descriptors[pause_fd].context)->object->chunks[0]);
        else
        {
            unsigned char output[8192];
            uint64_t first = offsets[i], bytes = sizes[i] - first;
            if (bytes > sizeof(output))
                bytes = sizeof(output);
            CHECK(descriptors[pause_fd].ops->pread(descriptors[pause_fd].context, output, bytes,
                                                   first) == (int64_t)bytes);
            for (uint64_t j = 0; j < bytes; ++j)
                CHECK(!output[j]);
        }
        if (!i)
            printf("VM_FAULT_EOF allocation=%zu resize_result=%lld file_bytes=0 fault_result=%d "
                   "pte_present=0\n",
                   sizeof(struct vm_chunk), (long long)pause_result, result);
        clean();
    }

    pause_fd = memory_file();
    CHECK(!call(0, LUME_VM_RESIZE, 0, 8192, 0, 0, pause_fd));
    pause_base = call(0, LUME_VM_MAP, 0, 8192, 3, LUME_MAP_PRIVATE, pause_fd);
    pause_bytes = 0;
    allocation_pause = resize_during_allocation;
    CHECK(lume_vm_fault(0, pause_base, true, false) == -1);
    CHECK(!allocation_pause && !physical_live && !mappings_live);
    CHECK(!((struct vm_handle *)descriptors[pause_fd].context)->object->chunks[0]);
    clean();

    pause_fd = memory_file();
    CHECK(!call(0, LUME_VM_RESIZE, 0, PAGE, 0, 0, pause_fd));
    pause_base = call(0, LUME_VM_MAP, 0, PAGE, 3, LUME_MAP_PRIVATE, pause_fd);
    allocation_pause = private_write_during_allocation;
    CHECK(lume_vm_fault(0, pause_base, false, false) == 1);
    CHECK(!allocation_pause);
    CHECK(*(unsigned char *)(uintptr_t)physical(0, pause_base, false, false) == 0xa5);
    clean();

    pause_fd = memory_file();
    const uint64_t boundary = 512 * PAGE;
    CHECK(!call(0, LUME_VM_RESIZE, 0, boundary + 8192, 0, 0, pause_fd));
    pause_base = call(0, LUME_VM_MAP, 0, boundary + 8192, 3, LUME_MAP_SHARED, pause_fd);
    uint64_t kept = physical(0, pause_base, true, false);
    *(unsigned char *)(uintptr_t)kept = 0xa7;
    unsigned before = metadata_live;
    pause_bytes = PAGE;
    allocation_pause = resize_during_allocation;
    CHECK(lume_vm_fault(0, pause_base + boundary, true, false) == -1);
    CHECK(!allocation_pause && !pause_result && file_size(pause_fd) == PAGE);
    CHECK(metadata_live == before && physical_live == 1 && mappings_live == 1);
    uint64_t translated;
    CHECK(lume_mmu_translate(1, pause_base, true, true, false, &translated));
    CHECK(translated == kept && *(unsigned char *)(uintptr_t)translated == 0xa7);
    CHECK(!lume_mmu_translate(1, pause_base + boundary, false, true, false, &translated));
    clean();

    void (*mutations[])(void) = {release_during_allocation,     remap_during_allocation,
                                 protect_during_allocation,     switch_view_during_allocation,
                                 switch_root_during_allocation, change_object_during_allocation};
    for (unsigned i = 0; i < sizeof(mutations) / sizeof(*mutations); ++i)
    {
        pause_fd = memory_file();
        CHECK(!call(0, LUME_VM_RESIZE, 0, 8192, 0, 0, pause_fd));
        pause_base = call(0, LUME_VM_MAP, 0, 8192, 3, LUME_MAP_SHARED, pause_fd);
        pause_object = ((struct vm_handle *)descriptors[pause_fd].context)->object;
        allocation_pause = mutations[i];
        int result = lume_vm_fault(0, pause_base, true, false);
        CHECK(!allocation_pause && result == (i >= 3 ? 2 : 0));
        CHECK(!mappings_live && !physical_live);
        if (i)
            CHECK(!pause_object->chunks[0]);
        if (i == 5)
            pause_object->changing = false;
        clean();
    }

    pause_fd = memory_file();
    CHECK(!call(0, LUME_VM_RESIZE, 0, 8192, 0, 0, pause_fd));
    pause_base = call(0, LUME_VM_MAP, 0, 8192, 3, LUME_MAP_SHARED, pause_fd);
    pause_other = call(1, LUME_VM_MAP, 0, 8192, 1, LUME_MAP_SHARED, pause_fd);
    allocation_pause = alias_fault_during_allocation;
    CHECK(lume_vm_fault(0, pause_base, true, false) == 1 && !allocation_pause);
    struct vm_object *object = ((struct vm_handle *)descriptors[pause_fd].context)->object;
    CHECK(object->chunks[0] && !object->chunks[0]->next);
    CHECK(physical_live == 1 && mappings_live == 2);
    CHECK(physical(0, pause_base, true, false) == physical(1, pause_other, false, false));
    clean();

    pause_fd = memory_file();
    CHECK(!call(0, LUME_VM_RESIZE, 0, 8192, 0, 0, pause_fd));
    pause_other = call(1, LUME_VM_MAP, 0, 8192, 1, LUME_MAP_SHARED, pause_fd);
    allocation_pause = alias_retry_during_write;
    unsigned char data[64], output[64];
    memset(data, 0x7a, sizeof(data));
    CHECK(descriptors[pause_fd].ops->write(descriptors[pause_fd].context, data, sizeof(data)) ==
          sizeof(data));
    object = ((struct vm_handle *)descriptors[pause_fd].context)->object;
    unsigned chunks = 0;
    for (struct vm_chunk *entry = object->chunks[0]; entry; entry = entry->next)
        ++chunks;
    printf("VM_WRITE_FAULT_SHARED chunks=%u frames=%u ptes=%u\n", chunks, physical_live,
           mappings_live);
    CHECK(!allocation_pause && chunks == 1 && physical_live == 1 && mappings_live == 0);
    uint64_t mapped = physical(1, pause_other, false, false);
    CHECK(!memcmp((void *)(uintptr_t)mapped, data, sizeof(data)));
    CHECK(descriptors[pause_fd].ops->pread(descriptors[pause_fd].context, output, sizeof(output),
                                           0) == sizeof(output));
    CHECK(!memcmp(output, data, sizeof(data)) && file_size(pause_fd) == 8192);
    clean();

    for (unsigned failure = 0; failure < 3; ++failure)
    {
        pause_base = call(0, LUME_VM_MAP, 0, 8192, 3, LUME_MAP_PRIVATE | LUME_MAP_ANONYMOUS, -1);
        unsigned before = metadata_live;
        if (failure == 0)
            metadata_budget = 0;
        else if (failure == 1)
            physical_budget = 0;
        else
            mapping_budget = 0;
        CHECK(lume_vm_fault(0, pause_base, true, false) == -1);
        CHECK(metadata_live == before && !physical_live && !mappings_live);
        CHECK(!spaces[0]->object->chunks[0] && spaces[0]->object->references == 1);
        clean();
    }
}

static void thread_region_cases(void)
{
    const uint64_t requests[] = {UINT64_C(2) * 1024 * 1024, UINT64_C(2) * 1024 * 1024 + 1,
                                 UINT64_C(8) * 1024 * 1024};
    for (unsigned n = 0; n < sizeof(requests) / sizeof(*requests); ++n)
    {
        uint64_t tls, stack, base, bytes, physical;
        CHECK(lume_vm_thread_region(0, 3, 64, requests[n], &tls, &stack));
        CHECK(!(tls % PAGE) && !(stack % PAGE) && stack == tls + 2 * PAGE);
        CHECK(lume_vm_thread_bounds(0, 3, &base, &bytes));
        CHECK(base == stack && bytes == rounded(requests[n]));
        CHECK(lume_vm_fault(0, tls - PAGE, true, false) == 0);
        CHECK(lume_vm_fault(0, stack - PAGE, true, false) == 0);
        CHECK(lume_vm_fault(0, stack + bytes, true, false) == 0);
        CHECK(!lume_mmu_translate(1, stack - PAGE, true, true, false, &physical));
        CHECK(lume_vm_fault(0, tls, true, false) == 1);
        CHECK(lume_vm_fault(0, stack, true, false) == 1);
        CHECK(lume_vm_fault(0, stack + bytes - 1, true, false) == 1);
        lume_vm_thread_release(0, 3);
        CHECK(!lume_vm_thread_bounds(0, 3, &base, &bytes));
        CHECK(lume_vm_fault(0, stack, true, false) == 0);
        unsigned rounds = 0;
        while (retired_threads)
        {
            CHECK(++rounds < 8);
            reap_threads(NULL);
        }
        CHECK(!mappings_live && !physical_live);
        clean();
    }
    uint64_t tls, stack;
    CHECK(!lume_vm_thread_region(0, 3, 64, UINT64_C(8) * 1024 * 1024 + 1, &tls, &stack));
    CHECK(!metadata_live && !physical_live && !mappings_live);
    for (int budget = 0; budget < 4; ++budget)
    {
        metadata_budget = budget;
        CHECK(!lume_vm_thread_region(0, 3, 64, UINT64_C(2) * 1024 * 1024, &tls, &stack));
        CHECK(!spaces[0] && !physical_live && !mappings_live);
        clean();
    }
}

static unsigned char file_data[2 * 1024 * 1024 + 12288];

static struct
{
    uint64_t size, next;
    unsigned calls, maximum, fail_call, short_call;
    uint64_t version;
} regular_file;

static void regular_close(void *context)
{
    CHECK(context == &regular_file);
}

static int64_t regular_stat(void *context, struct lume_stat *stat)
{
    CHECK(context == &regular_file);
    *stat = (struct lume_stat){.kind = LUME_NODE_FILE, .size = regular_file.size};
    return 0;
}

static int64_t regular_pread(void *context, void *data, uint64_t bytes, uint64_t offset)
{
    CHECK(context == &regular_file && bytes <= 65536);
    ++regular_file.calls;
    if (bytes > regular_file.maximum)
        regular_file.maximum = (unsigned)bytes;
    if (regular_file.calls == regular_file.fail_call)
        return -LUME_EIO;
    CHECK(offset <= regular_file.size && bytes <= regular_file.size - offset);
    memcpy(data, file_data + offset, (size_t)bytes);
    regular_file.next += bytes;
    return (int64_t)bytes - (regular_file.calls == regular_file.short_call);
}

int lume_vfs_object_identity(int fd, struct lume_file_identity *id)
{
    if (fd != 63 || !descriptors[63].ops)
        return -LUME_EBADF;
    *id = (struct lume_file_identity){
        .key = 1, .version = regular_file.version, .bytes = regular_file.size};
    return 0;
}

static const struct lume_file_ops regular_ops = {
    .kind = LUME_NODE_FILE, .close = regular_close, .stat = regular_stat, .pread = regular_pread};

static void regular_open(uint64_t size, uint64_t offset)
{
    memset(&regular_file, 0, sizeof(regular_file));
    regular_file.size = size;
    regular_file.version = 1;
    regular_file.next = offset;
    descriptors[63].ops = &regular_ops;
    descriptors[63].context = &regular_file;
}

static void file_mapping_cases(void)
{
    for (size_t i = 0; i < sizeof(file_data); ++i)
        file_data[i] = (unsigned char)(i * 37u + i / PAGE);
    const uint64_t available = 2 * 1024 * 1024 + PAGE + 3;
    regular_open(available + PAGE, PAGE);
    unsigned before = span_calls;
    struct lume_vm_request request = {.version = LUME_VM_VERSION,
                                      .length = rounded(available) + PAGE,
                                      .offset = PAGE,
                                      .protection = 3,
                                      .flags = LUME_MAP_PRIVATE,
                                      .fd = 63};
    int64_t base = lume_vm_call(&files, 1, LUME_VM_MAP, &request);
    CHECK(base > 0 && file_holds == 1 && !regular_file.calls && span_calls == before);
    uint64_t second = call(1, LUME_VM_MAP, 0, rounded(regular_file.size), 3, LUME_MAP_PRIVATE, 63);
    CHECK(second == LUME_VM_BASE && file_holds == 1 && !regular_file.calls);
    uint64_t cached = physical(0, base, false, false);
    CHECK(physical(1, second + PAGE, false, false) == cached);
    uint64_t dirty = physical(0, base, true, false);
    CHECK(dirty != cached && !memcmp((void *)(uintptr_t)dirty, file_data + PAGE, PAGE));
    *(unsigned char *)(uintptr_t)dirty ^= 0xff;
    CHECK(*(unsigned char *)(uintptr_t)cached == file_data[PAGE]);
    for (uint64_t position = PAGE; position < available; position += PAGE)
    {
        uint64_t bytes = available - position < PAGE ? available - position : PAGE;
        void *data = (void *)(uintptr_t)physical(0, base + position, false, false);
        CHECK(!memcmp(data, file_data + PAGE + position, (size_t)bytes));
        for (uint64_t i = bytes; i < PAGE; ++i)
            CHECK(!((unsigned char *)data)[i]);
    }
    CHECK(regular_file.maximum == 65536);
    CHECK(lume_vm_fault(0, base + rounded(available), false, false) == -1);
    CHECK(lume_vm_clone(0, 2));
    CHECK(physical(2, base, false, false) != dirty);
    CHECK(*(unsigned char *)(uintptr_t)physical(2, base, false, false) ==
          *(unsigned char *)(uintptr_t)dirty);
    CHECK(physical(2, base + PAGE, false, false) == physical(0, base + PAGE, false, false));
    regular_file.size = PAGE + 17;
    mapping_notify(1, regular_file.size, ++regular_file.version);
    CHECK(lume_vm_fault(0, base + PAGE, false, false) == -1);
    CHECK(*(unsigned char *)(uintptr_t)physical(0, base, false, false) ==
          *(unsigned char *)(uintptr_t)dirty);
    clean();

    regular_open(4 * PAGE, 0);
    struct lume_elf_plan plan = {.entry = UINT64_C(0x10040000),
                                 .count = 2,
                                 .segments = {{.address = UINT64_C(0x10040000),
                                               .offset = 0,
                                               .file_size = PAGE + 17,
                                               .memory_size = 3 * PAGE,
                                               .flags = 5},
                                              {.address = UINT64_C(0x10240000) + 31,
                                               .offset = PAGE + 31,
                                               .file_size = 19,
                                               .memory_size = 2 * PAGE,
                                               .flags = 6}}};
    CHECK(lume_vm_image_map(0, 63, &plan) && !regular_file.calls && !physical_live);
    CHECK(lume_vm_code_address(0, plan.entry) && !regular_file.calls);
    CHECK(!lume_vm_code_address(0, UINT64_C(0x10240000)));
    uint64_t text = physical(0, plan.entry, false, true);
    CHECK(!memcmp((void *)(uintptr_t)text, file_data, PAGE));
    CHECK(lume_vm_fault(0, plan.entry, true, false) == 0);
    uint64_t tail = physical(0, plan.entry + PAGE, false, true);
    CHECK(!memcmp((void *)(uintptr_t)tail, file_data + PAGE, 17));
    for (unsigned i = 17; i < PAGE; ++i)
        CHECK(!((unsigned char *)(uintptr_t)tail)[i]);
    uint64_t bss = physical(0, plan.entry + 2 * PAGE, false, true);
    for (unsigned i = 0; i < PAGE; ++i)
        CHECK(!((unsigned char *)(uintptr_t)bss)[i]);
    uint64_t data = physical(0, UINT64_C(0x10240000), true, false);
    for (unsigned i = 0; i < 31; ++i)
        CHECK(!((unsigned char *)(uintptr_t)data)[i]);
    CHECK(!memcmp((void *)(uintptr_t)(data + 31), file_data + PAGE + 31, 19));
    CHECK(!lume_vm_image_protect(0, plan.entry, PAGE, LUME_PROT_NONE));
    CHECK(!lume_vm_fault(0, plan.entry, false, false));
    CHECK(!lume_vm_image_protect(0, plan.entry, PAGE, LUME_PROT_READ | LUME_PROT_EXEC));
    CHECK(physical(0, plan.entry, false, true) == text);
    CHECK(lume_vm_clone(0, 1));
    CHECK(physical(1, plan.entry, false, true) == text);
    CHECK(physical(1, UINT64_C(0x10240000), true, false) != data);
    lume_vm_image_release(0);
    CHECK(!spaces[0]);
    clean();

    regular_open(65536 + 17, 0);
    span_unavailable = true;
    base = call(0, LUME_VM_MAP, 0, 73728, LUME_PROT_READ, LUME_MAP_PRIVATE, 63);
    CHECK(base > 0 && !regular_file.calls);
    CHECK(
        !memcmp((void *)(uintptr_t)physical(0, base + 65536, false, false), file_data + 65536, 17));
    clean();
    for (unsigned failure = 0; failure < 3; ++failure)
    {
        regular_open(131072, 0);
        base = call(0, LUME_VM_MAP, 0, 131072, 3, LUME_MAP_PRIVATE, 63);
        CHECK(base > 0 && !regular_file.calls);
        if (failure == 0)
            regular_file.fail_call = 1;
        else if (failure == 1)
            regular_file.short_call = 1;
        else
            physical_budget = 0;
        CHECK(lume_vm_fault(0, base, false, false) == -1);
        CHECK(!mappings_live && !physical_live);
        clean();
    }
    regular_open(131072, 0);
    base = call(0, LUME_VM_MAP, 0, 131072, 3, LUME_MAP_PRIVATE, 63);
    storage_context = false;
    CHECK(lume_vm_fault(0, base, false, false) == 2 && !regular_file.calls);
    CHECK(lume_vm_fault(1, base, false, false) == 0);
    CHECK(file_cache->loading && file_holds == 1);
    file_load(file_cache);
    storage_context = true;
    CHECK(lume_vm_fault(0, base, false, false) == 1 && regular_file.calls == 1);
    clean();
    regular_open(131072, 0);
    base = call(0, LUME_VM_MAP, 0, 131072, 3, LUME_MAP_PRIVATE, 63);
    storage_context = false;
    CHECK(lume_vm_fault(0, base, false, false) == 2);
    lume_vm_release(0);
    CHECK(file_cache->loading && !spaces[0]);
    file_load(file_cache);
    storage_context = true;
    clean();
}

static int pin_close_fd;

static void close_during_pin(void)
{
    CHECK(call(0, LUME_VM_RESIZE, 0, PAGE, 0, 0, pin_close_fd) == -LUME_EBUSY);
    CHECK(!lume_vfs_close(&files, (unsigned)pin_close_fd));
}

static const struct lume_vm_dma_pin *device_memory(void *context)
{
    return context;
}

static void close_device_memory(void *context)
{
    struct lume_vm_dma_pin *pin = context;
    lume_vm_dma_unpin(&pin);
}

static const struct lume_file_ops device_memory_ops = {
    .kind = LUME_NODE_FILE, .close = close_device_memory, .memory = device_memory};

static void dma_pin_cases(void)
{
    struct lume_vm_dma_pin *pin = NULL;
    struct lume_vm_dma_span span;
    CHECK(!lume_vm_dma_allocate(&files, 2 * PAGE, &pin, &span));
    int provider = lume_vfs_attach(&files, &device_memory_ops, pin, 0);
    CHECK(provider >= 0);
    lume_vfs_access_mode(&files, (unsigned)provider, LUME_O_RDONLY);
    CHECK(call(0, LUME_VM_MAP, 0, PAGE, 3, LUME_MAP_SHARED, provider) == -LUME_EACCES);
    lume_vfs_access_mode(&files, (unsigned)provider, LUME_O_RDWR);
    CHECK(call(0, LUME_VM_MAP, 0, PAGE, 3, LUME_MAP_PRIVATE, provider) == -LUME_EACCES);
    CHECK(call(0, LUME_VM_MAP, 0, 3 * PAGE, 3, LUME_MAP_SHARED, provider) == -LUME_EINVAL);
    CHECK(call(0, LUME_VM_MAP, 0, PAGE, 5, LUME_MAP_SHARED, provider) == -LUME_EACCES);
    int64_t device_mapping = call(0, LUME_VM_MAP, 0, 2 * PAGE, 3, LUME_MAP_SHARED, provider);
    CHECK(device_mapping > 0 && physical(0, device_mapping, true, false) == span.pages[0]);
    CHECK(!lume_vfs_close(&files, (unsigned)provider));
    pin = NULL;
    CHECK(physical(0, device_mapping, false, false));
    clean();
    int provider_source = memory_file();
    CHECK(!call(0, LUME_VM_RESIZE, 0, 3 * PAGE, 0, 0, provider_source));
    CHECK(!lume_vm_dma_pin(&files, (unsigned)provider_source, PAGE, PAGE, false, &pin, &span));
    provider = lume_vfs_attach(&files, &device_memory_ops, pin, 0);
    CHECK(provider >= 0);
    CHECK(call(0, LUME_VM_MAP, 0, PAGE, 3, LUME_MAP_SHARED, provider) == -LUME_EACCES);
    device_mapping = call(0, LUME_VM_MAP, 0, PAGE, 1, LUME_MAP_SHARED, provider);
    CHECK(device_mapping > 0 && physical(0, device_mapping, false, false) == span.pages[0]);
    CHECK(!lume_vfs_close(&files, (unsigned)provider_source));
    CHECK(!lume_vfs_close(&files, (unsigned)provider));
    pin = NULL;
    CHECK(physical(0, device_mapping, false, false));
    clean();
    CHECK(!lume_vm_dma_allocate(&files, 2 * PAGE, &pin, &span));
    CHECK(span.count == 2 && span.offset == 0 && span.bytes == 2 * PAGE);
    int exported = lume_vm_dma_export(&files, pin, true);
    CHECK(exported >= 0);
    int64_t mapped = call(0, LUME_VM_MAP, 0, 2 * PAGE, 3, LUME_MAP_SHARED, exported);
    CHECK(mapped > 0 && physical(0, mapped, true, false) == span.pages[0]);
    CHECK(!lume_vfs_close(&files, (unsigned)exported));
    lume_vm_dma_unpin(&pin);
    CHECK(physical(0, mapped, false, false));
    clean();
    CHECK(!lume_vm_dma_allocate(&files, PAGE, &pin, &span));
    exported = lume_vm_dma_export(&files, pin, false);
    CHECK(exported >= 0);
    CHECK(call(0, LUME_VM_MAP, 0, PAGE, 3, LUME_MAP_SHARED, exported) == -LUME_EACCES);
    CHECK(call(0, LUME_VM_MAP, 0, PAGE, 1, LUME_MAP_SHARED, exported) > 0);
    lume_vm_dma_unpin(&pin);
    clean();
    int fd = memory_file();
    CHECK(!call(0, LUME_VM_RESIZE, 0, 3 * PAGE, 0, 0, fd));
    uint64_t base = call(0, LUME_VM_MAP, 0, 3 * PAGE, 3, LUME_MAP_SHARED, fd);
    CHECK(base > 0);
    CHECK(!lume_vm_dma_pin(&files, (unsigned)fd, PAGE, 2 * PAGE, true, &pin, &span));
    CHECK(span.serial && span.offset == PAGE && span.bytes == 2 * PAGE && span.count == 2);
    CHECK(lume_vm_dma_export(&files, pin, true) == -LUME_EINVAL);
    CHECK(span.pages[0] == physical(0, base + PAGE, true, false));
    CHECK(call(0, LUME_VM_RESIZE, 0, 4 * PAGE, 0, 0, fd) == -LUME_EBUSY);
    CHECK(call(0, LUME_VM_RESIZE, 0, PAGE, 0, 0, fd) == -LUME_EBUSY);
    CHECK(!call(0, LUME_VM_RESIZE, 0, 3 * PAGE, 0, 0, fd));
    unsigned char value = 0x5a;
    unsigned frames_before = physical_live;
    CHECK(descriptors[fd].ops->seek(descriptors[fd].context, 3 * PAGE, 0) == 3 * PAGE);
    CHECK(descriptors[fd].ops->write(descriptors[fd].context, &value, 1) == -LUME_EBUSY);
    CHECK(descriptors[fd].ops->pwrite(descriptors[fd].context, &value, 1, 4 * PAGE) == -LUME_EBUSY);
    CHECK(file_size(fd) == 3 * PAGE && physical_live == frames_before);
    CHECK(call(0, LUME_VM_MAP, 0, PAGE, 1, LUME_MAP_PRIVATE, fd) == -LUME_EBUSY);
    CHECK(!call(0, LUME_VM_PROTECT, base, 3 * PAGE, 1, 0, -1));
    CHECK(call(0, LUME_VM_PROTECT, base, 3 * PAGE, 5, 0, -1) == -LUME_EACCES);
    CHECK(call(0, LUME_VM_ADD_SEALS, 0, 0, 0, LUME_SEAL_FUTURE_WRITE, fd) == -LUME_EBUSY);
    CHECK(!lume_vfs_close(&files, (unsigned)fd));
    lume_vm_release(0);
    drain();
    CHECK(physical_live == 2);
    lume_vm_dma_unpin(&pin);
    CHECK(!pin);
    lume_vm_dma_unpin(&pin);
    clean();

    for (unsigned failure = 0; failure < 3; ++failure)
    {
        fd = memory_file();
        CHECK(!call(0, LUME_VM_RESIZE, 0, 2 * PAGE, 0, 0, fd));
        if (failure)
            physical_budget = (int)failure - 1;
        else
            metadata_budget = 0;
        CHECK(lume_vm_dma_pin(&files, (unsigned)fd, 0, 2 * PAGE, true, &pin, &span) ==
              -LUME_ENOMEM);
        CHECK(!pin && !span.pages);
        struct vm_object *object = ((struct vm_handle *)descriptors[fd].context)->object;
        CHECK(!object->changing && !object->dma_pins && !object->dma_writers);
        clean();
    }
    fd = memory_file();
    CHECK(!call(0, LUME_VM_RESIZE, 0, PAGE, 0, 0, fd));
    pin_close_fd = fd;
    allocation_pause = close_during_pin;
    CHECK(!lume_vm_dma_pin(&files, (unsigned)fd, 0, PAGE, true, &pin, &span));
    CHECK(!descriptors[fd].ops && physical_live == 1);
    lume_vm_dma_unpin(&pin);
    clean();

    fd = memory_file();
    CHECK(!call(0, LUME_VM_RESIZE, 0, PAGE, 0, 0, fd));
    CHECK(call(0, LUME_VM_MAP, 0, PAGE, 1, LUME_MAP_PRIVATE, fd) > 0);
    CHECK(lume_vm_dma_pin(&files, (unsigned)fd, 0, PAGE, true, &pin, &span) == -ENOTSUP);
    clean();
    fd = memory_file();
    CHECK(!call(0, LUME_VM_RESIZE, 0, PAGE, 0, 0, fd));
    CHECK(call(0, LUME_VM_MAP, 0, PAGE, 5, LUME_MAP_SHARED, fd) > 0);
    CHECK(lume_vm_dma_pin(&files, (unsigned)fd, 0, PAGE, false, &pin, &span) == -LUME_EACCES);
    clean();
    fd = memory_file();
    CHECK(!call(0, LUME_VM_RESIZE, 0, PAGE, 0, 0, fd));
    CHECK(!lume_vm_dma_pin(&files, (unsigned)fd, 0, PAGE, false, &pin, &span));
    CHECK(!call(0, LUME_VM_ADD_SEALS, 0, 0, 0, LUME_SEAL_WRITE, fd));
    lume_vm_dma_unpin(&pin);
    CHECK(lume_vm_dma_pin(&files, (unsigned)fd, 0, PAGE, true, &pin, &span) == -LUME_EACCES);
    CHECK(!lume_vm_dma_pin(&files, (unsigned)fd, 0, PAGE, false, &pin, &span));
    lume_vm_dma_unpin(&pin);
    clean();
}
