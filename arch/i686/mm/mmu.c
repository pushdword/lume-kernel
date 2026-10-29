#include "lume/mmu.h"
#include "lume/elf.h"
#include <string.h>
#define PRESENT 1u
#define WRITE 2u
#define USER 4u
#define HUGE 128u
/* Bit 9 records executable mappings on processors without execute-disable support. */
#define EXECUTE 512u
#define ADDRESS UINT32_C(0xfffff000)
#define BLOCK UINT32_C(0x400000)
#define USER_BLOCKS ((LUME_USER_LIMIT - LUME_USER_BASE) / BLOCK)
static _Alignas(4096) uint32_t root[1024], image_pages[8][1024];
static _Alignas(4096) uint32_t task_root[LUME_TASK_LIMIT][1024];
static uint32_t *task_pt[LUME_TASK_LIMIT][USER_BLOCKS];
static unsigned shared[LUME_TASK_LIMIT];
extern unsigned char __image_start[], __text_end[], __readonly_end[], __image_end[];
extern unsigned char __stack_guard[], __worker_guard[], __trap_guard[], __fault_guard[];

uint64_t lume_mmu_root(void)
{
    return (uintptr_t)root;
}

void lume_mmu_switch(uint64_t physical)
{
    uint32_t value = (uint32_t)physical;
    __asm__ volatile("movl %0, %%cr3" : : "r"(value) : "memory");
}

bool lume_mmu_start(const struct lume_fdt *description)
{
    if (description->initrd_start < (uintptr_t)__image_end ||
        description->initrd_end <= description->initrd_start ||
        description->initrd_end > UINT64_C(0x40000000))
        return false;
    for (unsigned b = 0; b < 512; ++b)
    {
        uint64_t base = (uint64_t)b * BLOCK;
        if (base >= LUME_USER_BASE && base < LUME_USER_LIMIT)
            continue;
        for (unsigned i = 0; i < description->ram_count; ++i)
            if (base < description->ram[i].base + description->ram[i].size &&
                description->ram[i].base < base + BLOCK)
                root[b] = (uint32_t)base | PRESENT | WRITE | HUGE;
    }
    for (unsigned b = 768; b < 1024; ++b)
        root[b] = b * BLOCK | PRESENT | WRITE | HUGE | 0x18;
    unsigned end = ((uintptr_t)__image_end + BLOCK - 1) / BLOCK;
    if (end > 8)
        return false;
    for (unsigned b = 0; b < end; ++b)
    {
        for (unsigned n = 0; n < 1024; ++n)
        {
            uintptr_t address = b * BLOCK + n * 4096;
            uint32_t flags = PRESENT | WRITE;
            if (address < (uintptr_t)__image_start)
                flags = address == 0xb8000 ? PRESENT | WRITE | 0x18 : 0;
            else if (address < (uintptr_t)__text_end)
                flags = PRESENT | EXECUTE;
            else if (address < (uintptr_t)__readonly_end)
                flags = PRESENT;
            if (address >= (uintptr_t)__image_end || address == (uintptr_t)__stack_guard ||
                address == (uintptr_t)__worker_guard || address == (uintptr_t)__trap_guard ||
                address == (uintptr_t)__fault_guard)
                flags = 0;
            if (address >= description->initrd_start && address < description->initrd_end)
                flags = PRESENT;
            image_pages[b][n] = flags ? address | flags : 0;
        }
        root[b] = (uintptr_t)image_pages[b] | PRESENT | WRITE;
    }
    uint32_t control;
    __asm__ volatile("movl %%cr4, %0" : "=r"(control));
    control |= 0x10;
    __asm__ volatile("movl %0, %%cr4" : : "r"(control) : "memory");
    lume_mmu_switch((uintptr_t)root);
    __asm__ volatile("movl %%cr0, %0" : "=r"(control));
    control |= 0x80010000;
    __asm__ volatile("movl %0, %%cr0" : : "r"(control) : "memory");
    return true;
}

bool lume_mmu_translate(uint64_t physical, uint64_t va, bool write, bool user, bool execute,
                        uint64_t *out)
{
    if (!physical || physical > UINT32_MAX || va > UINT32_MAX || !out)
        return false;
    const uint32_t *directory = (const void *)(uintptr_t)(physical & ADDRESS);
    uint32_t pde = directory[va >> 22];
    if (!(pde & PRESENT) || (write && !(pde & WRITE)) || (user && !(pde & USER)))
        return false;
    uint32_t leaf = pde;
    unsigned mask = BLOCK - 1;
    if (!(pde & HUGE))
    {
        const uint32_t *pages = (const void *)(uintptr_t)(pde & ADDRESS);
        leaf = pages[(va >> 12) & 1023];
        mask = 4095;
    }
    if (!(leaf & PRESENT) || (write && !(leaf & WRITE)) || (user && !(leaf & USER)) ||
        (execute && !(leaf & EXECUTE)))
        return false;
    *out = (leaf & ADDRESS & ~mask) | ((uint32_t)va & mask);
    return true;
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
}

static uint32_t *entry(unsigned task, uint64_t va, bool create)
{
    task = owner(task);
    unsigned block = (unsigned)((va - LUME_USER_BASE) / BLOCK);
    if (!task_pt[task][block] && create)
    {
        uint64_t page = lume_page_alloc(0);
        if (!page)
            return NULL;
        task_pt[task][block] = (void *)(uintptr_t)page;
        task_root[task][(unsigned)(va >> 22)] = (uint32_t)page | PRESENT | WRITE | USER;
    }
    return task_pt[task][block] ? &task_pt[task][block][(va >> 12) & 1023] : NULL;
}

bool lume_mmu_map_user(unsigned task, uint64_t va, uint64_t pa, bool write, bool execute)
{
    if (task >= LUME_TASK_LIMIT || va < LUME_USER_BASE || va >= LUME_USER_LIMIT || !pa ||
        pa >= UINT64_C(0x80000000) || ((va | pa) & 4095) || (write && execute))
        return false;
    uint32_t *p = entry(task, va, true);
    if (!p || *p)
        return false;
    *p = (uint32_t)pa | PRESENT | USER | (write ? WRITE : 0) | (execute ? EXECUTE : 0);
    return true;
}

void lume_mmu_unmap_user(unsigned task, uint64_t va)
{
    if (task >= LUME_TASK_LIMIT || va < LUME_USER_BASE || va >= LUME_USER_LIMIT || (va & 4095))
        return;
    uint32_t *p = entry(task, va, false);
    if (p)
    {
        *p = 0;
        __asm__ volatile("invlpg (%0)" : : "r"((uintptr_t)va) : "memory");
    }
}

bool lume_mmu_user_root(unsigned task, uint64_t data, const uint64_t *stack)
{
    if (task >= LUME_TASK_LIMIT || !data || (data & 4095))
        return false;
    lume_mmu_release_user(task);
    memcpy(task_root[task], root, sizeof(root));
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
    for (unsigned i = 0; i < USER_BLOCKS; ++i)
    {
        unsigned slot = (unsigned)(LUME_USER_BASE / BLOCK) + i;
        uint32_t value = task_root[a][slot];
        task_root[a][slot] = task_root[b][slot];
        task_root[b][slot] = value;
        uint32_t *page = task_pt[a][i];
        task_pt[a][i] = task_pt[b][i];
        task_pt[b][i] = page;
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
    uint32_t active;
    __asm__ volatile("movl %%cr3, %0" : "=r"(active));
    for (uint64_t p = pointer, end = pointer + bytes; p < end; p = (p & ~UINT64_C(4095)) + 4096)
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
    __asm__ volatile("lock\naddl $0, (%%esp)" : : : "memory", "cc");
}

bool lume_mmu_device_mapping(uint64_t address)
{
    return address >= 0xc0000000 && address < UINT64_C(0x100000000);
}

bool lume_mmu_module_prepare(uint64_t physical, size_t bytes)
{
    (void)physical;
    (void)bytes;
    return false;
}

bool lume_mmu_module_protect(uint64_t physical, size_t bytes, unsigned flags)
{
    (void)physical;
    (void)bytes;
    (void)flags;
    return false;
}
