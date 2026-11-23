/* SPDX-License-Identifier: GPL-2.0-only */
#include <linux/slab.h>
#include <linux/err.h>
#include <linux/string.h>
#include <lume/linux_memory.h>
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#endif

static void *(*allocate_bytes)(size_t);
static void (*release_bytes)(void *);
static unsigned allocations;

struct allocation
{
    struct allocation *next, **link;
    void *base;
    size_t bytes;
    unsigned dma_users;
    bool release_pending, erase_pending;
};

static struct allocation *live;

int lume_linux_memory_start(void *(*allocate)(size_t), void (*release)(void *))
{
    if (allocate_bytes)
        return -EBUSY;
    if (!allocate || !release)
        return -EINVAL;
    allocate_bytes = allocate;
    release_bytes = release;
    return 0;
}

int lume_linux_memory_stop(void)
{
    if (allocations)
        return -EBUSY;
    allocate_bytes = NULL;
    release_bytes = NULL;
    return 0;
}

unsigned lume_linux_memory_allocations(void)
{
    return allocations;
}

void *kmalloc(size_t bytes, gfp_t flags)
{
    if (!allocate_bytes || flags != GFP_KERNEL || allocations == UINT32_MAX ||
        bytes > SIZE_MAX - sizeof(struct allocation) - 63)
        return NULL;
    if (!bytes)
        return ZERO_SIZE_PTR;
    void *base = allocate_bytes(bytes + sizeof(struct allocation) + 63);
    if (!base)
        return NULL;
    uintptr_t address = ((uintptr_t)base + sizeof(struct allocation) + 63) & ~(uintptr_t)63;
    struct allocation *record = (struct allocation *)address - 1;
    *record = (struct allocation){.next = live, .link = &live, .base = base, .bytes = bytes};
    if (live)
        live->link = &record->next;
    live = record;
    ++allocations;
    return (void *)address;
}

void *kzalloc(size_t bytes, gfp_t flags)
{
    void *memory = kmalloc(bytes, flags);
    if (!ZERO_OR_NULL_PTR(memory))
        memset(memory, 0, bytes);
    return memory;
}

void *kmalloc_array(size_t count, size_t bytes, gfp_t flags)
{
    return bytes && count > SIZE_MAX / bytes ? NULL : kmalloc(count * bytes, flags);
}

void *kcalloc(size_t count, size_t bytes, gfp_t flags)
{
    return bytes && count > SIZE_MAX / bytes ? NULL : kzalloc(count * bytes, flags);
}

size_t ksize(const void *memory)
{
    return ZERO_OR_NULL_PTR(memory) ? 0 : ((const struct allocation *)memory - 1)->bytes;
}

static void release_allocation(struct allocation *record)
{
    if (record->erase_pending)
        memzero_explicit(record + 1, record->bytes);
    *record->link = record->next;
    if (record->next)
        record->next->link = record->link;
    --allocations;
    release_bytes(record->base);
}

static void free_allocation(const void *memory, bool erase)
{
    if (ZERO_OR_NULL_PTR(memory))
        return;
    struct allocation *record = (struct allocation *)memory - 1;
    record->erase_pending |= erase;
    if (record->dma_users)
        record->release_pending = true;
    else
        release_allocation(record);
}

void kfree(const void *memory)
{
    free_allocation(memory, false);
}

void *lume_linux_memory_dma_hold(const void *memory, size_t bytes)
{
    uintptr_t start = (uintptr_t)memory;
    if (!bytes)
        return NULL;
    for (struct allocation *record = live; record; record = record->next)
    {
        uintptr_t base = (uintptr_t)(record + 1);
        if (start < base || start - base >= record->bytes)
            continue;
        if (bytes > record->bytes - (start - base) || record->release_pending ||
            record->dma_users == UINT32_MAX)
            return NULL;
        ++record->dma_users;
        return record;
    }
    return NULL;
}

void lume_linux_memory_dma_release(void *handle)
{
    struct allocation *record = handle;
    if (!--record->dma_users && record->release_pending)
        release_allocation(record);
}

void memzero_explicit(void *memory, size_t bytes)
{
    if (!bytes)
        return;
    memset(memory, 0, bytes);
    /* The compiler barrier retains the erase before the allocation is released. */
#if defined(_MSC_VER) && !defined(__clang__)
    _ReadWriteBarrier();
#else
    __asm__ volatile("" : : "r"(memory) : "memory");
#endif
}

void kfree_sensitive(const void *memory)
{
    free_allocation(memory, true);
}

void *krealloc(const void *memory, size_t bytes, gfp_t flags)
{
    if (!bytes)
    {
        kfree(memory);
        return ZERO_SIZE_PTR;
    }
    if (ZERO_OR_NULL_PTR(memory))
        return kmalloc(bytes, flags);
    if (!allocate_bytes || flags != GFP_KERNEL)
        return NULL;
    const struct allocation *record = (const struct allocation *)memory - 1;
    if (record->dma_users)
        return NULL;
    size_t previous = record->bytes;
    if (bytes <= previous)
        return (void *)memory;
    void *next = kmalloc(bytes, flags);
    if (!next)
        return NULL;
    memcpy(next, memory, previous);
    kfree(memory);
    return next;
}

void *kmemdup(const void *memory, size_t bytes, gfp_t flags)
{
    if (bytes && !memory)
        return NULL;
    void *next = kmalloc(bytes, flags);
    if (!ZERO_OR_NULL_PTR(next))
        memcpy(next, memory, bytes);
    return next;
}
