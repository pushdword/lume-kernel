#include "lume/vm.h"
#include "lume/early.h"
#include "lume/elf.h"
#include "lume/mmu.h"
#include "lume/external_file.h"
#include "lume/kernel_memory.h"
#include "lume/worker.h"
#include "lume/work.h"
#include <string.h>

#define PAGE UINT64_C(4096)
#define ENOTSUP 95
#define HASH 64u

struct vm_chunk
{
    uint64_t index, pages[512];
    struct vm_chunk *next;
};

struct vm_area;

struct vm_object
{
    uint64_t bytes, serial;
    unsigned references, seals;
    bool persistent, changing, discardable;
    unsigned uid, gid, mode;
    struct vm_chunk *chunks[HASH];
    struct vm_object *next;
    struct vm_area *areas;
    char name[LUME_VM_NAME_BYTES];
};

struct vm_area
{
    uint64_t first, end, offset;
    unsigned protection, maximum, thread, saved, owner;
    bool shared, retiring, jit;
    struct vm_object *object;
    struct vm_area *next, *object_next, *retire_next;
};

struct vm_handle
{
    struct vm_object *object;
    uint64_t offset;
    bool write;
};

static struct vm_area *spaces[LUME_TASK_LIMIT];
static struct vm_object *names, *retired;
static uint64_t serial;
static bool changing[LUME_TASK_LIMIT];
static void reap(void *);
static void reap_threads(void *);
static struct vm_area *retired_threads, *retired_threads_tail;
static struct lume_work reap_work = {.run = reap, .domain = LUME_WORK_GENERAL};
static struct lume_work thread_reap_work = {.run = reap_threads, .domain = LUME_WORK_GENERAL};

static void *allocate(size_t size)
{
    void *memory = lume_kernel_allocate(size);
    if (memory)
        memset(memory, 0, size);
    return memory;
}

static uint64_t rounded(uint64_t bytes)
{
    return bytes && bytes <= UINT64_MAX - (PAGE - 1) ? (bytes + PAGE - 1) & ~(PAGE - 1) : 0;
}

static struct vm_chunk *chunk(struct vm_object *object, uint64_t page, bool create)
{
    uint64_t index = page / 512;
    unsigned bucket = (unsigned)(index % HASH);
    struct vm_chunk *entry = object->chunks[bucket];
    while (entry && entry->index != index)
        entry = entry->next;
    if (!entry && create)
    {
        entry = allocate(sizeof(*entry));
        if (entry)
        {
            entry->index = index;
            entry->next = object->chunks[bucket];
            object->chunks[bucket] = entry;
        }
    }
    return entry;
}

static uint64_t *page_entry(struct vm_object *object, uint64_t page, bool create)
{
    struct vm_chunk *entry = chunk(object, page, create);
    return entry ? entry->pages + page % 512 : NULL;
}

static uint64_t get_page(struct vm_object *object, uint64_t page)
{
    uint64_t *entry = page_entry(object, page, true);
    if (entry && !*entry)
        *entry = lume_page_alloc(0);
    return entry ? *entry : 0;
}

static struct vm_object *new_object(uint64_t bytes, bool persistent)
{
    struct vm_object *object = allocate(sizeof(*object));
    if (object)
    {
        object->bytes = bytes;
        object->persistent = persistent;
        object->discardable = true;
        object->serial = UINT64_C(0x40000000) + ++serial;
        object->references = 1;
    }
    return object;
}

static void drop(struct vm_object *object)
{
    if (--object->references)
        return;
    object->next = retired;
    retired = object;
    (void)lume_work_submit(&reap_work);
}

/* Closed descriptors only retire objects. Reference transfer never suspends. */
static void reap(void *unused)
{
    (void)unused;
    unsigned budget = 16;
    while (retired && budget)
    {
        struct vm_object *object = retired;
        bool empty = true;
        for (unsigned bucket = 0; bucket < HASH && budget; ++bucket)
            while (object->chunks[bucket] && budget)
            {
                struct vm_chunk *entry = object->chunks[bucket];
                for (unsigned i = 0; i < 512 && budget; ++i)
                    if (entry->pages[i])
                    {
                        if (!lume_page_free(entry->pages[i]))
                            lume_fail("virtual memory frame release");
                        entry->pages[i] = 0;
                        --budget;
                    }
                bool live = false;
                for (unsigned i = 0; i < 512; ++i)
                    live |= entry->pages[i] != 0;
                if (live)
                {
                    empty = false;
                    break;
                }
                if (budget < 2)
                {
                    empty = false;
                    budget = 0;
                    break;
                }
                object->chunks[bucket] = entry->next;
                lume_kernel_free(entry);
                budget -= 2;
            }
        for (unsigned bucket = 0; bucket < HASH; ++bucket)
            empty &= object->chunks[bucket] == NULL;
        if (!empty || !budget)
            break;
        retired = object->next;
        lume_kernel_free(object);
        --budget;
    }
    if (retired)
        (void)lume_work_submit(&reap_work);
}

static struct vm_area *find_area(unsigned task, uint64_t address)
{
    for (struct vm_area *area = spaces[task]; area && area->first <= address; area = area->next)
        if (address < area->end)
            return area;
    return NULL;
}

static bool overlap(uint64_t a, uint64_t z, uint64_t b, uint64_t e)
{
    return a < e && b < z;
}

static bool writable_alias(struct vm_object *object, uint64_t offset, uint64_t end,
                           unsigned protection, unsigned task, uint64_t first, uint64_t last)
{
    for (struct vm_area *area = object->areas; area; area = area->object_next)
    {
        if (!overlap(offset, end, area->offset, area->offset + area->end - area->first))
            continue;
        if (area->owner == task)
        {
            uint64_t a = offset > area->offset ? offset : area->offset;
            uint64_t z = end < area->offset + area->end - area->first
                             ? end
                             : area->offset + area->end - area->first;
            if (area->first + a - area->offset >= first && area->first + z - area->offset <= last)
                continue;
        }
        if (((protection & LUME_PROT_EXEC) && (area->protection & LUME_PROT_WRITE)) ||
            ((protection & LUME_PROT_WRITE) && (area->protection & LUME_PROT_EXEC)))
            return true;
    }
    return false;
}

static bool covered(struct vm_object *object, uint64_t page)
{
    uint64_t offset = page * PAGE;
    for (struct vm_area *area = object->areas; area; area = area->object_next)
        if (offset >= area->offset && offset - area->offset < area->end - area->first)
            return true;
    return false;
}

static void prune_chunks(struct vm_object *object)
{
    for (unsigned bucket = 0; bucket < HASH; ++bucket)
    {
        struct vm_chunk **position = &object->chunks[bucket];
        while (*position)
        {
            struct vm_chunk *entry = *position;
            bool live = false;
            for (unsigned i = 0; i < 512; ++i)
                live |= entry->pages[i] != 0;
            if (live)
                position = &entry->next;
            else
            {
                *position = entry->next;
                lume_kernel_free(entry);
            }
            lume_worker_checkpoint(PAGE);
        }
    }
}

static void trim(struct vm_object *object)
{
    if (object->persistent || object->references == 1)
        return;
    unsigned count = 0;
    for (unsigned bucket = 0; bucket < HASH; ++bucket)
        for (struct vm_chunk *entry = object->chunks[bucket]; entry; entry = entry->next)
            for (unsigned i = 0; i < 512; ++i)
                if (entry->pages[i] && !covered(object, entry->index * 512 + i))
                {
                    if (!lume_page_free(entry->pages[i]))
                        lume_fail("virtual memory frame release");
                    entry->pages[i] = 0;
                    if (++count % 16 == 0)
                        lume_worker_checkpoint(65536);
                }
    prune_chunks(object);
}

static void insert(unsigned task, struct vm_area *area)
{
    struct vm_area **position = &spaces[task];
    while (*position && (*position)->first < area->first)
        position = &(*position)->next;
    area->next = *position;
    *position = area;
    area->owner = task;
    area->object_next = area->object->areas;
    area->object->areas = area;
}

static bool free_range(unsigned task, uint64_t first, uint64_t end)
{
    for (struct vm_area *area = spaces[task]; area && area->first < end; area = area->next)
        if (area->end > first)
            return false;
    return true;
}

/* At most two boundary nodes are needed. Allocation precedes every mutation. */
static bool boundaries(unsigned task, uint64_t first, uint64_t end)
{
    struct vm_area *a = find_area(task, first), *z = find_area(task, end);
    bool left = a && first != a->first;
    bool right = z && end != z->first;
    struct vm_area *split_a = left ? allocate(sizeof(*split_a)) : NULL;
    struct vm_area *split_z = right ? allocate(sizeof(*split_z)) : NULL;
    if ((left && !split_a) || (right && !split_z))
    {
        lume_kernel_free(split_a);
        lume_kernel_free(split_z);
        return false;
    }
    if (left)
    {
        *split_a = *a;
        split_a->first = first;
        split_a->offset += first - a->first;
        a->end = first;
        a->next = split_a;
        ++a->object->references;
        split_a->object_next = a->object->areas;
        a->object->areas = split_a;
        if (z == a)
            z = split_a;
    }
    if (right)
    {
        *split_z = *z;
        split_z->first = end;
        split_z->offset += end - z->first;
        z->end = end;
        z->next = split_z;
        ++z->object->references;
        split_z->object_next = z->object->areas;
        z->object->areas = split_z;
    }
    return true;
}

static void revoke(unsigned task, struct vm_area *area, uint64_t offset, uint64_t end)
{
    uint64_t addresses[LUME_MMU_BATCH_PAGES];
    unsigned count = 0, visited = 0;
    for (unsigned bucket = 0; bucket < HASH; ++bucket)
        for (struct vm_chunk *entry = area->object->chunks[bucket]; entry; entry = entry->next)
            for (unsigned i = 0; i < 512; ++i)
            {
                if ((++visited & 511) == 0)
                    lume_worker_checkpoint(PAGE);
                uint64_t position = (entry->index * 512 + i) * PAGE, physical;
                if (!entry->pages[i] || position < offset || position >= end ||
                    position < area->offset || position - area->offset >= area->end - area->first)
                    continue;
                uint64_t address = area->first + position - area->offset;
                if (!lume_mmu_translate(lume_mmu_task_root(task), address, false, true, false,
                                        &physical))
                    continue;
                addresses[count++] = address;
                if (count == LUME_MMU_BATCH_PAGES)
                {
                    if (!lume_mmu_unmap_user_batch(task, addresses, count))
                        lume_fail("virtual memory mapping release");
                    count = 0;
                    lume_worker_checkpoint(65536);
                }
            }
    if (count && !lume_mmu_unmap_user_batch(task, addresses, count))
        lume_fail("virtual memory mapping release");
    lume_worker_checkpoint(PAGE);
}

static void forget_retiring(struct vm_area *area)
{
    struct vm_area **position = &retired_threads, *previous = NULL;
    while (*position && *position != area)
    {
        previous = *position;
        position = &(*position)->retire_next;
    }
    if (*position)
    {
        *position = area->retire_next;
        if (retired_threads_tail == area)
            retired_threads_tail = previous;
        area->retire_next = NULL;
    }
}

static void remove_range(unsigned task, uint64_t first, uint64_t end)
{
    struct vm_area **position = &spaces[task];
    while (*position)
    {
        struct vm_area *area = *position;
        if (area->first >= first && area->end <= end)
        {
            if (area->retiring)
                forget_retiring(area);
            bool was_changing = area->object->changing;
            area->object->changing = true;
            *position = area->next;
            struct vm_area **alias = &area->object->areas;
            while (*alias && *alias != area)
                alias = &(*alias)->object_next;
            if (!*alias)
                lume_fail("virtual memory alias ownership");
            *alias = area->object_next;
            revoke(task, area, area->offset, area->offset + area->end - area->first);
            trim(area->object);
            area->object->changing = was_changing;
            drop(area->object);
            lume_kernel_free(area);
            lume_worker_checkpoint(PAGE);
        }
        else
            position = &area->next;
    }
}

int lume_vm_fault(unsigned task, uint64_t address, bool write, bool execute)
{
    if (task >= LUME_TASK_LIMIT)
        return 0;
    struct vm_area *area = find_area(task, address);
    unsigned required = execute ? LUME_PROT_EXEC : write ? LUME_PROT_WRITE : LUME_PROT_READ;
    if (!area || area->retiring)
        return 0;
    if (changing[task] || area->object->changing)
        return 2;
    if (!(area->protection & required))
        return 0;
    bool writer = false;
#if defined(__aarch64__) || defined(LUME_ARCH_AARCH64)
    writer = lume_mmu_jit_active(task);
#endif
    if (area->jit &&
        ((execute && writer) || (write && !writer && (area->protection & LUME_PROT_EXEC))))
        return 0;
    uint64_t offset = area->offset + address - area->first;
    if (offset / PAGE >= (area->object->bytes + PAGE - 1) / PAGE)
        return -1;
    uint64_t physical = get_page(area->object, offset / PAGE);
    if (!physical)
        return -1;
    uint64_t existing, root = lume_mmu_task_root(task);
#if defined(__aarch64__) || defined(LUME_ARCH_AARCH64)
    root = lume_mmu_task_view_root(task, writer);
#endif
    if (lume_mmu_translate(root, address, write, true, execute, &existing))
        return 1;
    if (area->protection & LUME_PROT_EXEC)
        lume_sync_code(physical, PAGE);
    bool mapped;
#if defined(__aarch64__) || defined(LUME_ARCH_AARCH64)
    if (area->jit)
        mapped = lume_mmu_map_user_jit(task, address & ~(PAGE - 1), physical,
                                       (area->protection & LUME_PROT_WRITE) != 0,
                                       (area->protection & LUME_PROT_EXEC) != 0);
    else
#endif
        mapped = lume_mmu_map_user(task, address & ~(PAGE - 1), physical,
                                   (area->protection & LUME_PROT_WRITE) != 0,
                                   (area->protection & LUME_PROT_EXEC) != 0);
    if (!mapped)
        return -1;
    return 1;
}

bool lume_vm_resolve_buffer(uint64_t root, uint64_t address, bool write)
{
    if (!lume_vm_address(address))
        return false;
    for (unsigned task = 0; task < LUME_TASK_LIMIT; ++task)
        if (spaces[task] &&
#if defined(__aarch64__) || defined(LUME_ARCH_AARCH64)
            lume_mmu_root_matches(task, root)
#else
            lume_mmu_task_root(task) == root
#endif
        )
            return lume_vm_fault(task, address, write, false) == 1;
    return false;
}

static int64_t handle_read_at(void *context, void *data, uint64_t bytes, uint64_t offset)
{
    struct vm_handle *handle = context;
    struct vm_object *object = handle->object;
    if (object->changing)
        return -LUME_EAGAIN;
    if (offset >= object->bytes)
        return 0;
    if (bytes > object->bytes - offset)
        bytes = object->bytes - offset;
    uint64_t done = 0;
    while (done < bytes)
    {
        unsigned part = PAGE - (unsigned)(offset & (PAGE - 1));
        if (part > bytes - done)
            part = (unsigned)(bytes - done);
        uint64_t *entry = page_entry(object, offset / PAGE, false);
        if (entry && *entry)
            memcpy((char *)data + done, (void *)(uintptr_t)(*entry + offset % PAGE), part);
        else
            memset((char *)data + done, 0, part);
        offset += part;
        done += part;
    }
    return (int64_t)done;
}

static int64_t handle_read(void *context, void *data, uint64_t bytes)
{
    struct vm_handle *handle = context;
    int64_t result = handle_read_at(context, data, bytes, handle->offset);
    if (result > 0)
        handle->offset += (uint64_t)result;
    return result;
}

static void zero_old_tail(struct vm_object *object)
{
    if (!(object->bytes & (PAGE - 1)))
        return;
    uint64_t first = object->bytes & ~(PAGE - 1);
    uint64_t *entry = page_entry(object, first / PAGE, false);
    if (!entry || !*entry)
        return;
    bool was_changing = object->changing;
    object->changing = true;
    for (struct vm_area *area = object->areas; area; area = area->object_next)
        revoke(area->owner, area, first, first + PAGE);
    memset((void *)(uintptr_t)(*entry + object->bytes % PAGE), 0, PAGE - object->bytes % PAGE);
    object->changing = was_changing;
}

static int64_t resize_object(struct vm_object *object, uint64_t bytes)
{
    if (object->changing)
        return -LUME_EAGAIN;
    if (bytes > INT64_MAX || (bytes < object->bytes && (object->seals & LUME_SEAL_SHRINK)) ||
        (bytes > object->bytes && (object->seals & LUME_SEAL_GROW)))
        return -LUME_EPERM;
    if (bytes >= object->bytes)
    {
        if (bytes > object->bytes)
            zero_old_tail(object);
        object->bytes = bytes;
        return 0;
    }
    object->changing = true;
    uint64_t first = bytes & ~(PAGE - 1);
    /* Publish EOF before yielding. Faults cannot recreate truncated frames. */
    object->bytes = bytes;
    for (struct vm_area *area = object->areas; area; area = area->object_next)
        revoke(area->owner, area, first, UINT64_MAX);
    unsigned released = 0;
    for (unsigned bucket = 0; bucket < HASH; ++bucket)
        for (struct vm_chunk *entry = object->chunks[bucket]; entry; entry = entry->next)
            for (unsigned i = 0; i < 512; ++i)
            {
                uint64_t position = (entry->index * 512 + i) * PAGE;
                if (!entry->pages[i] || position < first)
                    continue;
                if (position == first && (bytes & (PAGE - 1)))
                    memset((void *)(uintptr_t)(entry->pages[i] + bytes % PAGE), 0,
                           PAGE - bytes % PAGE);
                else
                {
                    if (!lume_page_free(entry->pages[i]))
                        lume_fail("virtual memory resize release");
                    entry->pages[i] = 0;
                    if (++released % 16 == 0)
                        lume_worker_checkpoint(65536);
                }
            }
    prune_chunks(object);
    object->changing = false;
    return 0;
}

static int64_t handle_resize(void *context, uint64_t bytes)
{
    struct vm_handle *handle = context;
    return handle->write ? resize_object(handle->object, bytes) : -LUME_EACCES;
}

static int64_t handle_write(void *context, const void *data, uint64_t bytes)
{
    struct vm_handle *handle = context;
    struct vm_object *object = handle->object;
    if (object->changing)
        return -LUME_EAGAIN;
    if (!handle->write || (object->seals & (LUME_SEAL_WRITE | LUME_SEAL_FUTURE_WRITE)))
        return -LUME_EPERM;
    if (bytes > INT64_MAX - handle->offset)
        return -LUME_EINVAL;
    if (!bytes)
        return 0;
    uint64_t first = handle->offset, end = first + bytes;
    if (writable_alias(object, first, end, LUME_PROT_WRITE, LUME_TASK_LIMIT, 0, 0))
        return -LUME_EACCES;
    if (end > object->bytes)
    {
        if (object->seals & LUME_SEAL_GROW)
            return -LUME_EPERM;
        zero_old_tail(object);
    }
    uint64_t done = 0;
    while (done < bytes)
    {
        uint64_t offset = first + done, physical = get_page(object, offset / PAGE);
        if (!physical)
            break;
        unsigned part = PAGE - (unsigned)(offset % PAGE);
        if (part > bytes - done)
            part = (unsigned)(bytes - done);
        memcpy((void *)(uintptr_t)(physical + offset % PAGE), (const char *)data + done, part);
        done += part;
    }
    handle->offset += done;
    if (done && handle->offset > object->bytes)
        object->bytes = handle->offset;
    return done ? (int64_t)done : -LUME_ENOMEM;
}

static int64_t handle_write_at(void *context, const void *data, uint64_t bytes, uint64_t offset)
{
    struct vm_handle handle = *(const struct vm_handle *)context;
    handle.offset = offset;
    return handle_write(&handle, data, bytes);
}

static int64_t handle_seek(void *context, int64_t offset, unsigned whence)
{
    struct vm_handle *handle = context;
    uint64_t base = whence == 0 ? 0 : whence == 1 ? handle->offset : handle->object->bytes;
    if (whence > 2 || base > INT64_MAX || (offset < 0 && (uint64_t)(-(offset + 1)) + 1 > base) ||
        (offset > 0 && (uint64_t)offset > INT64_MAX - base))
        return -LUME_EINVAL;
    handle->offset = base + (uint64_t)offset;
    return (int64_t)handle->offset;
}

static int64_t handle_stat(void *context, struct lume_stat *stat)
{
    struct vm_handle *handle = context;
    *stat = (struct lume_stat){
        .inode = handle->object->serial, .size = handle->object->bytes, .kind = LUME_NODE_FILE};
    return 0;
}

static unsigned handle_poll(void *context)
{
    return LUME_POLLIN | (((struct vm_handle *)context)->write ? LUME_POLLOUT : 0);
}

static void handle_close(void *context)
{
    struct vm_handle *handle = context;
    drop(handle->object);
    lume_kernel_free(handle);
}

static const struct lume_file_ops handle_ops = {.kind = LUME_NODE_FILE,
                                                .close = handle_close,
                                                .read = handle_read,
                                                .write = handle_write,
                                                .poll = handle_poll,
                                                .memory_io = true,
                                                .seek = handle_seek,
                                                .stat = handle_stat,
                                                .resize = handle_resize,
                                                .pread = handle_read_at,
                                                .pwrite = handle_write_at};

static int attach(struct lume_files *files, struct vm_object *object, bool write, unsigned flags)
{
    struct vm_handle *handle = allocate(sizeof(*handle));
    if (!handle)
        return -LUME_ENOMEM;
    *handle = (struct vm_handle){.object = object, .write = write};
    int fd = lume_vfs_attach(files, &handle_ops, handle,
                             flags & LUME_MEMFD_CLOEXEC ? LUME_FD_CLOEXEC : 0);
    if (fd < 0)
        lume_kernel_free(handle);
    else
    {
        ++object->references;
        lume_vfs_access_mode(files, (unsigned)fd, write ? LUME_O_RDWR : LUME_O_RDONLY);
    }
    return fd;
}

struct page_copy
{
    struct
    {
        uint64_t source, target;
    } pages[LUME_MMU_BATCH_PAGES];

    unsigned count;
};

static void copy_pages(void *context)
{
    const struct page_copy *copy = context;
    for (unsigned i = 0; i < copy->count; ++i)
        memcpy((void *)(uintptr_t)copy->pages[i].target,
               (const void *)(uintptr_t)copy->pages[i].source, PAGE);
}

static bool copy_object(struct vm_object *source, uint64_t offset, uint64_t length,
                        struct vm_object *target)
{
    struct page_copy copy = {0};
    for (unsigned bucket = 0; bucket < HASH; ++bucket)
        for (struct vm_chunk *entry = source->chunks[bucket]; entry; entry = entry->next)
            for (unsigned i = 0; i < 512; ++i)
            {
                uint64_t position = (entry->index * 512 + i) * PAGE;
                if (!entry->pages[i] || position < offset || position - offset >= length)
                    continue;
                uint64_t page = get_page(target, (position - offset) / PAGE);
                if (!page)
                    return false;
                copy.pages[copy.count].source = entry->pages[i];
                copy.pages[copy.count++].target = page;
                if (copy.count == LUME_MMU_BATCH_PAGES)
                {
                    lume_worker_compute(copy_pages, &copy);
                    lume_worker_checkpoint(65536);
                    copy.count = 0;
                }
            }
    if (copy.count)
        copy_pages(&copy);
    return true;
}

void lume_vm_poll_background(void)
{
    if (retired)
        (void)lume_work_submit(&reap_work);
    if (retired_threads)
        (void)lume_work_submit(&thread_reap_work);
}

bool lume_vm_file_call(unsigned operation, const struct lume_vm_request *request)
{
    return operation == LUME_VM_MAP && !(request->flags & LUME_MAP_ANONYMOUS);
}

static int64_t map(struct lume_files *files, unsigned task, struct lume_vm_request *request)
{
    uint64_t length = rounded(request->length);
    unsigned flags = request->flags, protection = request->protection;
    const unsigned allowed = LUME_MAP_SHARED | LUME_MAP_PRIVATE | LUME_MAP_FIXED |
                             LUME_MAP_ANONYMOUS | LUME_MAP_NORESERVE | LUME_MAP_FIXED_NOREPLACE |
                             LUME_MAP_JIT;
    bool jit = (flags & LUME_MAP_JIT) != 0;
#if !defined(__aarch64__) && !defined(LUME_ARCH_AARCH64)
    if (jit)
        return -LUME_ENOSYS;
#endif
    if (jit && ((flags & (LUME_MAP_PRIVATE | LUME_MAP_SHARED | LUME_MAP_ANONYMOUS)) !=
                    (LUME_MAP_PRIVATE | LUME_MAP_ANONYMOUS) ||
                request->fd != -1))
        return -LUME_EINVAL;
    if (!length || length > LUME_VM_LIMIT - LUME_VM_BASE || (flags & ~allowed) ||
        (flags & (LUME_MAP_SHARED | LUME_MAP_PRIVATE)) == 0 ||
        (flags & (LUME_MAP_SHARED | LUME_MAP_PRIVATE)) == (LUME_MAP_SHARED | LUME_MAP_PRIVATE) ||
        (protection & ~7u) || ((protection & 6u) == 6u && !jit) || (request->offset & (PAGE - 1)))
        return -LUME_EINVAL;
    if (protection)
        protection |= LUME_PROT_READ;
    uint64_t alignment = request->alignment ? request->alignment : PAGE;
    if (alignment < PAGE || (alignment & (alignment - 1)) ||
        alignment > LUME_VM_LIMIT - LUME_VM_BASE)
        return -LUME_EINVAL;
    bool fixed = flags & (LUME_MAP_FIXED | LUME_MAP_FIXED_NOREPLACE);
    uint64_t address = request->address;
    if (fixed && ((address & (alignment - 1)) || address < LUME_VM_BASE ||
                  address >= LUME_VM_LIMIT || length > LUME_VM_LIMIT - address))
        return -LUME_EINVAL;
    if (!fixed)
    {
        if (address < LUME_VM_BASE || address >= LUME_VM_LIMIT ||
            length > LUME_VM_LIMIT - address || (address & (alignment - 1)) ||
            !free_range(task, address, address + length))
        {
            address = (LUME_VM_BASE + alignment - 1) & ~(alignment - 1);
            for (struct vm_area *area = spaces[task]; area && address <= LUME_VM_LIMIT - length;
                 area = area->next)
                if (overlap(address, address + length, area->first, area->end))
                    address = (area->end + alignment - 1) & ~(alignment - 1);
        }
        if (address > LUME_VM_LIMIT - length)
            return -LUME_ENOMEM;
    }
    if ((flags & LUME_MAP_FIXED_NOREPLACE) && !free_range(task, address, address + length))
        return -LUME_EEXIST;
    struct vm_area *area = allocate(sizeof(*area));
    if (!area)
        return -LUME_ENOMEM;
    struct vm_object *object = NULL;
    unsigned maximum = 7;
    uint64_t offset = request->offset;
    if (flags & LUME_MAP_ANONYMOUS)
    {
        if (offset)
            goto invalid;
        object = new_object(length, (flags & LUME_MAP_SHARED) != 0);
    }
    else
    {
        struct vm_handle *handle = lume_vfs_external(files, (unsigned)request->fd, &handle_ops);
        if (handle && (flags & LUME_MAP_SHARED))
        {
            object = handle->object;
            if (object->changing)
            {
                lume_kernel_free(area);
                return -LUME_EBUSY;
            }
            maximum = handle->write && !(object->seals & (LUME_SEAL_WRITE | LUME_SEAL_FUTURE_WRITE))
                          ? 7u
                          : 5u;
            if ((protection & ~maximum) || offset > INT64_MAX || length > INT64_MAX - offset ||
                writable_alias(object, offset, offset + length, protection, LUME_TASK_LIMIT, 0, 0))
                goto access;
            ++object->references;
        }
        else
        {
            if (flags & LUME_MAP_SHARED)
            {
                lume_kernel_free(area);
                return -ENOTSUP;
            }
            struct lume_stat stat;
            int64_t result = lume_vfs_stat(files, (unsigned)request->fd, &stat);
            if (result < 0 || stat.kind != LUME_NODE_FILE || offset > INT64_MAX)
            {
                lume_kernel_free(area);
                return result < 0 ? result : -LUME_EINVAL;
            }
            uint64_t available = offset < stat.size ? stat.size - offset : 0;
            if (available > length)
                available = length;
            object = new_object(available, false);
            if (object)
                object->discardable = false;
            if (!object)
                goto memory;
            int held = lume_vfs_hold(files, (unsigned)request->fd);
            if (held < 0)
            {
                drop(object);
                lume_kernel_free(area);
                return held;
            }
            for (uint64_t position = 0; position < available; position += PAGE)
            {
                uint64_t page = get_page(object, position / PAGE);
                unsigned bytes =
                    available - position > PAGE ? PAGE : (unsigned)(available - position);
                result = page ? lume_vfs_pread_object(held, offset + position,
                                                      (void *)(uintptr_t)page, bytes)
                              : -LUME_ENOMEM;
                if (result != bytes)
                {
                    lume_vfs_drop(held);
                    drop(object);
                    lume_kernel_free(area);
                    return result < 0 ? result : -LUME_EIO;
                }
                lume_worker_checkpoint(PAGE);
            }
            lume_vfs_drop(held);
            offset = 0;
        }
    }
    if (!object)
        goto memory;
    for (struct vm_area *old = spaces[task]; old && old->first < address + length; old = old->next)
        if (old->end > address && old->object->changing)
        {
            drop(object);
            lume_kernel_free(area);
            return -LUME_EBUSY;
        }
    if (!boundaries(task, address, address + length))
    {
        drop(object);
        goto memory;
    }
    *area = (struct vm_area){.first = address,
                             .end = address + length,
                             .offset = offset,
                             .protection = protection,
                             .maximum = maximum,
                             .shared = (flags & LUME_MAP_SHARED) != 0,
                             .object = object,
                             .jit = jit};
    changing[task] = true;
    remove_range(task, address, address + length);
    insert(task, area);
    changing[task] = false;
    request->address = address;
    return (int64_t)address;
access:
    lume_kernel_free(area);
    return -LUME_EACCES;
invalid:
    lume_kernel_free(area);
    return -LUME_EINVAL;
memory:
    lume_kernel_free(area);
    return -LUME_ENOMEM;
}

struct cache_batch
{
    struct
    {
        uint64_t physical;
        size_t bytes;
    } pages[LUME_MMU_BATCH_PAGES];

    unsigned count;
};

static void clean_cache(void *context)
{
    const struct cache_batch *batch = context;
    for (unsigned i = 0; i < batch->count; ++i)
        lume_sync_code_defer(batch->pages[i].physical, batch->pages[i].bytes);
    lume_sync_code_clean_finish();
}

static int64_t sync_cache(unsigned task, uint64_t first, uint64_t bytes)
{
    if (first > UINT64_MAX - bytes)
        return -LUME_EINVAL;
    uint64_t end = first + bytes;
    while (first < end)
    {
        struct cache_batch batch = {0};
        struct vm_object *pinned[LUME_MMU_BATCH_PAGES];
        unsigned objects = 0;
        while (first < end && batch.count < LUME_MMU_BATCH_PAGES)
        {
            size_t part = PAGE - first % PAGE;
            if (part > end - first)
                part = (size_t)(end - first);
            struct vm_area *area = find_area(task, first);
            if (area && (area->retiring || area->object->changing))
            {
                lume_sync_code_finish();
                return -LUME_EBUSY;
            }
            uint64_t physical;
            if (!lume_user_buffer(first, part, false) ||
                !lume_mmu_translate(lume_mmu_task_root(task), first, false, true, false, &physical))
            {
                lume_sync_code_finish();
                return -LUME_EFAULT;
            }
            batch.pages[batch.count].physical = physical;
            batch.pages[batch.count++].bytes = part;
            if (area)
            {
                bool known = false;
                for (unsigned i = 0; i < objects; ++i)
                    known |= pinned[i] == area->object;
                if (!known)
                    pinned[objects++] = area->object;
            }
            first += part;
        }
        for (unsigned i = 0; i < objects; ++i)
            pinned[i]->changing = true;
        lume_worker_compute(clean_cache, &batch);
        for (unsigned i = 0; i < objects; ++i)
            pinned[i]->changing = false;
        lume_worker_checkpoint(65536);
    }
    lume_sync_code_finish();
    return 0;
}

int64_t lume_vm_call(struct lume_files *files, unsigned pid, unsigned operation,
                     struct lume_vm_request *request)
{
    if (!pid || pid > LUME_TASK_LIMIT || request->version != LUME_VM_VERSION || request->reserved)
        return -LUME_EINVAL;
    unsigned task = pid - 1;
    if (changing[task])
        return -LUME_EBUSY;
    if (retired)
        (void)lume_work_submit(&reap_work);
    if (operation == LUME_VM_SYNC)
        return sync_cache(task, request->address, request->length);
    if (operation == LUME_VM_INFO)
    {
        request->address = LUME_VM_BASE;
        request->length = LUME_VM_LIMIT - LUME_VM_BASE;
        request->alignment = PAGE;
        return 0;
    }
    if (operation == LUME_VM_MAP)
        return map(files, task, request);
    if (operation == LUME_VM_UNMAP || operation == LUME_VM_PROTECT || operation == LUME_VM_ADVISE)
    {
        uint64_t first = request->address, length = rounded(request->length);
        if (!length || (first & (PAGE - 1)) || first < LUME_VM_BASE || first >= LUME_VM_LIMIT ||
            length > LUME_VM_LIMIT - first)
            return -LUME_EINVAL;
        uint64_t end = first + length;
        unsigned protection = request->protection;
        if (operation == LUME_VM_PROTECT)
        {
            if (protection & ~7u)
                return -LUME_EACCES;
            if (protection)
                protection |= LUME_PROT_READ;
            uint64_t position = first;
            while (position < end)
            {
                struct vm_area *area = find_area(task, position);
                if (!area)
                    return -LUME_ENOMEM;
                if (area->object->changing)
                    return -LUME_EBUSY;
                if (((protection & 6u) == 6u && !area->jit) || (protection & ~area->maximum) ||
                    ((protection & LUME_PROT_WRITE) && area->shared &&
                     (area->object->seals & LUME_SEAL_WRITE)) ||
                    writable_alias(area->object, area->offset + position - area->first,
                                   area->offset + (area->end < end ? area->end : end) - area->first,
                                   protection, task, first, end))
                    return -LUME_EACCES;
                position = area->end;
            }
        }
        if (operation == LUME_VM_ADVISE)
        {
            for (uint64_t position = first; position < end;)
            {
                struct vm_area *area = find_area(task, position);
                if (!area)
                    return -LUME_ENOMEM;
                position = area->end;
            }
            if (request->advice <= LUME_MADV_WILLNEED)
                return 0;
            if (request->advice != LUME_MADV_DONTNEED && request->advice != LUME_MADV_FREE)
                return -ENOTSUP;
            for (uint64_t position = first; position < end;)
            {
                struct vm_area *area = find_area(task, position);
                if (!area)
                    return -LUME_ENOMEM;
                if (area->shared || area->object->persistent || !area->object->discardable)
                    return -ENOTSUP;
                position = area->end;
            }
        }
        for (struct vm_area *area = spaces[task]; area && area->first < end; area = area->next)
            if (area->end > first && area->object->changing)
                return -LUME_EBUSY;
        if (!boundaries(task, first, end))
            return -LUME_ENOMEM;
        changing[task] = true;
        if (operation == LUME_VM_UNMAP)
        {
            remove_range(task, first, end);
            changing[task] = false;
            return 0;
        }
        for (struct vm_area *area = spaces[task]; area; area = area->next)
            if (area->first >= first && area->end <= end)
            {
                area->saved = area->protection;
                area->protection = 0;
                area->object->changing = true;
            }
        unsigned discarded = 0;
        for (struct vm_area *area = spaces[task]; area; area = area->next)
            if (area->first >= first && area->end <= end)
            {
                revoke(task, area, area->offset, area->offset + area->end - area->first);
                if (operation == LUME_VM_ADVISE)
                    for (unsigned bucket = 0; bucket < HASH; ++bucket)
                        for (struct vm_chunk *entry = area->object->chunks[bucket]; entry;
                             entry = entry->next)
                            for (unsigned i = 0; i < 512; ++i)
                            {
                                uint64_t offset = (entry->index * 512 + i) * PAGE;
                                if (entry->pages[i] && offset >= area->offset &&
                                    offset - area->offset < area->end - area->first)
                                {
                                    if (!lume_page_free(entry->pages[i]))
                                        lume_fail("virtual memory discard release");
                                    entry->pages[i] = 0;
                                    if (++discarded % 16 == 0)
                                        lume_worker_checkpoint(65536);
                                }
                            }
            }
        if (operation == LUME_VM_ADVISE)
            for (struct vm_area *area = spaces[task]; area; area = area->next)
                if (area->first >= first && area->end <= end)
                    prune_chunks(area->object);
        for (struct vm_area *area = spaces[task]; area; area = area->next)
            if (area->first >= first && area->end <= end)
                area->protection = operation == LUME_VM_PROTECT ? protection : area->saved;
        for (struct vm_area *area = spaces[task]; area; area = area->next)
            if (area->first >= first && area->end <= end)
                area->object->changing = false;
        changing[task] = false;
        return 0;
    }
    if (operation == LUME_VM_CREATE || operation == LUME_VM_OPEN || operation == LUME_VM_UNLINK)
    {
        if (!memchr(request->name, 0, sizeof(request->name)))
            return -LUME_EINVAL;
        if (operation != LUME_VM_CREATE &&
            (request->name[0] != '/' || !request->name[1] || strchr(request->name + 1, '/')))
            return -LUME_EINVAL;
        struct vm_object **position = &names;
        while (*position && strcmp((*position)->name, request->name))
            position = &(*position)->next;
        if (operation == LUME_VM_UNLINK)
        {
            if (!*position)
                return -LUME_ENOENT;
            struct vm_object *object = *position;
            if (files->identity.euid && files->identity.euid != object->uid)
                return -LUME_EACCES;
            *position = object->next;
            object->next = NULL;
            drop(object);
            return 0;
        }
        unsigned flags = request->flags;
        struct vm_object *object = operation == LUME_VM_OPEN ? *position : NULL;
        bool fresh = !object;
        if (operation == LUME_VM_OPEN &&
            (((flags & 3) != LUME_O_RDONLY && (flags & 3) != LUME_O_RDWR) ||
             (flags & ~(LUME_O_CREAT | LUME_O_EXCL | LUME_O_TRUNC | 3u))))
            return -LUME_EINVAL;
        if (operation == LUME_VM_OPEN && (flags & LUME_O_TRUNC) && (flags & 3) != LUME_O_RDWR)
            return -LUME_EACCES;
        if (object && files->identity.euid)
        {
            unsigned shift = files->identity.euid == object->uid ? 6 : 0;
            if (!shift)
            {
                bool group = files->identity.egid == object->gid;
                for (unsigned i = 0; i < files->identity.groups_count; ++i)
                    group |= files->identity.groups[i] == object->gid;
                if (group)
                    shift = 3;
            }
            unsigned permission = (object->mode >> shift) & 7;
            if (!(permission & 4) || ((flags & 3) == LUME_O_RDWR && !(permission & 2)))
                return -LUME_EACCES;
        }
        if (operation == LUME_VM_CREATE &&
            (flags & ~(LUME_MEMFD_CLOEXEC | LUME_MEMFD_ALLOW_SEALING)))
            return -LUME_EINVAL;
        if (object && (flags & LUME_O_CREAT) && (flags & LUME_O_EXCL))
            return -LUME_EEXIST;
        if (!object && operation == LUME_VM_OPEN && !(flags & LUME_O_CREAT))
            return -LUME_ENOENT;
        if (!object)
        {
            object = new_object(0, true);
            if (!object)
                return -LUME_ENOMEM;
            object->uid = files->identity.euid;
            object->gid = files->identity.egid;
            object->mode = request->advice & 0777u & ~files->identity.umask;
            object->seals = operation == LUME_VM_CREATE && (flags & LUME_MEMFD_ALLOW_SEALING)
                                ? 0
                                : LUME_SEAL_SEAL;
        }
        bool write = operation == LUME_VM_CREATE || (flags & 3) == LUME_O_RDWR;
        int result =
            attach(files, object, write, operation == LUME_VM_CREATE ? flags : LUME_MEMFD_CLOEXEC);
        if (result >= 0 && fresh && operation == LUME_VM_OPEN)
        {
            memcpy(object->name, request->name, sizeof(object->name));
            object->next = names;
            names = object;
        }
        else if (fresh)
            drop(object);
        if (result >= 0 && (flags & LUME_O_TRUNC) && operation == LUME_VM_OPEN)
        {
            int64_t error = write ? resize_object(object, 0) : -LUME_EACCES;
            if (error)
            {
                (void)lume_vfs_close(files, (unsigned)result);
                return error;
            }
        }
        return result;
    }
    struct vm_handle *handle = lume_vfs_external(files, (unsigned)request->fd, &handle_ops);
    if (!handle)
        return -LUME_EBADF;
    if (handle->object->changing)
        return -LUME_EBUSY;
    if (operation == LUME_VM_RESIZE)
        return handle_resize(handle, request->length);
    if (operation == LUME_VM_DUPLICATE)
    {
        if ((request->flags & ~LUME_MEMFD_CLOEXEC) ||
            (request->protection != LUME_PROT_READ &&
             request->protection != (LUME_PROT_READ | LUME_PROT_WRITE)))
            return -LUME_EINVAL;
        bool write = (request->protection & LUME_PROT_WRITE) != 0;
        return write && !handle->write ? -LUME_EACCES
                                       : attach(files, handle->object, write, request->flags);
    }
    if (operation == LUME_VM_GET_SEALS)
        return handle->object->seals;
    if (operation == LUME_VM_ADD_SEALS)
    {
        unsigned seals = request->flags;
        if (!handle->write || (handle->object->seals & LUME_SEAL_SEAL))
            return -LUME_EPERM;
        if (seals & ~31u)
            return -LUME_EINVAL;
        if ((seals & LUME_SEAL_WRITE) &&
            writable_alias(handle->object, 0, UINT64_MAX, LUME_PROT_EXEC, LUME_TASK_LIMIT, 0, 0))
            return -LUME_EBUSY;
        handle->object->seals |= seals;
        return 0;
    }
    return -LUME_EINVAL;
}

void lume_vm_release(unsigned task)
{
    if (task < LUME_TASK_LIMIT)
    {
        for (;;)
        {
            bool busy = false;
            for (struct vm_area *area = spaces[task]; area; area = area->next)
                busy |= area->object->changing;
            if (!busy)
                break;
            if (!lume_worker_pause_ms(1))
                lume_fail("virtual memory retirement busy");
        }
        changing[task] = true;
        remove_range(task, 0, UINT64_MAX);
        changing[task] = false;
        while (retired)
        {
            reap(NULL);
            lume_worker_checkpoint(65536);
        }
    }
}

void lume_vm_swap(unsigned a, unsigned b)
{
    struct vm_area *areas = spaces[a];
    spaces[a] = spaces[b];
    spaces[b] = areas;
    for (struct vm_area *area = spaces[a]; area; area = area->next)
        area->owner = a;
    for (struct vm_area *area = spaces[b]; area; area = area->next)
        area->owner = b;
}

bool lume_vm_clone(unsigned source, unsigned target)
{
    if (source >= LUME_TASK_LIMIT || target >= LUME_TASK_LIMIT || source == target ||
        spaces[target])
        return false;
    struct vm_area **tail = &spaces[target];
    for (struct vm_area *area = spaces[source]; area; area = area->next)
    {
        if (area->retiring)
            continue;
        struct vm_area *copy = allocate(sizeof(*copy));
        if (!copy)
            goto fail;
        *copy = *area;
        copy->next = NULL;
        if (area->shared)
        {
            if (copy->object->changing)
            {
                lume_kernel_free(copy);
                goto fail;
            }
            ++copy->object->references;
        }
        else
        {
            uint64_t length = area->end - area->first;
            uint64_t available =
                area->offset < area->object->bytes ? area->object->bytes - area->offset : 0;
            if (available > length)
                available = length;
            copy->object = new_object(available, false);
            if (copy->object)
                copy->object->discardable = area->object->discardable;
            copy->offset = 0;
            if (!copy->object || !copy_object(area->object, area->offset, length, copy->object))
            {
                if (copy->object)
                    drop(copy->object);
                lume_kernel_free(copy);
                goto fail;
            }
        }
        copy->owner = target;
        copy->object_next = copy->object->areas;
        copy->object->areas = copy;
        *tail = copy;
        tail = &copy->next;
        lume_worker_checkpoint(8192);
    }
    return true;
fail:
    lume_vm_release(target);
    return false;
}

bool lume_vm_thread_region(unsigned owner, unsigned task, uint64_t tls_bytes, uint64_t stack_bytes,
                           uint64_t *tls_base, uint64_t *stack_base)
{
    uint64_t tls = rounded(tls_bytes), stack = rounded(stack_bytes);
    if (owner >= LUME_TASK_LIMIT || task >= LUME_TASK_LIMIT || !tls || !stack || tls > 17 * PAGE ||
        stack > LUME_THREAD_STACK_MAX || tls + PAGE + stack > LUME_THREAD_STRIDE - PAGE)
        return false;
    uint64_t base = LUME_THREAD_VM_BASE + (uint64_t)task * LUME_THREAD_STRIDE + PAGE;
    struct vm_area *a = allocate(sizeof(*a)), *b = allocate(sizeof(*b));
    struct vm_object *ta = new_object(tls, false), *sa = new_object(stack, false);
    if (!a || !b || !ta || !sa || !free_range(owner, base, base + tls + PAGE + stack))
    {
        lume_kernel_free(a);
        lume_kernel_free(b);
        if (ta)
            drop(ta);
        if (sa)
            drop(sa);
        return false;
    }
    *a = (struct vm_area){.first = base,
                          .end = base + tls,
                          .protection = 3,
                          .maximum = 3,
                          .thread = task + 1,
                          .object = ta};
    *b = (struct vm_area){.first = base + tls + PAGE,
                          .end = base + tls + PAGE + stack,
                          .protection = 3,
                          .maximum = 3,
                          .thread = task + 1,
                          .object = sa};
    insert(owner, a);
    insert(owner, b);
    *tls_base = a->first;
    *stack_base = b->first;
    return true;
}

/* Retired stack VMAs reserve their slot until the bounded reclaimer removes every PTE. */
void lume_vm_thread_release(unsigned owner, unsigned task)
{
    if (owner >= LUME_TASK_LIMIT || task >= LUME_TASK_LIMIT)
        return;
    for (struct vm_area *area = spaces[owner]; area; area = area->next)
        if (area->thread == task + 1 && !area->retiring)
        {
            area->retiring = true;
            area->protection = 0;
            area->retire_next = NULL;
            if (retired_threads_tail)
                retired_threads_tail->retire_next = area;
            else
                retired_threads = area;
            retired_threads_tail = area;
        }
    if (retired_threads)
        (void)lume_work_submit(&thread_reap_work);
}

static void reap_threads(void *unused)
{
    (void)unused;
    struct vm_area *area = retired_threads;
    if (!area)
        return;
    struct vm_object *object = area->object;
    if (object->changing)
    {
        (void)lume_work_submit(&thread_reap_work);
        return;
    }
    object->changing = true;
    uint64_t addresses[LUME_MMU_BATCH_PAGES], *frames[LUME_MMU_BATCH_PAGES];
    unsigned mapped = 0, count = 0;
    bool more = false;
    for (unsigned bucket = 0; bucket < HASH; ++bucket)
        for (struct vm_chunk *entry = object->chunks[bucket]; entry; entry = entry->next)
            for (unsigned i = 0; i < 512; ++i)
            {
                if (!entry->pages[i])
                    continue;
                if (count == LUME_MMU_BATCH_PAGES)
                {
                    more = true;
                    continue;
                }
                uint64_t address = area->first + (entry->index * 512 + i) * PAGE;
                uint64_t physical;
                if (lume_mmu_translate(lume_mmu_task_root(area->owner), address, false, true, false,
                                       &physical))
                    addresses[mapped++] = address;
                frames[count++] = &entry->pages[i];
            }
    if (mapped && !lume_mmu_unmap_user_batch(area->owner, addresses, mapped))
        lume_fail("thread virtual mapping release");
    for (unsigned i = 0; i < count; ++i)
    {
        if (!lume_page_free(*frames[i]))
            lume_fail("thread virtual frame release");
        *frames[i] = 0;
    }
    object->changing = false;
    if (!more)
    {
        struct vm_area **position = &spaces[area->owner];
        while (*position && *position != area)
            position = &(*position)->next;
        if (!*position)
            lume_fail("thread virtual mapping ownership");
        *position = area->next;
        struct vm_area **alias = &object->areas;
        while (*alias && *alias != area)
            alias = &(*alias)->object_next;
        if (!*alias)
            lume_fail("thread virtual alias ownership");
        *alias = area->object_next;
        forget_retiring(area);
        drop(object);
        lume_kernel_free(area);
    }
    if (retired_threads)
        (void)lume_work_submit(&thread_reap_work);
}

bool lume_vm_thread_bounds(unsigned owner, unsigned task, uint64_t *base, uint64_t *bytes)
{
    if (owner >= LUME_TASK_LIMIT || task >= LUME_TASK_LIMIT)
        return false;
    struct vm_area *stack = NULL;
    for (struct vm_area *area = spaces[owner]; area; area = area->next)
        if (area->thread == task + 1 && !area->retiring)
            stack = area;
    if (!stack)
        return false;
    *base = stack->first;
    *bytes = stack->end - stack->first;
    return true;
}
