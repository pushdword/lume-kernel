#include "lume/module_arch.h"
#include "lume/elf.h"
#include "lume/mmu.h"
#include "lume/worker.h"
#include "lume/abi.h"
#include "lume/memory.h"
#include "lume/vm.h"
#include "lume/vfs.h"
#include "lume/external_file.h"
#include <string.h>

static uint16_t le16(const unsigned char *p)
{
    return (uint16_t)p[0] | (uint16_t)p[1] << 8;
}

static uint32_t le32(const unsigned char *p)
{
    return le16(p) | (uint32_t)le16(p + 2) << 16;
}

static uint64_t le64(const unsigned char *p)
{
    return le32(p) | (uint64_t)le32(p + 4) << 32;
}

static bool bounded(uint64_t offset, uint64_t bytes, uint64_t size)
{
    return offset <= size && bytes <= size - offset;
}

static bool image_range(uint64_t address, uint64_t bytes)
{
    return (address >= LUME_ELF_BASE && bounded(address, bytes, LUME_ELF_LIMIT)) ||
           (address >= LUME_LARGE_ELF_BASE && bounded(address, bytes, LUME_LARGE_ELF_LIMIT));
}

static bool plan_headers(const void *data, size_t size, const unsigned char *headers_data,
                         struct lume_elf_plan *plan)
{
    const unsigned char *b = data;
    *plan = (struct lume_elf_plan){0};
#if defined(LUME_ARCH_I686)
    const unsigned elf_class = 1, ehsize = 52, phsize = 32;
    if (size < ehsize || le32(b + 36) != 0 || le16(b + 40) != ehsize || le16(b + 42) != phsize)
        return false;
    unsigned headers = le16(b + 44);
    uint64_t phoff = le32(b + 28), entry = le32(b + 24);
#else
    const unsigned elf_class = 2, ehsize = 64, phsize = 56;
    if (size < ehsize || le32(b + 48) != 0 || le16(b + 52) != ehsize || le16(b + 54) != phsize)
        return false;
    unsigned headers = le16(b + 56);
    uint64_t phoff = le64(b + 32), entry = le64(b + 24);
#endif
    if (b[0] != 0x7f || b[1] != 'E' || b[2] != 'L' || b[3] != 'F' || b[4] != elf_class ||
        b[5] != 1 || b[6] != 1 || b[7] != 0 || b[8] != 0 || le16(b + 16) != 2 ||
        le16(b + 18) != LUME_ELF_MACHINE || le32(b + 20) != 1 || !headers || headers > 16 ||
        phoff < ehsize || !bounded(phoff, (uint64_t)headers * phsize, size))
        return false;
    bool executable_entry = false;
    bool tls_seen = false;
    uint64_t tls_offset = 0;
    for (unsigned n = 0; n < headers; ++n)
    {
        const unsigned char *h = (headers_data ? headers_data : b + phoff) + n * phsize;
#if defined(LUME_ARCH_I686)
        unsigned type = le32(h), flags = le32(h + 24);
        uint64_t offset = le32(h + 4), address = le32(h + 8), file = le32(h + 16),
                 memory = le32(h + 20), align = le32(h + 28);
#else
        unsigned type = le32(h), flags = le32(h + 4);
        uint64_t offset = le64(h + 8), address = le64(h + 16), file = le64(h + 32),
                 memory = le64(h + 40), align = le64(h + 48);
#endif
        if (type == 7)
        {
            if (tls_seen)
                return false;
            tls_seen = true;
            if (!memory && !file)
                continue;
            if (!align)
                align = 1;
            if (!memory || memory > 65536 || file > memory || !bounded(offset, file, size) ||
                !image_range(address, memory) || align > 64 || (align & (align - 1)) ||
                ((address - offset) & (align - 1)))
                return false;
            plan->tls_address = address;
            plan->tls_file = file;
            plan->tls_size = memory;
            plan->tls_align = align;
            tls_offset = offset;
            continue;
        }
        if (type == 0 || type == 4 || type == 6)
            continue;
        if (type == 0x6474e551)
        {
            if (flags & 1)
                return false;
            continue;
        }
        if (type != 1 || plan->count == LUME_ELF_SEGMENTS)
            return false;
        struct lume_elf_segment s = {offset, address, file, memory, flags};
        if ((flags & ~7u) || !(flags & 4) || (flags & 3) == 3 || !s.memory_size ||
            s.file_size > s.memory_size || !bounded(s.offset, s.file_size, size) ||
            !image_range(s.address, s.memory_size) || (s.address & 4095) != (s.offset & 4095) ||
            (align > 1 && ((align & (align - 1)) || align > 0x200000 ||
                           (s.address & (align - 1)) != (s.offset & (align - 1)))))
            return false;
        uint64_t first = s.address & ~UINT64_C(4095),
                 end = (s.address + s.memory_size + 4095) & ~UINT64_C(4095);
        unsigned pages = (unsigned)((end - first) / 4096);
        if (pages > LUME_ELF_PAGES - plan->pages)
            return false;
        for (unsigned i = 0; i < plan->count; ++i)
        {
            struct lume_elf_segment *old = &plan->segments[i];
            uint64_t a = old->address & ~UINT64_C(4095),
                     z = (old->address + old->memory_size + 4095) & ~UINT64_C(4095);
            if (first < z && a < end)
                return false; /* No page permission unions. */
        }
        if ((flags & 1) && entry >= s.address && entry - s.address < s.file_size)
            executable_entry = true;
        plan->segments[plan->count++] = s;
        plan->pages += pages;
    }
    if (!plan->count || !executable_entry)
        return false;
    if (plan->tls_address)
    {
        bool contained = false;
        for (unsigned i = 0; i < plan->count; ++i)
        {
            const struct lume_elf_segment *s = &plan->segments[i];
            uint64_t delta = plan->tls_address - s->address;
            if (plan->tls_address >= s->address && delta <= s->memory_size &&
                plan->tls_size <= s->memory_size - delta &&
                (!plan->tls_file ||
                 (delta <= s->file_size && plan->tls_file <= s->file_size - delta &&
                  tls_offset == s->offset + delta)))
                contained = true;
        }
        if (!contained)
            return false;
    }
    plan->entry = entry;
    return true;
}

bool lume_elf_plan(const void *data, size_t size, struct lume_elf_plan *plan)
{
    return plan_headers(data, size, NULL, plan);
}

bool lume_elf_plan_file(int file, size_t size, struct lume_elf_plan *plan)
{
    unsigned char header[64], entries[16 * 56];
#if defined(LUME_ARCH_I686)
    const unsigned ehsize = 52, phsize = 32;
#else
    const unsigned ehsize = 64, phsize = 56;
#endif
    if (size < ehsize || lume_vfs_pread_object(file, 0, header, ehsize) != ehsize)
        return false;
#if defined(LUME_ARCH_I686)
    uint64_t offset = le32(header + 28);
    unsigned count = le16(header + 44);
#else
    uint64_t offset = le64(header + 32);
    unsigned count = le16(header + 56);
#endif
    uint64_t bytes = (uint64_t)count * phsize;
    if (!count || count > 16 || !bounded(offset, bytes, size) ||
        lume_vfs_pread_object(file, offset, entries, bytes) != (int64_t)bytes)
        return false;
    return plan_headers(header, size, entries, plan);
}

/* A successful load consumes the held executable description and its write exclusion. */
bool lume_elf_load_file(unsigned task, int file, const struct lume_elf_plan *plan,
                        struct lume_elf_image *image)
{
    *image = (struct lume_elf_image){0};
    if (!lume_vm_image_map(task, file, plan))
        return false;
    image->task = task;
    image->entry = plan->entry;
    image->tls_address = plan->tls_address;
    image->tls_file = plan->tls_file;
    image->tls_size = plan->tls_size;
    image->tls_align = plan->tls_align;
    image->source_file = file + 1;
    return true;
}

static bool image_page_mapped(const struct lume_elf_image *image, unsigned index)
{
    uint64_t physical;
#if defined(__aarch64__) || defined(LUME_ARCH_AARCH64) || defined(LUME_ARCH_AMD64) ||              \
    defined(LUME_ARCH_I686)
    for (unsigned view = 0; view < 2; ++view)
        if (!lume_mmu_translate(lume_mmu_task_view_root(image->task, view != 0),
                                image->pages[index].address, false, false, false, &physical) ||
            physical != image->pages[index].physical)
            return false;
#else
    if (!lume_mmu_translate(lume_mmu_task_root(image->task), image->pages[index].address, false,
                            false, false, &physical) ||
        physical != image->pages[index].physical)
        return false;
#endif
    return true;
}

static void protect_image_batch(struct lume_elf_image *image, const uint64_t *addresses,
                                const uint64_t *physical, const unsigned *indices, unsigned count,
                                unsigned protection)
{
    if (!lume_mmu_protect_user_batch(image->task, addresses, physical, count, protection))
        lume_fail("ELF page protection state");
    for (unsigned i = 0; i < count; ++i)
        image->pages[indices[i]].protection = protection;
}

int64_t lume_elf_protect(struct lume_elf_image *image, uint64_t first, uint64_t length,
                         unsigned protection)
{
    if (!image || image->task >= LUME_TASK_LIMIT || !length || ((first | length) & 4095) ||
        !image_range(first, length))
        return -LUME_EINVAL;
    if ((protection & ~7u) || (protection & 6u) == 6u)
        return -LUME_EACCES;
    if (protection)
        protection |= LUME_PROT_READ;
    if (image->source_file)
        return lume_vm_image_protect(image->task, first, length, protection);
    if (!image->pages || length / 4096 > image->count)
        return -LUME_ENOMEM;
    uint64_t end = first + length;
    unsigned found = 0;
    for (unsigned i = 0; i < image->count; ++i)
        if (image->pages[i].address >= first && image->pages[i].address < end)
        {
            if (protection & ~image->pages[i].maximum)
                return -LUME_EACCES;
            if (!image_page_mapped(image, i))
                return -LUME_ENOMEM;
            ++found;
        }
    if (found != length / 4096)
        return -LUME_ENOMEM;
    uint64_t addresses[LUME_MMU_BATCH_PAGES], physical[LUME_MMU_BATCH_PAGES];
    unsigned indices[LUME_MMU_BATCH_PAGES], count = 0;
    for (unsigned i = 0; i < image->count; ++i)
        if (image->pages[i].address >= first && image->pages[i].address < end)
        {
            addresses[count] = image->pages[i].address;
            physical[count] = image->pages[i].physical;
            indices[count++] = i;
            if (count == LUME_MMU_BATCH_PAGES)
            {
                protect_image_batch(image, addresses, physical, indices, count, protection);
                count = 0;
                lume_worker_checkpoint(65536);
            }
        }
    if (count)
        protect_image_batch(image, addresses, physical, indices, count, protection);
    return 0;
}

void lume_elf_release(struct lume_elf_image *image)
{
    if (image->source_file)
    {
        lume_vm_image_release(image->task);
        lume_vfs_exec_unpin(image->source_file - 1);
        lume_vfs_drop(image->source_file - 1);
        *image = (struct lume_elf_image){0};
        return;
    }
    for (unsigned first = 0; first < image->count;)
    {
        uint64_t addresses[LUME_MMU_BATCH_PAGES];
        unsigned count = image->count - first;
        if (count > LUME_MMU_BATCH_PAGES)
            count = LUME_MMU_BATCH_PAGES;
        for (unsigned i = 0; i < count; ++i)
            addresses[i] = image->pages[first + i].address;
        if (!lume_mmu_unmap_user_batch(image->task, addresses, count))
            lume_fail("ELF mapping release");
        for (unsigned i = 0; i < count; ++i)
            if (!lume_page_free(image->pages[first + i].physical))
                lume_fail("ELF frame release");
        first += count;
        if (count == LUME_MMU_BATCH_PAGES)
            lume_worker_checkpoint(65536);
    }
    for (unsigned i = 0; i < image->metadata_pages; ++i)
        if (!lume_page_free((uintptr_t)image->pages + (uint64_t)i * 4096))
            lume_fail("ELF metadata release");
    *image = (struct lume_elf_image){0};
}

struct elf_page_copy
{
    struct
    {
        unsigned char *target;
        const unsigned char *source;
        size_t bytes;
        uint64_t executable;
    } pages[LUME_MMU_BATCH_PAGES];

    unsigned count;
};

/* Image builders retain lifecycle ownership until publication. Copy descriptors contain
 * only pinned physical memory and cache ranges. */
static void copy_elf_pages(void *context)
{
    const struct elf_page_copy *copy = context;
    bool executable = false;
    for (unsigned n = 0; n < copy->count; ++n)
    {
        if (copy->pages[n].bytes)
            memcpy(copy->pages[n].target, copy->pages[n].source, copy->pages[n].bytes);
        if (copy->pages[n].executable)
        {
            lume_sync_code_defer(copy->pages[n].executable, 4096);
            executable = true;
        }
    }
    if (executable)
        lume_sync_code_clean_finish();
}

static void finish_elf_batch(struct elf_page_copy *copy)
{
    if (copy->count == LUME_MMU_BATCH_PAGES)
    {
        lume_worker_compute(copy_elf_pages, copy);
        lume_worker_checkpoint(0);
    }
    else if (copy->count)
        copy_elf_pages(copy);
    copy->count = 0;
}

bool lume_elf_clone(unsigned task, const struct lume_elf_image *source,
                    struct lume_elf_image *image)
{
    *image = *source;
    image->task = task;
    if (source->source_file)
    {
        int file = source->source_file - 1;
        if (lume_vfs_object_retain(file))
            goto lazy_fail;
        if (lume_vfs_exec_pin(file))
        {
            lume_vfs_drop(file);
            goto lazy_fail;
        }
        return true;
    lazy_fail:
        *image = (struct lume_elf_image){0};
        return false;
    }
    image->count = 0;
    image->pages = (void *)(uintptr_t)lume_pages_alloc_span(image->metadata_pages);
    if (!image->pages)
    {
        *image = (struct lume_elf_image){0};
        return false;
    }
    struct elf_page_copy copy = {0};
    for (unsigned i = 0; i < source->count; ++i)
    {
        uint64_t address = source->pages[i].address;
        unsigned protection = source->pages[i].protection;
        bool write = (protection & LUME_PROT_WRITE) != 0;
        bool execute = (protection & LUME_PROT_EXEC) != 0;
        uint64_t page = lume_page_alloc(0);
        if (!page)
            goto fail;
        if (!lume_mmu_map_user(task, address, page, write, execute))
        {
            (void)lume_page_free(page);
            goto fail;
        }
        image->pages[image->count] = source->pages[i];
        image->pages[image->count++].physical = page;
        if (!protection && !lume_mmu_protect_user_batch(task, &address, &page, 1, 0))
            goto fail;
        copy.pages[copy.count].target = (void *)(uintptr_t)page;
        copy.pages[copy.count].source = (const void *)(uintptr_t)source->pages[i].physical;
        copy.pages[copy.count].bytes = 4096;
        copy.pages[copy.count++].executable = source->pages[i].maximum & LUME_PROT_EXEC ? page : 0;
        if (copy.count == LUME_MMU_BATCH_PAGES)
            finish_elf_batch(&copy);
    }
    finish_elf_batch(&copy);
    lume_sync_code_finish();
    return true;
fail:
    lume_sync_code_finish();
    lume_elf_release(image);
    return false;
}

bool lume_elf_load(unsigned task, const void *data, size_t size, struct lume_elf_image *image)
{
    struct lume_elf_plan plan;
    *image = (struct lume_elf_image){0};
    if (!lume_elf_plan(data, size, &plan))
        return false;
    image->task = task;
    image->metadata_pages = (unsigned)((plan.pages * sizeof(*image->pages) + 4095) / 4096);
    image->pages = (void *)(uintptr_t)lume_pages_alloc_span(image->metadata_pages);
    if (!image->pages)
    {
        *image = (struct lume_elf_image){0};
        return false;
    }
    struct elf_page_copy copy = {0};
    for (unsigned n = 0; n < plan.count; ++n)
    {
        struct lume_elf_segment *s = &plan.segments[n];
        uint64_t first = s->address & ~UINT64_C(4095),
                 end = (s->address + s->memory_size + 4095) & ~UINT64_C(4095);
        for (uint64_t va = first; va < end; va += 4096)
        {
            uint64_t page = lume_page_alloc(0);
            if (!page)
                goto fail;
            if (!lume_mmu_map_user(task, va, page, (s->flags & 2) != 0, (s->flags & 1) != 0))
            {
                (void)lume_page_free(page);
                goto fail;
            }
            unsigned protection = LUME_PROT_READ | ((s->flags & 2) ? LUME_PROT_WRITE : 0) |
                                  ((s->flags & 1) ? LUME_PROT_EXEC : 0);
            image->pages[image->count].address = va;
            image->pages[image->count].physical = page;
            image->pages[image->count].maximum = protection;
            image->pages[image->count++].protection = protection;
            uint64_t copy_start = va > s->address ? va : s->address;
            uint64_t file_end = s->address + s->file_size;
            uint64_t copy_end = va + 4096 < file_end ? va + 4096 : file_end;
            copy.pages[copy.count].target = (void *)(uintptr_t)page;
            copy.pages[copy.count].source = 0;
            copy.pages[copy.count].bytes = 0;
            copy.pages[copy.count].executable = (s->flags & 1) ? page : 0;
            if (copy_start < copy_end)
            {
                copy.pages[copy.count].target = (void *)(uintptr_t)(page + copy_start - va);
                copy.pages[copy.count].source =
                    (const unsigned char *)data + s->offset + copy_start - s->address;
                copy.pages[copy.count].bytes = (size_t)(copy_end - copy_start);
            }
            if (++copy.count == LUME_MMU_BATCH_PAGES)
                finish_elf_batch(&copy);
        }
    }
    finish_elf_batch(&copy);
    lume_sync_code_finish();
    image->entry = plan.entry;
    image->tls_address = plan.tls_address;
    image->tls_file = plan.tls_file;
    image->tls_size = plan.tls_size;
    image->tls_align = plan.tls_align;
    return true;
fail:
    lume_sync_code_finish();
    lume_elf_release(image);
    return false;
}
