#include "lume/vm.h"
#include "lume/vm_dma.h"
#include "lume/vfs.h"
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
    uint64_t code_ready[8];
    struct vm_chunk *next;
};

struct vm_area;

struct vm_object
{
    uint64_t bytes, serial;
    unsigned references, seals, dma_pins, dma_writers;
    bool persistent, changing, discardable;
    unsigned uid, gid, mode;
    struct vm_chunk *chunks[HASH];
    struct vm_object *next;
    struct vm_area *areas;
    char name[LUME_VM_NAME_BYTES];
    struct vm_object *backing, *children, *backing_next, *file_next;
    uint64_t backing_offset, file_first, file_end;
    uint64_t file_key, file_version, file_age, io_page, io_error_page;
    int file, io_error;
    volatile uint32_t io_ready;
    bool cached, file_limited, loading, io_running;
    struct lume_work file_work;
};

struct vm_area
{
    uint64_t first, end, offset, generation;
    unsigned protection, maximum, thread, saved, owner;
    bool shared, retiring, jit, image;
    struct vm_object *object;
    struct vm_area *next, *object_next, *retire_next;
};

struct vm_handle
{
    struct vm_object *object;
    uint64_t offset;
    bool write;
};

struct lume_vm_dma_pin
{
    struct vm_object *object;
    uint64_t serial, offset, bytes;
    bool write;
    uint64_t pages[];
};

static struct vm_area *spaces[LUME_TASK_LIMIT];
static struct vm_object *names, *retired;
static struct vm_object *file_cache;
static uint64_t cached_pages, file_age;
static unsigned file_waiters;
static uint64_t serial, area_serial;
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
        struct vm_chunk *prepared = allocate(sizeof(*prepared));
        if (prepared)
        {
            /* Another client can prepare the same chunk while allocation suspends. */
            entry = chunk(object, page, false);
            if (entry)
                lume_kernel_free(prepared);
            else
            {
                prepared->index = index;
                prepared->next = object->chunks[bucket];
                object->chunks[bucket] = prepared;
                if (object->cached)
                    cached_pages += lume_kernel_allocation_pages(prepared);
                entry = prepared;
            }
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
    if (serial >= UINT64_MAX - UINT64_C(0x40000000))
        return NULL;
    uint64_t identity = UINT64_C(0x40000000) + ++serial;
    struct vm_object *object = allocate(sizeof(*object));
    if (object)
    {
        object->bytes = bytes;
        object->persistent = persistent;
        object->discardable = true;
        object->serial = identity;
        object->references = 1;
    }
    return object;
}

static void drop(struct vm_object *object)
{
    if (--object->references)
        return;
    if (object->backing)
    {
        struct vm_object **position = &object->backing->children;
        while (*position && *position != object)
            position = &(*position)->backing_next;
        if (!*position)
            lume_fail("virtual memory backing ownership");
        *position = object->backing_next;
        object->backing_next = NULL;
    }
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
                        if (object->cached)
                            --cached_pages;
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
                if (object->cached)
                    cached_pages -= lume_kernel_allocation_pages(entry);
                lume_kernel_free(entry);
                budget -= 2;
            }
        for (unsigned bucket = 0; bucket < HASH; ++bucket)
            empty &= object->chunks[bucket] == NULL;
        if (!empty || !budget)
            break;
        retired = object->next;
        struct vm_object *backing = object->backing;
        if (object->file)
            lume_vfs_drop(object->file - 1);
        if (object->cached)
            cached_pages -= lume_kernel_allocation_pages(object);
        lume_kernel_free(object);
        if (backing)
            drop(backing);
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

int lume_vm_shared_word_key(unsigned task, uint64_t address, struct lume_vm_word_key *key)
{
    if (task >= LUME_TASK_LIMIT || !key || (address & 3))
        return -LUME_EINVAL;
    if (changing[task])
        return -LUME_EAGAIN;
    struct vm_area *area = find_area(task, address);
    if (!area || area->end - address < sizeof(uint32_t) || !(area->protection & LUME_PROT_READ))
        return -LUME_EFAULT;
    struct vm_object *object = area->object;
    if (area->retiring || object->changing)
        return -LUME_EAGAIN;
    if (!area->shared || object->file || object->backing)
        return -LUME_EOPNOTSUPP;
    uint64_t offset = area->offset + address - area->first;
    if (offset > object->bytes || object->bytes - offset < sizeof(uint32_t))
        return -LUME_EFAULT;
    *key = (struct lume_vm_word_key){object->serial, offset};
    return 0;
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
    if (object->children)
        return true;
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
                        lume_worker_checkpoint(0);
                }
    prune_chunks(object);
}

static void area_changed(struct vm_area *area)
{
    if (!++area_serial)
        lume_fail("virtual memory area generation exhausted");
    area->generation = area_serial;
}

static void insert(unsigned task, struct vm_area *area)
{
    struct vm_area **position = &spaces[task];
    while (*position && (*position)->first < area->first)
        position = &(*position)->next;
    area->next = *position;
    *position = area;
    area->owner = task;
    area_changed(area);
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
        area_changed(a);
        area_changed(split_a);
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
        area_changed(z);
        area_changed(split_z);
        z->next = split_z;
        ++z->object->references;
        split_z->object_next = z->object->areas;
        z->object->areas = split_z;
    }
    return true;
}

static void revoke_object(unsigned task, struct vm_area *area, struct vm_object *object,
                          uint64_t source_offset, uint64_t offset, uint64_t end)
{
    uint64_t addresses[LUME_MMU_UNMAP_PAGES];
    unsigned count = 0, visited = 0;
    for (unsigned bucket = 0; bucket < HASH; ++bucket)
        for (struct vm_chunk *entry = object->chunks[bucket]; entry; entry = entry->next)
            for (unsigned i = 0; i < 512; ++i)
            {
                if ((++visited & 511) == 0)
                    lume_worker_checkpoint(PAGE);
                uint64_t source = (entry->index * 512 + i) * PAGE, physical;
                if (source < source_offset)
                    continue;
                uint64_t position = source - source_offset;
                if (!entry->pages[i] || position < offset || position >= end ||
                    position < area->offset || position - area->offset >= area->end - area->first)
                    continue;
                uint64_t address = area->first + position - area->offset;
                if (!lume_mmu_translate(lume_mmu_task_root(task), address, false, true, false,
                                        &physical))
                    continue;
                addresses[count++] = address;
                if (count == LUME_MMU_UNMAP_PAGES)
                {
                    if (!lume_mmu_unmap_user_batch(task, addresses, count))
                        lume_fail("virtual memory mapping release");
                    count = 0;
                    lume_worker_checkpoint(0);
                }
            }
    if (count && !lume_mmu_unmap_user_batch(task, addresses, count))
        lume_fail("virtual memory mapping release");
    lume_worker_checkpoint(PAGE);
}

static void revoke(unsigned task, struct vm_area *area, uint64_t offset, uint64_t end)
{
    revoke_object(task, area, area->object, 0, offset, end);
    if (area->object->backing)
        revoke_object(task, area, area->object->backing, area->object->backing_offset, offset, end);
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

#include "vm_file_cache.h"

static void file_map_around(unsigned task, const struct vm_area *area, uint64_t page)
{
    struct vm_object *object = area->object, *source = object->backing;
    if (!source || !source->cached || area->jit)
        return;
    uint64_t first = object->file_limited ? object->file_first : 0;
    uint64_t end = object->file_limited                     ? object->file_end
                   : source->bytes > object->backing_offset ? source->bytes - object->backing_offset
                                                            : 0;
    uint64_t source_first = object->backing_offset / PAGE;
    uint64_t group_end = (source_first + page + FILE_READ_PAGES) & ~(uint64_t)(FILE_READ_PAGES - 1);
    uint64_t prefix = (source_first + page) % FILE_READ_PAGES;
    uint64_t group_first = page > prefix ? page - prefix : 0;
    uint64_t addresses[FILE_READ_PAGES], frames[FILE_READ_PAGES];
    unsigned count = 0;
    bool cleaned = false;
    for (uint64_t next = group_first; source_first + next < group_end; ++next)
    {
        uint64_t offset = next * PAGE, existing;
        if (offset < first || offset < area->offset)
            continue;
        if (offset + PAGE > end || offset - area->offset >= area->end - area->first)
            break;
        uint64_t address = area->first + offset - area->offset;
        struct vm_chunk *entry = chunk(source, source_first + next, false);
        unsigned index = (unsigned)((source_first + next) % 512);
        uint64_t physical = entry ? entry->pages[index] : 0;
        uint64_t *owned = page_entry(object, next, false);
        if (!physical || (owned && *owned) ||
            lume_mmu_translate(lume_mmu_task_root(task), address, false, false, false, &existing))
            continue;
        if ((area->protection & LUME_PROT_EXEC) &&
            !(entry->code_ready[index / 64] & (UINT64_C(1) << (index % 64))))
        {
            lume_sync_code_defer(physical, PAGE);
            entry->code_ready[index / 64] |= UINT64_C(1) << (index % 64);
            cleaned = true;
        }
        addresses[count] = address;
        frames[count++] = physical;
    }
    if (cleaned)
    {
        lume_sync_code_clean_finish();
        lume_sync_code_finish();
    }
    for (unsigned i = 0; i < count; ++i)
        if (!lume_mmu_map_user(task, addresses[i], frames[i], false,
                               (area->protection & LUME_PROT_EXEC) != 0))
            break;
}

int lume_vm_fault(unsigned task, uint64_t address, bool write, bool execute)
{
    if (task >= LUME_TASK_LIMIT)
        return 0;
    struct vm_area *area = find_area(task, address);
    unsigned required = execute ? LUME_PROT_EXEC : write ? LUME_PROT_WRITE : LUME_PROT_READ;
    if (!area || area->retiring)
        return 0;
    struct vm_object *object = area->object;
    if (changing[task] || object->changing || (object->backing && object->backing->changing))
        return 2;
    if (!(area->protection & required))
        return 0;
    bool writer = false;
#if defined(__aarch64__) || defined(LUME_ARCH_AARCH64) || defined(LUME_ARCH_AMD64) ||              \
    defined(LUME_ARCH_I686)
    writer = lume_mmu_jit_active(task);
#endif
    if (area->jit &&
        ((execute && writer) || (write && !writer && (area->protection & LUME_PROT_EXEC))))
        return 0;
    uint64_t offset = area->offset + address - area->first;
    uint64_t page = offset / PAGE, position = page * PAGE;
    if (page >= (object->bytes + PAGE - 1) / PAGE)
        return -1;
    uint64_t first = 0, end = 0, source_physical = 0, source_version = 0;
    struct vm_object *source = object->backing;
    if (source)
    {
        first = object->file_limited ? object->file_first : 0;
        end = object->file_limited                     ? object->file_end
              : source->bytes > object->backing_offset ? source->bytes - object->backing_offset
                                                       : 0;
        if (!object->file_limited && page >= (end + PAGE - 1) / PAGE)
            return -1;
    }
    uint64_t generation = area->generation, root = lume_mmu_task_root(task);
#if defined(__aarch64__) || defined(LUME_ARCH_AARCH64) || defined(LUME_ARCH_AMD64) ||              \
    defined(LUME_ARCH_I686)
    root = lume_mmu_task_view_root(task, writer);
#endif
    ++object->references;
    struct vm_chunk *prepared = NULL;
    struct vm_chunk *entry = chunk(object, page, false);
    bool owned = entry && entry->pages[page % 512];
    bool partial = source && (position < first || end < position + PAGE);
    bool private = owned || !source || write || partial ||
                   (source && !source->cached && (area->protection & LUME_PROT_EXEC));
    int result = 0;
    if (!owned && source && position < end && first < position + PAGE)
    {
        result = backing_page(object, page, &source_physical);
        if (result != 1)
            goto out;
        source_version = source->file_version;
    }
    if (private && !entry)
        prepared = allocate(sizeof(*prepared));
    area = find_area(task, address);
    if (!area || area->generation != generation || area->object != object || area->retiring ||
        !(area->protection & required))
    {
        result = 0;
        goto out;
    }
    if (changing[task] || object->changing || (source && source->changing))
    {
        result = 2;
        goto out;
    }
    uint64_t current_root = lume_mmu_task_root(task);
#if defined(__aarch64__) || defined(LUME_ARCH_AARCH64) || defined(LUME_ARCH_AMD64) ||              \
    defined(LUME_ARCH_I686)
    if (lume_mmu_jit_active(task) != writer)
    {
        result = 2;
        goto out;
    }
    current_root = lume_mmu_task_view_root(task, writer);
#endif
    if (current_root != root)
    {
        result = 2;
        goto out;
    }
    if (page >= (object->bytes + PAGE - 1) / PAGE)
    {
        result = -1;
        goto out;
    }
    if (source && !object->file_limited &&
        position >= rounded(source->bytes > object->backing_offset
                                ? source->bytes - object->backing_offset
                                : 0))
    {
        result = -1;
        goto out;
    }
    if (source_physical)
    {
        uint64_t source_page = object->backing_offset / PAGE + page;
        struct vm_chunk *backing = chunk(source, source_page, false);
        if (!backing || backing->pages[source_page % 512] != source_physical ||
            source_version != source->file_version)
        {
            result = 2;
            goto out;
        }
    }
    entry = chunk(object, page, false);
    if (entry && entry->pages[page % 512])
        private = true;
    bool new_chunk = private && !entry;
    if (new_chunk)
        entry = prepared;
    if (private && !entry)
    {
        result = -1;
        goto out;
    }
    unsigned slot = (unsigned)(page % 512), bucket = (unsigned)((page / 512) % HASH);
    bool new_frame = private && !entry->pages[slot];
    uint64_t physical = private ? entry->pages[slot] : source_physical;
    if (new_frame)
    {
        physical =
            source_physical && !partial ? lume_page_alloc_uninitialized(0) : lume_page_alloc(0);
        if (!physical)
        {
            result = -1;
            goto out;
        }
        if (source_physical)
        {
            uint64_t low = first > position ? first - position : 0;
            uint64_t high = end - position < PAGE ? end - position : PAGE;
            if (high > low)
                memcpy((void *)(uintptr_t)(physical + low),
                       (const void *)(uintptr_t)(source_physical + low), (size_t)(high - low));
        }
        entry->pages[slot] = physical;
    }
    if (new_chunk)
    {
        entry->index = page / 512;
        entry->next = object->chunks[bucket];
        object->chunks[bucket] = entry;
        prepared = NULL;
    }
    if (!physical)
    {
        result = -1;
        goto out;
    }
    uint64_t existing;
    if (lume_mmu_translate(root, address, write, true, execute, &existing) &&
        (existing & ~(PAGE - 1)) == physical)
    {
        result = 1;
        goto out;
    }
    if (area->protection & LUME_PROT_EXEC)
    {
        bool clean = false;
        struct vm_chunk *backing = NULL;
        unsigned bit = 0;
        if (!private)
        {
            uint64_t source_page = object->backing_offset / PAGE + page;
            backing = chunk(source, source_page, false);
            bit = (unsigned)(source_page % 512);
            clean = backing && (backing->code_ready[bit / 64] & (UINT64_C(1) << (bit % 64)));
        }
        if (!clean)
            lume_sync_code(physical, PAGE);
        if (backing)
            backing->code_ready[bit / 64] |= UINT64_C(1) << (bit % 64);
    }
    uint64_t aligned = address & ~(PAGE - 1);
    if (lume_mmu_translate(root, aligned, false, false, false, &existing))
        if (!lume_mmu_unmap_user_batch(task, &aligned, 1))
            lume_fail("copy-on-write mapping replacement");
    bool mapped;
#if defined(__aarch64__) || defined(LUME_ARCH_AARCH64) || defined(LUME_ARCH_AMD64) ||              \
    defined(LUME_ARCH_I686)
    if (area->jit)
        mapped = lume_mmu_map_user_jit(task, aligned, physical,
                                       private && (area->protection & LUME_PROT_WRITE),
                                       (area->protection & LUME_PROT_EXEC) != 0);
    else
#endif
        mapped = lume_mmu_map_user(task, aligned, physical,
                                   private && (area->protection & LUME_PROT_WRITE),
                                   (area->protection & LUME_PROT_EXEC) != 0);
    result = mapped ? 1 : -1;
    if (mapped && !private)
        file_map_around(task, area, page);
    if (!mapped && new_frame)
    {
        entry->pages[slot] = 0;
        if (!lume_page_free(physical))
            lume_fail("virtual memory speculative frame release");
        if (new_chunk)
        {
            object->chunks[bucket] = entry->next;
            lume_kernel_free(entry);
        }
    }
out:
    lume_kernel_free(prepared);
    drop(object);
    return result;
}

bool lume_vm_resolve_buffer(uint64_t root, uint64_t address, bool write)
{
    for (unsigned task = 0; task < LUME_TASK_LIMIT; ++task)
        if (spaces[task] &&
#if defined(__aarch64__) || defined(LUME_ARCH_AARCH64) || defined(LUME_ARCH_AMD64) ||              \
    defined(LUME_ARCH_I686)
            lume_mmu_root_matches(task, root)
#else
            lume_mmu_task_root(task) == root
#endif
        )
        {
            for (;;)
            {
                int result = lume_vm_fault(task, address, write, false);
                if (result != 2)
                    return result == 1;
                if (lume_worker_current() >= LUME_WORK_DOMAINS)
                {
                    lume_task_vm_buffer_wait();
                    return false;
                }
                ++file_waiters;
                bool paused = lume_worker_pause_ms(1);
                --file_waiters;
                if (!paused)
                    return false;
            }
        }
    return false;
}

bool lume_vm_code_address(unsigned task, uint64_t address)
{
    if (task >= LUME_TASK_LIMIT)
        return false;
    struct vm_area *area = find_area(task, address);
    if (area)
    {
        if (area->retiring || !(area->protection & LUME_PROT_EXEC))
            return false;
#if defined(__aarch64__) || defined(LUME_ARCH_AARCH64) || defined(LUME_ARCH_AMD64) ||              \
    defined(LUME_ARCH_I686)
        if (area->jit && lume_mmu_jit_active(task))
            return false;
#endif
        return true;
    }
    uint64_t physical;
    return lume_mmu_translate(lume_mmu_task_root(task), address, false, true, true, &physical);
}

bool lume_vm_file_pointer(unsigned task, uint64_t address)
{
    struct vm_area *area = task < LUME_TASK_LIMIT ? find_area(task, address) : NULL;
    return area && area->object->backing;
}

bool lume_vm_buffer_busy(unsigned task, uint64_t address, uint64_t bytes)
{
    if (task >= LUME_TASK_LIMIT || !bytes || address > UINT64_MAX - bytes)
        return false;
    if (changing[task])
        return true;
    uint64_t end = address + bytes;
    for (struct vm_area *area = spaces[task]; area && area->first < end; area = area->next)
        if (area->end > address)
        {
            struct vm_object *object = area->object;
            if (object->changing || (object->backing && object->backing->changing))
                return true;
            if (object->backing && object->backing->cached)
            {
                uint64_t first = address > area->first ? address : area->first;
                uint64_t last = end < area->end ? end : area->end;
                for (uint64_t at = first & ~(PAGE - 1); at < last; at += PAGE)
                {
                    uint64_t page = (area->offset + at - area->first) / PAGE;
                    uint64_t *owned = page_entry(object, page, false);
                    uint64_t position = object->backing_offset + page * PAGE;
                    uint64_t *cached = page_entry(object->backing, position / PAGE, false);
                    if ((!owned || !*owned) && (!cached || !*cached) &&
                        position < object->backing->bytes &&
                        (!object->file_limited || page * PAGE < object->file_end) &&
                        file_request(object->backing, position / PAGE) == 2)
                        return true;
                }
            }
        }
    return false;
}

bool lume_vm_buffer_ready(unsigned task, uint64_t address, uint64_t bytes)
{
    if (task >= LUME_TASK_LIMIT || changing[task] || address > UINT64_MAX - bytes)
        return false;
    while (bytes)
    {
        uint64_t part = PAGE - address % PAGE;
        if (part > bytes)
            part = bytes;
        struct vm_area *area = find_area(task, address);
        if (area || lume_vm_address(address))
        {
            if (!area || area->retiring || area->object->changing ||
                area->offset + address - area->first >= rounded(area->object->bytes))
                return false;
        }
        address += part;
        bytes -= part;
    }
    return true;
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
    if (object->dma_pins && bytes != object->bytes)
        return -LUME_EBUSY;
    if (object->changing)
        return -LUME_EAGAIN;
    if (bytes > INT64_MAX || (bytes < object->bytes && (object->seals & LUME_SEAL_SHRINK)) ||
        (bytes > object->bytes && (object->seals & LUME_SEAL_GROW)))
        return -LUME_EPERM;
    if (bytes >= object->bytes)
    {
        if (bytes > object->bytes)
            zero_old_tail(object);
        if (object->bytes != bytes)
            ++object->file_version;
        object->bytes = bytes;
        return 0;
    }
    object->changing = true;
    uint64_t first = bytes & ~(PAGE - 1);
    /* Publish EOF before yielding. Faults cannot recreate truncated frames. */
    object->bytes = bytes;
    ++object->file_version;
    backing_revoke(object);
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
                        lume_worker_checkpoint(0);
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
        if (object->dma_pins)
            return -LUME_EBUSY;
        if (object->seals & LUME_SEAL_GROW)
            return -LUME_EPERM;
    }
    if (object->references == UINT32_MAX)
        return -LUME_EBUSY;
    ++object->references;
    object->changing = true;
    if (end > object->bytes)
        zero_old_tail(object);
    uint64_t done = 0;
    unsigned copied_pages = 0;
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
        if (++copied_pages % 16 == 0)
            lume_worker_checkpoint(0);
    }
    handle->offset += done;
    if (done && handle->offset > object->bytes)
        object->bytes = handle->offset;
    object->changing = false;
    drop(object);
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

static int pin_object(struct vm_object *object, uint64_t offset, uint64_t bytes, bool write,
                      struct lume_vm_dma_pin **output, struct lume_vm_dma_span *span)
{
    if (object->changing || object->dma_pins == UINT32_MAX || object->references == UINT32_MAX)
        return -LUME_EBUSY;
    if (!object->persistent || object->cached || object->backing || object->children)
        return -ENOTSUP;
    if (offset > object->bytes || bytes > object->bytes - offset)
        return -LUME_EINVAL;
    if (write && (object->seals & (LUME_SEAL_WRITE | LUME_SEAL_FUTURE_WRITE)))
        return -LUME_EACCES;
    for (struct vm_area *area = object->areas; area; area = area->object_next)
        if (area->protection & LUME_PROT_EXEC)
            return -LUME_EACCES;
    ++object->references;
    object->changing = true;
    unsigned count = (unsigned)(bytes / PAGE);
    struct lume_vm_dma_pin *pin = allocate(sizeof(*pin) + (size_t)count * sizeof(uint64_t));
    if (!pin)
        goto memory;
    pin->object = object;
    pin->serial = object->serial;
    pin->offset = offset;
    pin->bytes = bytes;
    pin->write = write;
    for (unsigned i = 0; i < count; ++i)
    {
        pin->pages[i] = get_page(object, offset / PAGE + i);
        if (!pin->pages[i])
        {
            lume_kernel_free(pin);
            goto memory;
        }
        if ((i + 1) % 16 == 0)
            lume_worker_checkpoint(0);
    }
    ++object->dma_pins;
    if (write)
        ++object->dma_writers;
    object->changing = false;
    *span = (struct lume_vm_dma_span){pin->pages, object->serial, offset, bytes, count};
    *output = pin;
    return 0;
memory:
    object->changing = false;
    drop(object);
    return -LUME_ENOMEM;
}

int lume_vm_dma_pin(struct lume_files *files, unsigned fd, uint64_t offset, uint64_t bytes,
                    bool write, struct lume_vm_dma_pin **output, struct lume_vm_dma_span *span)
{
    if (!files || !output || !span || *output || !bytes || ((offset | bytes) & (PAGE - 1)) ||
        bytes / PAGE > LUME_VM_DMA_MAX_PAGES)
        return -LUME_EINVAL;
    *span = (struct lume_vm_dma_span){0};
    struct vm_handle *handle = lume_vfs_external(files, fd, &handle_ops);
    if (!handle)
        return -LUME_EBADF;
    if (write && !handle->write)
        return -LUME_EACCES;
    return pin_object(handle->object, offset, bytes, write, output, span);
}

int lume_vm_dma_allocate(struct lume_files *files, uint64_t bytes, struct lume_vm_dma_pin **output,
                         struct lume_vm_dma_span *span)
{
    if (!files || !output || !span || *output || !bytes || (bytes & (PAGE - 1)) ||
        bytes / PAGE > LUME_VM_DMA_MAX_PAGES)
        return -LUME_EINVAL;
    *span = (struct lume_vm_dma_span){0};
    struct vm_object *object = new_object(bytes, true);
    if (!object)
        return -LUME_ENOMEM;
    object->uid = files->identity.euid;
    object->gid = files->identity.egid;
    object->mode = 0600;
    object->seals = LUME_SEAL_SEAL;
    int result = pin_object(object, 0, bytes, true, output, span);
    drop(object);
    return result;
}

int lume_vm_dma_export(struct lume_files *files, const struct lume_vm_dma_pin *pin, bool write)
{
    if (!files || !pin || pin->serial != pin->object->serial || pin->offset ||
        pin->bytes != pin->object->bytes)
        return -LUME_EINVAL;
    struct vm_object *object = pin->object;
    if (write && !pin->write)
        return -LUME_EACCES;
    if (object->changing || object->references == UINT32_MAX)
        return -LUME_EBUSY;
    return attach(files, object, write, LUME_MEMFD_CLOEXEC);
}

void lume_vm_dma_unpin(struct lume_vm_dma_pin **owner)
{
    if (!owner || !*owner)
        return;
    struct lume_vm_dma_pin *pin = *owner;
    struct vm_object *object = pin->object;
    if (pin->serial != object->serial || !object->dma_pins || (pin->write && !object->dma_writers))
        lume_fail("virtual memory DMA ownership");
    *owner = NULL;
    --object->dma_pins;
    if (pin->write)
        --object->dma_writers;
    lume_kernel_free(pin);
    drop(object);
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
                uint64_t *slot = page_entry(target, (position - offset) / PAGE, true);
                if (slot && !*slot)
                    *slot = lume_page_alloc_uninitialized(0);
                uint64_t page = slot ? *slot : 0;
                if (!page)
                    return false;
                copy.pages[copy.count].source = entry->pages[i];
                copy.pages[copy.count++].target = page;
                if (copy.count == LUME_MMU_BATCH_PAGES)
                {
                    lume_worker_compute(copy_pages, &copy);
                    lume_worker_checkpoint(0);
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
    for (struct vm_object *object = file_cache; object; object = object->file_next)
        if (object->loading && !object->io_running && !object->file_work.queued)
            (void)lume_work_submit(&object->file_work);
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
#if !defined(__aarch64__) && !defined(LUME_ARCH_AARCH64) && !defined(LUME_ARCH_AMD64) &&           \
    !defined(LUME_ARCH_I686)
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
        struct vm_handle memory_handle;
        struct vm_handle *handle = lume_vfs_external(files, (unsigned)request->fd, &handle_ops);
        if (!handle)
        {
            unsigned mode;
            const struct lume_vm_dma_pin *pin =
                lume_vfs_memory(files, (unsigned)request->fd, &mode);
            if (pin)
            {
                if (!(flags & LUME_MAP_SHARED) || mode == LUME_O_WRONLY)
                    goto access;
                if (pin->serial != pin->object->serial || offset > pin->bytes ||
                    length > pin->bytes - offset || pin->offset > INT64_MAX - offset)
                    goto invalid;
                memory_handle = (struct vm_handle){.object = pin->object,
                                                   .write = pin->write && mode == LUME_O_RDWR};
                handle = &memory_handle;
                offset += pin->offset;
            }
        }
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
                ((protection & LUME_PROT_EXEC) && object->dma_pins) ||
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
            if (length > INT64_MAX - offset)
                goto invalid;
            int error = 0;
            struct vm_object *source = handle ? handle->object : NULL;
            int held = -1;
            if (!source)
            {
                held = lume_vfs_hold(files, (unsigned)request->fd);
                if (held >= 0)
                    source = file_object(held, &error);
                else
                    error = held;
            }
            if (!source || source->changing || source->dma_pins)
            {
                if (held >= 0)
                    lume_vfs_drop(held);
                lume_kernel_free(area);
                return error ? error : -LUME_EBUSY;
            }
            /* Retain the backing before allocation can suspend. */
            ++source->references;
            object = new_object(length, false);
            if (object)
                backing_attach(object, source, offset);
            drop(source);
            if (held >= 0)
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

bool lume_vm_image_map(unsigned task, int file, const struct lume_elf_plan *plan)
{
    int error;
    struct vm_object *source = file_object(file, &error);
    if (!source || task >= LUME_TASK_LIMIT)
        return false;
    ++source->references;
    for (unsigned i = 0; i < plan->count; ++i)
    {
        const struct lume_elf_segment *segment = &plan->segments[i];
        uint64_t first = segment->address & ~(PAGE - 1);
        uint64_t length = rounded(segment->address - first + segment->memory_size);
        struct vm_area *area = allocate(sizeof(*area));
        struct vm_object *object = new_object(length, false);
        if (!area || !object || !free_range(task, first, first + length))
        {
            lume_kernel_free(area);
            if (object)
                drop(object);
            lume_vm_image_release(task);
            drop(source);
            return false;
        }
        backing_attach(object, source, segment->offset & ~(PAGE - 1));
        object->file_limited = true;
        object->file_first = segment->address - first;
        object->file_end = object->file_first + segment->file_size;
        unsigned protection = LUME_PROT_READ | ((segment->flags & 2) ? LUME_PROT_WRITE : 0) |
                              ((segment->flags & 1) ? LUME_PROT_EXEC : 0);
        *area = (struct vm_area){.first = first,
                                 .end = first + length,
                                 .protection = protection,
                                 .maximum = protection,
                                 .image = true,
                                 .object = object};
        insert(task, area);
    }
    drop(source);
    return true;
}

void lume_vm_image_release(unsigned task)
{
    if (task >= LUME_TASK_LIMIT)
        return;
    for (;;)
    {
        struct vm_area *area = spaces[task];
        while (area && !area->image)
            area = area->next;
        if (!area)
        {
            while (retired)
            {
                reap(NULL);
                lume_worker_checkpoint(0);
            }
            return;
        }
        remove_range(task, area->first, area->end);
    }
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
        lume_worker_checkpoint(0);
    }
    lume_sync_code_finish();
    return 0;
}

static int64_t vm_call(struct lume_files *files, unsigned pid, unsigned operation,
                       struct lume_vm_request *request, bool image)
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
        if (!image && operation == LUME_VM_PROTECT &&
            (first < LUME_VM_BASE || first >= LUME_VM_LIMIT))
            return lume_task_protect_image(task, first, length, request->protection);
        if (!length || (first & (PAGE - 1)) || first > UINT64_MAX - length ||
            (!image &&
             (first < LUME_VM_BASE || first >= LUME_VM_LIMIT || length > LUME_VM_LIMIT - first)))
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
                if (!area || (image && !area->image))
                    return -LUME_ENOMEM;
                if (area->object->changing)
                    return -LUME_EBUSY;
                if (((protection & 6u) == 6u && !area->jit) || (protection & ~area->maximum) ||
                    ((protection & LUME_PROT_EXEC) && area->object->dma_pins) ||
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
                area_changed(area);
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
                                        lume_worker_checkpoint(0);
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
        if (handle->object->dma_writers && (seals & (LUME_SEAL_WRITE | LUME_SEAL_FUTURE_WRITE)))
            return -LUME_EBUSY;
        if ((seals & LUME_SEAL_WRITE) &&
            writable_alias(handle->object, 0, UINT64_MAX, LUME_PROT_EXEC, LUME_TASK_LIMIT, 0, 0))
            return -LUME_EBUSY;
        handle->object->seals |= seals;
        return 0;
    }
    return -LUME_EINVAL;
}

int64_t lume_vm_call(struct lume_files *files, unsigned pid, unsigned operation,
                     struct lume_vm_request *request)
{
    return vm_call(files, pid, operation, request, false);
}

int64_t lume_vm_image_protect(unsigned task, uint64_t first, uint64_t length, unsigned protection)
{
    struct lume_vm_request request = {
        .version = LUME_VM_VERSION, .address = first, .length = length, .protection = protection};
    return vm_call(NULL, task + 1, LUME_VM_PROTECT, &request, true);
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
            lume_worker_checkpoint(0);
        }
    }
}

void lume_vm_swap(unsigned a, unsigned b)
{
    struct vm_area *areas = spaces[a];
    spaces[a] = spaces[b];
    spaces[b] = areas;
    for (struct vm_area *area = spaces[a]; area; area = area->next)
    {
        area->owner = a;
        area_changed(area);
    }
    for (struct vm_area *area = spaces[b]; area; area = area->next)
    {
        area->owner = b;
        area_changed(area);
    }
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
            {
                struct vm_object *old = area->object;
                copy->object->discardable = old->discardable;
                if (old->backing)
                {
                    backing_attach(copy->object, old->backing, old->backing_offset + area->offset);
                    copy->object->file_limited = old->file_limited;
                    copy->object->file_first =
                        old->file_first > area->offset ? old->file_first - area->offset : 0;
                    copy->object->file_end =
                        old->file_end > area->offset ? old->file_end - area->offset : 0;
                }
            }
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
        area_changed(copy);
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
#if defined(LUME_ARCH_I686)
    uint64_t base = LUME_THREAD_VM_BASE + PAGE;
    uint64_t span = tls + PAGE + stack;
    for (struct vm_area *area = spaces[owner]; area; area = area->next)
    {
        if (area->end <= LUME_THREAD_VM_BASE)
            continue;
        if (area->first >= base + span + PAGE)
            break;
        if (area->end + PAGE > base)
            base = area->end + PAGE;
    }
    if (base >= LUME_THREAD_VM_LIMIT || span + PAGE > LUME_THREAD_VM_LIMIT - base)
        return false;
#else
    uint64_t base = LUME_THREAD_VM_BASE + (uint64_t)task * LUME_THREAD_STRIDE + PAGE;
#endif
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
            area_changed(area);
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
