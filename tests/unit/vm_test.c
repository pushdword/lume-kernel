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

static struct
{
    unsigned task;
    uint64_t address, physical;
    bool write, execute, live;
} mappings[4096];

static uint64_t frames[4096];

static struct
{
    const struct lume_file_ops *ops;
    void *context;
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
        ++metadata_live;
    return value;
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

bool lume_page_free(uint64_t physical)
{
    for (unsigned i = 0; i < 4096; ++i)
        if (mappings[i].live)
            CHECK(mappings[i].physical != physical);
    for (unsigned i = 0; i < 4096; ++i)
        if (frames[i] == physical)
        {
#if defined(_WIN32)
            _aligned_free((void *)(uintptr_t)physical);
#else
            free((void *)(uintptr_t)physical);
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
    return task + 1;
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
    (void)task;
    return false;
}

uint64_t lume_mmu_task_view_root(unsigned task, bool writer)
{
    CHECK(!writer);
    return task + 1;
}

bool lume_mmu_root_matches(unsigned task, uint64_t root)
{
    return root == task + 1;
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
            return fd;
        }
    return -LUME_EMFILE;
}

void *lume_vfs_external(struct lume_files *files, unsigned fd, const struct lume_file_ops *ops)
{
    (void)files;
    return fd < 64 && descriptors[fd].ops == ops ? descriptors[fd].context : NULL;
}

void lume_vfs_access_mode(struct lume_files *files, unsigned fd, unsigned mode)
{
    (void)files;
    (void)fd;
    (void)mode;
}

int lume_vfs_hold(struct lume_files *files, unsigned fd)
{
    (void)files;
    return fd < 64 && descriptors[fd].ops ? (int)fd : -LUME_EBADF;
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
    CHECK(lume_vm_fault(task, address, write, execute) == 1);
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
    for (unsigned i = 0; i < 64; ++i)
        lume_vm_release(i);
    for (unsigned fd = 0; fd < 64; ++fd)
        if (descriptors[fd].ops)
            lume_vfs_close(&files, fd);
    drain();
    CHECK(!metadata_live && !physical_live && !mappings_live && !retired_threads);
}

static void write_cases(void);

int main(void)
{
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
