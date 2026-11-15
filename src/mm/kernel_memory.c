#include "lume/kernel_memory.h"
#include "lume/early.h"
#include "lume/irq.h"
#include "lume/ticket_lock.h"
#include <string.h>

#define PAGE_BYTES 4096u
#define CLASS_COUNT 7u
#define ALLOCATION_MAGIC UINT64_C(0x4c554d45414c4c4f)
#define SLOT_MAGIC UINT64_C(0x4c554d45534c4f54)
#define SLAB_MAGIC UINT64_C(0x4c554d45534c4142)

struct allocation
{
    uint64_t pages, magic;
};

struct slab
{
    uint64_t magic, used[2];
    struct slab *previous, *next;
    unsigned class_id, capacity, available;
};

#define SLAB_HEADER ((sizeof(struct slab) + 15u) & ~(size_t)15u)

struct size_class
{
    struct lume_ticket_lock lock;
    struct slab *available, *empty;
};

static struct size_class classes[CLASS_COUNT];
static unsigned cached_empty_pages;

struct guard
{
    uint64_t flags;
    uint32_t ticket;
};

static struct guard enter(struct size_class *class)
{
    struct guard guard = {.flags = lume_irq_save()};
    guard.ticket = lume_ticket_reserve(&class->lock);
    while (!lume_ticket_ready(&class->lock, guard.ticket))
        __atomic_signal_fence(__ATOMIC_ACQUIRE);
    return guard;
}

static void leave(struct size_class *class, struct guard guard)
{
    lume_ticket_release(&class->lock, guard.ticket);
    lume_irq_restore(guard.flags);
}

static void remove_available(struct size_class *class, struct slab *slab)
{
    if (slab->previous)
        slab->previous->next = slab->next;
    else
        class->available = slab->next;
    if (slab->next)
        slab->next->previous = slab->previous;
    slab->previous = slab->next = NULL;
}

static void add_available(struct size_class *class, struct slab *slab)
{
    slab->previous = NULL;
    slab->next = class->available;
    if (slab->next)
        slab->next->previous = slab;
    class->available = slab;
}

static void *allocate_small(unsigned class_id)
{
    unsigned stride = 32u << class_id;
    struct size_class *class = &classes[class_id];
    struct guard guard = enter(class);
    struct slab *slab = class->available;
    if (!slab)
    {
        leave(class, guard);
        uint64_t physical = lume_page_alloc(0);
        if (!physical)
            return NULL;
        slab = (void *)(uintptr_t)physical;
        *slab = (struct slab){.magic = SLAB_MAGIC,
                              .class_id = class_id,
                              .capacity = (PAGE_BYTES - SLAB_HEADER) / stride};
        slab->available = slab->capacity;
        guard = enter(class);
        add_available(class, slab);
    }
    uint64_t free_bits = ~slab->used[0];
    unsigned slot = free_bits ? (unsigned)__builtin_ctzll(free_bits)
                              : 64u + (unsigned)__builtin_ctzll(~slab->used[1]);
    if (slot >= slab->capacity)
        lume_fail("invalid slab free count");
    slab->used[slot / 64] |= UINT64_C(1) << (slot & 63);
    if (!--slab->available)
        remove_available(class, slab);
    if (class->empty == slab)
    {
        class->empty = NULL;
        __atomic_fetch_sub(&cached_empty_pages, 1, __ATOMIC_RELEASE);
    }
    struct allocation *header = (void *)((uintptr_t)slab + SLAB_HEADER + slot * stride);
    *header = (struct allocation){class_id + 1u, SLOT_MAGIC};
    leave(class, guard);
    memset(header + 1, 0, stride - sizeof(*header));
    return header + 1;
}

void *lume_kernel_allocate(size_t bytes)
{
    if (!bytes || bytes > 16u * 1024u * 1024u)
        return NULL;
    for (unsigned class_id = 0; class_id < CLASS_COUNT; ++class_id)
        if (bytes <= (32u << class_id) - sizeof(struct allocation))
            return allocate_small(class_id);
    return lume_kernel_allocate_private(bytes);
}

void *lume_kernel_allocate_private(size_t bytes)
{
    if (!bytes || bytes > 16u * 1024u * 1024u)
        return NULL;
    unsigned pages = (unsigned)((bytes + sizeof(struct allocation) + PAGE_BYTES - 1) / PAGE_BYTES);
    uint64_t physical = pages == 1 ? lume_page_alloc(0) : lume_pages_alloc_span(pages);
    if (!physical)
        return NULL;
    struct allocation *header = (void *)(uintptr_t)physical;
    *header = (struct allocation){pages, ALLOCATION_MAGIC};
    return header + 1;
}

void lume_kernel_free(void *memory)
{
    if (!memory)
        return;
    struct allocation *header = (struct allocation *)memory - 1;
    if (header->magic == SLOT_MAGIC && header->pages && header->pages <= CLASS_COUNT)
    {
        unsigned class_id = (unsigned)header->pages - 1u, stride = 32u << class_id;
        struct size_class *class = &classes[class_id];
        struct slab *slab = (void *)((uintptr_t)header & ~(uintptr_t)(PAGE_BYTES - 1u));
        uintptr_t offset = (uintptr_t)header - (uintptr_t)slab;
        struct guard guard = enter(class);
        unsigned slot = (unsigned)((offset - SLAB_HEADER) / stride);
        if (slab->magic != SLAB_MAGIC || slab->class_id != class_id || offset < SLAB_HEADER ||
            (offset - SLAB_HEADER) % stride || slot >= slab->capacity ||
            !(slab->used[slot / 64] & (UINT64_C(1) << (slot & 63))))
            lume_fail("invalid kernel slab release");
        header->magic = 0;
        slab->used[slot / 64] &= ~(UINT64_C(1) << (slot & 63));
        if (!slab->available++)
            add_available(class, slab);
        bool release = false;
        if (slab->available == slab->capacity)
        {
            if (class->empty)
            {
                remove_available(class, slab);
                slab->magic = 0;
                release = true;
            }
            else
            {
                class->empty = slab;
                __atomic_fetch_add(&cached_empty_pages, 1, __ATOMIC_RELEASE);
            }
        }
        leave(class, guard);
        if (release && !lume_page_free((uintptr_t)slab))
            lume_fail("kernel slab release");
        return;
    }
    if (header->magic != ALLOCATION_MAGIC || !header->pages || header->pages > 4097 ||
        ((uintptr_t)header & (PAGE_BYTES - 1)))
        lume_fail("invalid kernel allocation");
    uint64_t pages = header->pages;
    header->magic = 0;
    for (uint64_t i = 0; i < pages; ++i)
        if (!lume_page_free((uintptr_t)header + i * PAGE_BYTES))
            lume_fail("kernel allocation release");
}

uint64_t lume_kernel_allocation_pages(const void *memory)
{
    if (!memory)
        return 0;
    const struct allocation *header = (const struct allocation *)memory - 1;
    if (header->magic == SLOT_MAGIC && header->pages && header->pages <= CLASS_COUNT)
        return 1;
    if (header->magic != ALLOCATION_MAGIC)
        lume_fail("invalid kernel allocation query");
    return header->pages;
}

uint64_t lume_kernel_cache_pages(void)
{
    return __atomic_load_n(&cached_empty_pages, __ATOMIC_ACQUIRE);
}
