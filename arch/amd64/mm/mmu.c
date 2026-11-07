#include "lume/mmu.h"
#include "lume/user_buffer.h"
#include "lume/elf.h"
#include "lume/module_memory.h"
#include "lume/acpi.h"
#include "lume/apic.h"
#include "lume/vm.h"
#include "lume/kernel_memory.h"
#include "lume/worker.h"
#include "lume/heap.h"
#include "lume/abi.h"
#include <string.h>
#define PRESENT UINT64_C(1)
#define WRITE UINT64_C(2)
#define USER UINT64_C(4)
#define HUGE UINT64_C(128)
#define NX (UINT64_C(1) << 63)
#define ADDRESS UINT64_C(0x000ffffffffff000)
#define BLOCK UINT64_C(0x200000)
#define RAM_END UINT64_C(0x200000000)
#define USER_BLOCKS ((LUME_USER_LIMIT - LUME_USER_BASE) / BLOCK)
static _Alignas(4096) uint64_t root[512], directories[8][512], pdpt[512];
static _Alignas(4096) uint64_t image_pages[16][512], module_pages[2][512];
/* Thread write scopes select separate roots without writable executable leaves. */
static _Alignas(4096) uint64_t task_root[LUME_TASK_LIMIT][2][512];
static _Alignas(4096) uint64_t task_pdpt[LUME_TASK_LIMIT][2][512];
static _Alignas(4096) uint64_t task_pd[LUME_TASK_LIMIT][2][512];
static uint64_t *task_pt[LUME_TASK_LIMIT][2][USER_BLOCKS];
static unsigned shared[LUME_TASK_LIMIT];
static unsigned jit_depth[LUME_TASK_LIMIT];

struct sparse_table
{
    uint64_t *entries;
    struct sparse_table *next;
};
static struct sparse_table *sparse[LUME_TASK_LIMIT];
static _Alignas(4096) uint64_t trampoline_pages[512];
static uint64_t module_base, module_end, module_first;
extern unsigned char __image_start[], __text_end[], __readonly_end[], __image_end[];
extern unsigned char __stack_guard[], __worker_guard[], __trap_guard[], __fault_guard[];
extern unsigned char __secondary_stacks[];

static uint64_t table(const uint64_t *p, bool user)
{
    return (uintptr_t)p | PRESENT | WRITE | (user ? USER : 0);
}

void lume_mmu_ap_trampoline(bool enable)
{
    /* The startup page is writable only until every AP enters the kernel mappings. */
    trampoline_pages[LUME_AP_TRAMPOLINE / 4096] = LUME_AP_TRAMPOLINE | PRESENT | WRITE | NX;
    directories[0][0] = enable ? table(trampoline_pages, false) : 0;
    lume_arch_flush_user_tlb();
}

uint64_t lume_mmu_root(void)
{
    return (uintptr_t)root;
}

void lume_mmu_switch(uint64_t physical)
{
    __asm__ volatile("movq %0, %%cr3" : : "r"(physical) : "memory");
}

bool lume_mmu_start(const struct lume_fdt *description)
{
    uint64_t initrd_start = description->initrd_start;
    uint64_t initrd_end = description->initrd_end;
    bool initrd_valid = false;
    if (initrd_start >= (uintptr_t)__image_end && initrd_end > initrd_start &&
        initrd_end <= UINT64_C(0x40000000))
        for (unsigned i = 0; i < description->ram_count; ++i)
        {
            uint64_t base = description->ram[i].base, size = description->ram[i].size;
            if (initrd_start >= base && initrd_start - base <= size &&
                initrd_end - initrd_start <= size - (initrd_start - base))
                initrd_valid = true;
        }
    if (!initrd_valid)
        return false;
    uint64_t initrd_first_page = initrd_start & ~UINT64_C(4095);
    memset(root, 0, sizeof(root));
    memset(pdpt, 0, sizeof(pdpt));
    memset(directories, 0, sizeof(directories));
    root[0] = table(pdpt, true);
    for (unsigned g = 0; g < 8; ++g)
    {
        pdpt[g] = table(directories[g], true);
        for (unsigned b = 0; b < 512; ++b)
        {
            uint64_t base = ((uint64_t)g * 512 + b) * BLOCK;
            if (base >= LUME_USER_BASE && base < LUME_USER_LIMIT)
                continue;
            bool ram = false;
            for (unsigned i = 0; i < description->ram_count; ++i)
            {
                uint64_t start = description->ram[i].base;
                uint64_t size = description->ram[i].size;
                if (base >= start && base - start <= size && BLOCK <= size - (base - start))
                    ram = true;
            }
            if (ram)
                directories[g][b] = base | PRESENT | WRITE | HUGE | NX;
            if (base >= 0xc0000000 && base < 0x100000000)
                directories[g][b] = base | PRESENT | WRITE | HUGE | NX | 0x18;
        }
    }
    /* ECAM is described by MCFG and may sit below the fixed PCI MMIO aperture. */
    const struct lume_acpi *acpi = lume_q35_acpi();
    if (acpi)
        for (unsigned i = 0; i < acpi->segment_count; ++i)
        {
            const struct lume_acpi_segment *s = acpi->segments + i;
            uint64_t start = s->base + ((uint64_t)s->first << 20);
            uint64_t end = s->base + (((uint64_t)s->last + 1) << 20);
            if (start < UINT64_C(0x80000000) || end > UINT64_C(0x100000000))
                continue;
            uint64_t first = start & ~(BLOCK - 1), limit = (end + BLOCK - 1) & ~(BLOCK - 1);
            for (uint64_t base = first; base < limit; base += BLOCK)
            {
                for (unsigned n = 0; n < description->ram_count; ++n)
                    if (base < description->ram[n].base + description->ram[n].size &&
                        description->ram[n].base < base + BLOCK)
                        return false;
                directories[base >> 30][(base >> 21) & 511] =
                    base | PRESENT | WRITE | HUGE | NX | 0x18;
            }
        }
    uint64_t first = (uintptr_t)__image_start / BLOCK;
    uint64_t end = ((uintptr_t)__image_end + BLOCK - 1) / BLOCK;
    if (end - first > 16 || end > 512)
        return false;
    for (uint64_t block = first; block < end; ++block)
    {
        uint64_t *pages = image_pages[block - first];
        for (unsigned n = 0; n < 512; ++n)
        {
            uint64_t address = block * BLOCK + n * 4096;
            uint64_t flags = PRESENT | WRITE | NX;
            if (address < (uintptr_t)__text_end)
                flags = PRESENT;
            else if (address < (uintptr_t)__readonly_end)
                flags = PRESENT | NX;
            if (address >= (uintptr_t)__image_end || address == (uintptr_t)__stack_guard ||
                address == (uintptr_t)__worker_guard || address == (uintptr_t)__trap_guard ||
                address == (uintptr_t)__fault_guard)
                flags = 0;
            if (address >= (uintptr_t)__secondary_stacks)
            {
                uint64_t offset = (address - (uintptr_t)__secondary_stacks) % LUME_AP_STACK_BYTES;
                if (!offset || offset == 0x21000 || offset == 0x42000)
                    flags = 0;
            }
            /* Multiboot may place its first module inside the final image window. */
            if (address >= (uintptr_t)__image_end && address >= initrd_first_page &&
                address < initrd_end)
                flags = PRESENT | NX;
            pages[n] = flags ? address | flags : 0;
        }
        directories[0][block] = table(pages, false);
    }
    /* No low-memory identity mapping survives into application execution. */
    directories[0][0] = 0;
    lume_mmu_switch((uintptr_t)root);
    return true;
}

bool lume_mmu_translate(uint64_t physical, uint64_t va, bool write, bool user, bool execute,
                        uint64_t *out)
{
    if (!physical || !out || va >= (UINT64_C(1) << 47))
        return false;
    uint64_t *p = (void *)(uintptr_t)(physical & ADDRESS);
    const unsigned shifts[] = {39, 30, 21, 12};
    for (unsigned level = 0; level < 4; ++level)
    {
        uint64_t d = p[(va >> shifts[level]) & 511];
        if (!(d & PRESENT) || (write && !(d & WRITE)) || (user && !(d & USER)) ||
            (execute && (d & NX)))
            return false;
        if (level == 3 || (level && (d & HUGE)))
        {
            uint64_t mask = (UINT64_C(1) << shifts[level]) - 1;
            *out = (d & ADDRESS & ~mask) | (va & mask);
            return true;
        }
        p = (void *)(uintptr_t)(d & ADDRESS);
    }
    return false;
}

bool lume_mmu_check(void)
{
    uint64_t p;
    return !lume_mmu_translate((uintptr_t)root, 0, false, false, false, &p) &&
           !lume_mmu_translate((uintptr_t)root, (uintptr_t)__stack_guard, true, false, false, &p);
}

static unsigned owner(unsigned task)
{
    return task < LUME_TASK_LIMIT && shared[task] ? shared[task] - 1 : task;
}

uint64_t lume_mmu_task_view_root(unsigned task, bool writer)
{
    task = owner(task);
    if (task >= LUME_TASK_LIMIT || !task_root[task][writer ? 1 : 0][0])
        return 0;
    return (uintptr_t)task_root[task][writer ? 1 : 0];
}

uint64_t lume_mmu_task_root(unsigned task)
{
    return task < LUME_TASK_LIMIT ? lume_mmu_task_view_root(task, jit_depth[task] != 0) : 0;
}

bool lume_mmu_root_matches(unsigned task, uint64_t physical)
{
    task = owner(task);
    physical &= ADDRESS;
    return task < LUME_TASK_LIMIT && physical &&
           (physical == lume_mmu_task_view_root(task, false) ||
            physical == lume_mmu_task_view_root(task, true));
}

bool lume_mmu_root_active(uint64_t physical)
{
    uint64_t active;
    __asm__ volatile("movq %%cr3, %0" : "=r"(active));
    return active == physical;
}

bool lume_mmu_jit_active(unsigned task)
{
    uint64_t writer = lume_mmu_task_view_root(task, true);
    return writer && lume_mmu_root_active(writer);
}

int lume_mmu_jit_scope(unsigned task, bool writable)
{
    if (task >= LUME_TASK_LIMIT || !lume_mmu_task_root(task) ||
        (writable && jit_depth[task] == UINT32_MAX) || (!writable && !jit_depth[task]))
        return -LUME_EINVAL;
    bool before = jit_depth[task] != 0;
    if (writable)
        ++jit_depth[task];
    else
        --jit_depth[task];
    if (before != (jit_depth[task] != 0))
        lume_mmu_switch(lume_mmu_task_root(task));
    return 0;
}

void lume_mmu_jit_copy(unsigned target, unsigned source)
{
    if (target >= LUME_TASK_LIMIT || source >= LUME_TASK_LIMIT)
        lume_fail("invalid JIT scope owner");
    jit_depth[target] = jit_depth[source];
}

void lume_mmu_share_user(unsigned task, unsigned source)
{
    if (task >= LUME_TASK_LIMIT || source >= LUME_TASK_LIMIT || task == source)
        lume_fail("invalid shared address space");
    source = owner(source);
    if (task == source)
        lume_fail("invalid shared address space");
    lume_user_buffer_invalidate_root(task);
    shared[task] = source + 1;
    jit_depth[task] = 0;
}

void lume_mmu_release_user(unsigned task)
{
    if (task >= LUME_TASK_LIMIT)
        return;
    jit_depth[task] = 0;
    if (shared[task])
    {
        shared[task] = 0;
        return;
    }
    lume_user_buffer_invalidate_root(task);
    for (unsigned view = 0; view < 2; ++view)
    {
        task_pdpt[task][view][1] = 0;
        for (unsigned i = RAM_END >> 30; i < 512; ++i)
            task_pdpt[task][view][i] = 0;
        for (unsigned i = 1; i < 128; ++i)
            task_root[task][view][i] = 0;
    }
    lume_arch_flush_user_tlb();
    unsigned count = 0;
    while (sparse[task])
    {
        struct sparse_table *entry = sparse[task];
        sparse[task] = entry->next;
        if (!lume_page_free((uintptr_t)entry->entries))
            lume_fail("sparse page table release");
        lume_kernel_free(entry);
        if (++count % 16 == 0)
            lume_worker_checkpoint(65536);
    }
    for (unsigned view = 0; view < 2; ++view)
        for (unsigned i = 0; i < USER_BLOCKS; ++i)
        {
            if (task_pt[task][view][i] && !lume_page_free((uintptr_t)task_pt[task][view][i]))
                lume_fail("user page table release");
            task_pt[task][view][i] = NULL;
        }
    memset(task_root[task], 0, sizeof(task_root[task]));
    memset(task_pdpt[task], 0, sizeof(task_pdpt[task]));
    memset(task_pd[task], 0, sizeof(task_pd[task]));
}

static bool user_address(uint64_t va)
{
    return (va >= LUME_USER_BASE && va < LUME_USER_LIMIT) ||
           (va >= LUME_LARGE_ELF_BASE && va < LUME_LARGE_ELF_LIMIT) ||
           (va >= LUME_HEAP_EXTENDED_BASE && va < LUME_HEAP_EXTENDED_LIMIT) || lume_vm_address(va);
}

static bool sparse_address(uint64_t va)
{
    return user_address(va) && !(va >= LUME_USER_BASE && va < LUME_USER_LIMIT);
}

static uint64_t *entry(unsigned task, unsigned view, uint64_t va, bool create)
{
    task = owner(task);
    if (task >= LUME_TASK_LIMIT || !user_address(va) || !task_root[task][view][0])
        return NULL;
    if (sparse_address(va))
    {
        uint64_t *entries = task_root[task][view];
        const unsigned shifts[] = {39, 30, 21};
        for (unsigned level = 0; level < 3; ++level)
        {
            uint64_t *value = entries + ((va >> shifts[level]) & 511);
            if (!*value && create)
            {
                struct sparse_table *node = lume_kernel_allocate(sizeof(*node));
                if (!node)
                    return NULL;
                node->entries = (void *)(uintptr_t)lume_page_alloc(0);
                if (!node->entries)
                {
                    lume_kernel_free(node);
                    return NULL;
                }
                node->next = sparse[task];
                sparse[task] = node;
                *value = table(node->entries, true);
            }
            if (!(*value & PRESENT) || (*value & HUGE))
                return NULL;
            entries = (void *)(uintptr_t)(*value & ADDRESS);
        }
        return entries + ((va >> 12) & 511);
    }
    if (va < LUME_USER_BASE || va >= LUME_USER_LIMIT)
        return NULL;
    unsigned block = (unsigned)((va - LUME_USER_BASE) / BLOCK);
    if (!task_pt[task][view][block] && create)
    {
        uint64_t page = lume_page_alloc(0);
        if (!page)
            return NULL;
        task_pt[task][view][block] = (void *)(uintptr_t)page;
        task_pd[task][view][block] = page | PRESENT | WRITE | USER;
    }
    return task_pt[task][view][block] ? &task_pt[task][view][block][(va >> 12) & 511] : NULL;
}

static void detach_empty(unsigned task, unsigned view, uint64_t va, struct sparse_table **retired)
{
    if (!sparse_address(va))
        return;
    task = owner(task);
    if (task >= LUME_TASK_LIMIT || !task_root[task][view][0])
        return;
    uint64_t *entries = task_root[task][view], *parents[3], *children[3];
    const unsigned shifts[] = {39, 30, 21};
    unsigned depth = 0;
    for (; depth < 3; ++depth)
    {
        parents[depth] = entries + ((va >> shifts[depth]) & 511);
        if (!(*parents[depth] & PRESENT) || (*parents[depth] & HUGE))
            break;
        children[depth] = (void *)(uintptr_t)(*parents[depth] & ADDRESS);
        entries = children[depth];
    }
    while (depth)
    {
        --depth;
        bool empty = true;
        for (unsigned i = 0; i < 512; ++i)
            empty &= children[depth][i] == 0;
        if (!empty)
            break;
        struct sparse_table **position = &sparse[task];
        while (*position && (*position)->entries != children[depth])
            position = &(*position)->next;
        if (!*position)
            break;
        struct sparse_table *node = *position;
        *position = node->next;
        *parents[depth] = 0;
        node->next = *retired;
        *retired = node;
    }
}

static void free_tables(struct sparse_table *retired)
{
    while (retired)
    {
        struct sparse_table *node = retired;
        retired = node->next;
        if (!lume_page_free((uintptr_t)node->entries))
            lume_fail("sparse page table release");
        lume_kernel_free(node);
    }
}

static uint64_t detach_low_empty(unsigned task, unsigned view, uint64_t va)
{
    task = owner(task);
    if (task >= LUME_TASK_LIMIT || va < LUME_USER_BASE || va >= LUME_USER_LIMIT)
        return 0;
    unsigned block = (unsigned)((va - LUME_USER_BASE) / BLOCK);
    uint64_t *entries = task_pt[task][view][block];
    if (!entries)
        return 0;
    for (unsigned i = 0; i < 512; ++i)
        if (entries[i])
            return 0;
    task_pd[task][view][block] = 0;
    task_pt[task][view][block] = NULL;
    return (uintptr_t)entries;
}

static bool map_user_views(unsigned task, uint64_t va, uint64_t pa, bool write, bool execute,
                           bool jit)
{
    if (task >= LUME_TASK_LIMIT || !user_address(va) || !pa || pa >= RAM_END ||
        ((va | pa) & 4095) || (write && execute && !jit))
        return false;
    uint64_t *entries[2];
    entries[0] = entry(task, 0, va, true);
    entries[1] = entry(task, 1, va, true);
    if (!entries[0] || !entries[1])
    {
        struct sparse_table *retired = NULL;
        uint64_t low[2];
        for (unsigned view = 0; view < 2; ++view)
        {
            detach_empty(task, view, va, &retired);
            low[view] = detach_low_empty(task, view, va);
        }
        if (retired || low[0] || low[1])
            lume_arch_flush_user_tlb();
        free_tables(retired);
        for (unsigned view = 0; view < 2; ++view)
            if (low[view] && !lume_page_free(low[view]))
                lume_fail("user page table release");
        return false;
    }
    if (*entries[0] || *entries[1])
        return false;
    lume_user_buffer_invalidate(owner(task), va, 4096);
    for (unsigned view = 0; view < 2; ++view)
    {
        bool writable = write && (!jit || !execute || view == 1);
        bool executable = execute && (!jit || view == 0);
        *entries[view] = pa | PRESENT | USER | (writable ? WRITE : 0) | (executable ? 0 : NX);
    }
    return true;
}

bool lume_mmu_map_user(unsigned task, uint64_t va, uint64_t pa, bool write, bool execute)
{
    return map_user_views(task, va, pa, write, execute, false);
}

bool lume_mmu_map_user_jit(unsigned task, uint64_t va, uint64_t pa, bool write, bool execute)
{
    return map_user_views(task, va, pa, write, execute, true);
}

bool lume_mmu_protect_user_batch(unsigned task, const uint64_t *addresses, const uint64_t *physical,
                                 unsigned count, unsigned protection)
{
    if (task >= LUME_TASK_LIMIT || !addresses || !physical || !count ||
        count > LUME_MMU_BATCH_PAGES || (protection & ~7u) || (protection & 6u) == 6u ||
        (protection && !(protection & LUME_PROT_READ)))
        return false;
    uint64_t *entries[LUME_MMU_BATCH_PAGES][2], values[LUME_MMU_BATCH_PAGES][2];
    for (unsigned i = 0; i < count; ++i)
    {
        uint64_t va = addresses[i];
        if (!user_address(va) || (va & 4095) || !physical[i] || (physical[i] & 4095))
            return false;
        for (unsigned view = 0; view < 2; ++view)
        {
            entries[i][view] = entry(task, view, va, false);
            if (!entries[i][view] || !(*entries[i][view] & PRESENT) ||
                (*entries[i][view] & ADDRESS) != physical[i])
                return false;
            values[i][view] = (*entries[i][view] & ~(USER | WRITE | NX)) |
                              ((protection & LUME_PROT_READ) ? USER : 0) |
                              ((protection & LUME_PROT_WRITE) ? WRITE : 0) |
                              ((protection & LUME_PROT_EXEC) ? 0 : NX);
        }
    }
    for (unsigned i = 0; i < count; ++i)
        lume_user_buffer_invalidate(owner(task), addresses[i], 4096);
    for (unsigned i = 0; i < count; ++i)
        for (unsigned view = 0; view < 2; ++view)
            *entries[i][view] = values[i][view];
    lume_arch_flush_user_tlb();
    return true;
}

bool lume_mmu_unmap_user_batch(unsigned task, const uint64_t *addresses, unsigned count)
{
    if (task >= LUME_TASK_LIMIT || !addresses || !count || count > LUME_MMU_BATCH_PAGES)
        return false;
    uint64_t *entries[LUME_MMU_BATCH_PAGES][2];
    for (unsigned i = 0; i < count; ++i)
    {
        uint64_t va = addresses[i];
        if (!user_address(va) || (va & 4095))
            return false;
        for (unsigned view = 0; view < 2; ++view)
        {
            entries[i][view] = entry(task, view, va, false);
            if (!entries[i][view] || !(*entries[i][view] & PRESENT))
                return false;
        }
    }
    for (unsigned i = 0; i < count; ++i)
        lume_user_buffer_invalidate(owner(task), addresses[i], 4096);
    for (unsigned i = 0; i < count; ++i)
        for (unsigned view = 0; view < 2; ++view)
            *entries[i][view] = 0;
    struct sparse_table *retired = NULL;
    for (unsigned i = 0; i < count; ++i)
        for (unsigned view = 0; view < 2; ++view)
            detach_empty(task, view, addresses[i], &retired);
    lume_arch_flush_user_tlb();
    free_tables(retired);
    return true;
}

void lume_mmu_unmap_user(unsigned task, uint64_t va)
{
    (void)lume_mmu_unmap_user_batch(task, &va, 1);
}

bool lume_mmu_user_root(unsigned task, uint64_t data, const uint64_t *stack)
{
    if (task >= LUME_TASK_LIMIT || !data || (data & 4095))
        return false;
    for (unsigned i = 0; i < LUME_STACK_PAGES; ++i)
        if (!stack[i] || (stack[i] & 4095))
            return false;
    lume_mmu_release_user(task);
    for (unsigned view = 0; view < 2; ++view)
    {
        memcpy(task_root[task][view], root, sizeof(root));
        memcpy(task_pdpt[task][view], pdpt, sizeof(pdpt));
        memcpy(task_pd[task][view], directories[1], sizeof(task_pd[task][view]));
        task_root[task][view][0] = table(task_pdpt[task][view], true);
        task_pdpt[task][view][1] = table(task_pd[task][view], true);
    }
    if (!lume_mmu_map_user(task, LUME_USER_BASE, (uintptr_t)__user_code_start, false, true) ||
        !lume_mmu_map_user(task, LUME_USER_DATA, data, true, false))
        goto fail;
    for (unsigned i = 0; i < LUME_STACK_PAGES; ++i)
        if (!lume_mmu_map_user(task, LUME_USER_STACK + (uint64_t)i * 4096, stack[i], true, false))
            goto fail;
    return true;
fail:
    lume_mmu_release_user(task);
    return false;
}

bool lume_mmu_swap_user(unsigned a, unsigned b)
{
    if (a >= LUME_TASK_LIMIT || b >= LUME_TASK_LIMIT || a == b || shared[a] || shared[b])
        return false;
    lume_user_buffer_invalidate_root(a);
    lume_user_buffer_invalidate_root(b);
    for (unsigned view = 0; view < 2; ++view)
    {
        for (unsigned i = 0; i < 512; ++i)
        {
            uint64_t value = task_pd[a][view][i];
            task_pd[a][view][i] = task_pd[b][view][i];
            task_pd[b][view][i] = value;
        }
        for (unsigned i = 0; i < USER_BLOCKS; ++i)
        {
            uint64_t *value = task_pt[a][view][i];
            task_pt[a][view][i] = task_pt[b][view][i];
            task_pt[b][view][i] = value;
        }
        for (unsigned i = RAM_END >> 30; i < 512; ++i)
        {
            uint64_t value = task_pdpt[a][view][i];
            task_pdpt[a][view][i] = task_pdpt[b][view][i];
            task_pdpt[b][view][i] = value;
        }
        for (unsigned i = 1; i < 128; ++i)
        {
            uint64_t value = task_root[a][view][i];
            task_root[a][view][i] = task_root[b][view][i];
            task_root[b][view][i] = value;
        }
    }
    struct sparse_table *tables = sparse[a];
    sparse[a] = sparse[b];
    sparse[b] = tables;
    lume_arch_flush_user_tlb();
    return true;
}

bool lume_user_buffer(uint64_t pointer, uint64_t bytes, bool write)
{
    if (!bytes)
        return true;
    if (pointer > UINT64_MAX - bytes)
        return false;
    uint64_t active;
    __asm__ volatile("movq %%cr3, %0" : "=r"(active));
    uint64_t end = pointer + bytes;
    for (uint64_t p = pointer; p < end; p = (p & ~UINT64_C(4095)) + 4096)
    {
        uint64_t physical;
        if (!lume_mmu_translate(active, p, write, true, false, &physical) &&
            !lume_vm_resolve_buffer(active, p, write))
            return false;
    }
    return true;
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
    __asm__ volatile("mfence" : : : "memory");
}

void lume_sync_code(uint64_t address, size_t bytes)
{
    lume_sync_code_defer(address, bytes);
    lume_sync_code_finish();
}

bool lume_mmu_device_mapping(uint64_t address)
{
    return address >= 0xc0000000 && address < 0x100000000;
}

bool lume_mmu_module_prepare(uint64_t physical, size_t bytes)
{
    if (module_end || !physical || !bytes || ((physical | bytes) & 4095) || bytes > BLOCK ||
        physical + bytes >= LUME_USER_BASE)
        return false;
    module_first = physical & ~(BLOCK - 1);
    uint64_t last = (physical + bytes + BLOCK - 1) & ~(BLOCK - 1);
    for (uint64_t base = module_first; base < last; base += BLOCK)
    {
        unsigned index = (unsigned)((base - module_first) / BLOCK);
        uint64_t *old = &directories[base >> 30][(base >> 21) & 511];
        if (!(*old & HUGE))
            return false;
        for (unsigned n = 0; n < 512; ++n)
            module_pages[index][n] = base + n * UINT64_C(4096) | PRESENT | WRITE | NX;
        *old = table(module_pages[index], false);
    }
    module_base = physical;
    module_end = physical + bytes;
    lume_arch_flush_user_tlb();
    return true;
}

bool lume_mmu_module_protect(uint64_t physical, size_t bytes, unsigned flags)
{
    if (!bytes || ((physical | bytes) & 4095) || physical < module_base || physical >= module_end ||
        bytes > module_end - physical || !(flags & LUME_MODULE_READ) || (flags & ~7u) ||
        ((flags & (LUME_MODULE_WRITE | LUME_MODULE_EXECUTE)) ==
         (LUME_MODULE_WRITE | LUME_MODULE_EXECUTE)))
        return false;
    for (uint64_t p = physical; p < physical + bytes; p += 4096)
        module_pages[(p - module_first) / BLOCK][(p >> 12) & 511] =
            p | PRESENT | (flags & LUME_MODULE_WRITE ? WRITE : 0) |
            (flags & LUME_MODULE_EXECUTE ? 0 : NX);
    lume_arch_flush_user_tlb();
    return true;
}
