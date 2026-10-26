#include "lume/mmu.h"
#include "lume/elf.h"
#include "lume/module_memory.h"
#include "lume/acpi.h"
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
static _Alignas(4096) uint64_t task_root[LUME_TASK_LIMIT][512];
static _Alignas(4096) uint64_t task_pdpt[LUME_TASK_LIMIT][512];
static _Alignas(4096) uint64_t task_pd[LUME_TASK_LIMIT][512];
static uint64_t *task_pt[LUME_TASK_LIMIT][USER_BLOCKS];
static unsigned shared[LUME_TASK_LIMIT];
static uint64_t module_base, module_end, module_first;
extern unsigned char __image_start[], __text_end[], __readonly_end[], __image_end[];
extern unsigned char __stack_guard[], __worker_guard[], __trap_guard[], __fault_guard[];

static uint64_t table(const uint64_t *p, bool user)
{
    return (uintptr_t)p | PRESENT | WRITE | (user ? USER : 0);
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
    if (!physical || !out || va >= RAM_END)
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
    return shared[task] ? shared[task] - 1 : task;
}

uint64_t lume_mmu_task_root(unsigned task)
{
    return task < LUME_TASK_LIMIT ? (uintptr_t)task_root[owner(task)] : 0;
}

void lume_mmu_share_user(unsigned task, unsigned source)
{
    if (task >= LUME_TASK_LIMIT || source >= LUME_TASK_LIMIT || task == source)
        lume_fail("invalid shared address space");
    shared[task] = owner(source) + 1;
}

void lume_mmu_release_user(unsigned task)
{
    if (task >= LUME_TASK_LIMIT)
        return;
    if (shared[task])
    {
        shared[task] = 0;
        return;
    }
    for (unsigned i = 0; i < USER_BLOCKS; ++i)
    {
        if (task_pt[task][i] && !lume_page_free((uintptr_t)task_pt[task][i]))
            lume_fail("user page table release");
        task_pt[task][i] = NULL;
    }
    memset(task_root[task], 0, sizeof(task_root[task]));
    memset(task_pdpt[task], 0, sizeof(task_pdpt[task]));
    memset(task_pd[task], 0, sizeof(task_pd[task]));
}

static uint64_t *entry(unsigned task, uint64_t va, bool create)
{
    task = owner(task);
    unsigned block = (unsigned)((va - LUME_USER_BASE) / BLOCK);
    if (!task_pt[task][block] && create)
    {
        uint64_t page = lume_page_alloc(0);
        if (!page)
            return NULL;
        task_pt[task][block] = (void *)(uintptr_t)page;
        task_pd[task][block] = page | PRESENT | WRITE | USER;
    }
    return task_pt[task][block] ? &task_pt[task][block][(va >> 12) & 511] : NULL;
}

bool lume_mmu_map_user(unsigned task, uint64_t va, uint64_t pa, bool write, bool execute)
{
    if (task >= LUME_TASK_LIMIT || va < LUME_USER_BASE || va >= LUME_USER_LIMIT || !pa ||
        pa >= RAM_END || ((va | pa) & 4095) || (write && execute))
        return false;
    uint64_t *p = entry(task, va, true);
    if (!p || *p)
        return false;
    *p = pa | PRESENT | USER | (write ? WRITE : 0) | (execute ? 0 : NX);
    return true;
}

void lume_mmu_unmap_user(unsigned task, uint64_t va)
{
    if (task >= LUME_TASK_LIMIT || va < LUME_USER_BASE || va >= LUME_USER_LIMIT || (va & 4095))
        return;
    uint64_t *p = entry(task, va, false);
    if (p)
    {
        *p = 0;
        __asm__ volatile("invlpg (%0)" : : "r"(va) : "memory");
    }
}

bool lume_mmu_user_root(unsigned task, uint64_t data, const uint64_t *stack)
{
    if (task >= LUME_TASK_LIMIT || !data || (data & 4095))
        return false;
    lume_mmu_release_user(task);
    memcpy(task_root[task], root, sizeof(root));
    memcpy(task_pdpt[task], pdpt, sizeof(pdpt));
    memcpy(task_pd[task], directories[1], sizeof(task_pd[task]));
    task_root[task][0] = table(task_pdpt[task], true);
    task_pdpt[task][1] = table(task_pd[task], true);
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
    for (unsigned i = 0; i < 512; ++i)
    {
        uint64_t value = task_pd[a][i];
        task_pd[a][i] = task_pd[b][i];
        task_pd[b][i] = value;
    }
    for (unsigned i = 0; i < USER_BLOCKS; ++i)
    {
        uint64_t *value = task_pt[a][i];
        task_pt[a][i] = task_pt[b][i];
        task_pt[b][i] = value;
    }
    lume_arch_flush_user_tlb();
    return true;
}

bool lume_user_buffer(uint64_t pointer, uint64_t bytes, bool write)
{
    if (!bytes)
        return true;
    if (pointer < LUME_USER_BASE || pointer >= LUME_USER_LIMIT || bytes > LUME_USER_LIMIT - pointer)
        return false;
    uint64_t active;
    __asm__ volatile("movq %%cr3, %0" : "=r"(active));
    uint64_t end = pointer + bytes;
    for (uint64_t p = pointer; p < end; p = (p & ~UINT64_C(4095)) + 4096)
    {
        uint64_t physical;
        if (!lume_mmu_translate(active, p, write, true, false, &physical))
            return false;
    }
    return true;
}

void lume_sync_code(uint64_t address, size_t bytes)
{
    (void)address;
    (void)bytes;
    __asm__ volatile("mfence" : : : "memory");
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
