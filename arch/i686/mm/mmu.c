#include "lume/mmu.h"
#include "lume/user_buffer.h"
#include "lume/elf.h"
#include "lume/vm.h"
#include "lume/paging.h"
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
static _Alignas(4096) uint32_t task_root[LUME_TASK_LIMIT][2][1024];
static uint32_t *task_pt[LUME_TASK_LIMIT][2][USER_BLOCKS];
static unsigned shared[LUME_TASK_LIMIT], jit_depth[LUME_TASK_LIMIT];
extern unsigned char __image_start[], __text_end[], __readonly_end[], __image_end[];
extern unsigned char __stack_guard[], __worker_guard[], __trap_guard[], __fault_guard[];

uint64_t lume_mmu_root(void)
{
    return (uintptr_t)root;
}

void lume_mmu_switch(uint64_t physical)
{
    lume_i686_paging_switch((uint32_t)physical);
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
    lume_i686_paging_enable((uintptr_t)root);
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
    physical &= ADDRESS;
    return physical && (physical == lume_mmu_task_view_root(task, false) ||
                        physical == lume_mmu_task_view_root(task, true));
}

bool lume_mmu_root_active(uint64_t physical)
{
    return lume_i686_paging_root() == physical;
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
    lume_user_buffer_invalidate_root(task);
    source = owner(source);
    if (task == source)
        lume_fail("invalid shared address space");
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
    bool mapped = false;
    for (unsigned view = 0; view < 2; ++view)
        for (unsigned i = 0; i < USER_BLOCKS; ++i)
        {
            unsigned slot = (unsigned)(LUME_USER_BASE / BLOCK) + i;
            mapped |= task_root[task][view][slot] != 0;
            task_root[task][view][slot] = 0;
        }
    if (mapped)
        lume_arch_flush_user_tlb();
    for (unsigned view = 0; view < 2; ++view)
        for (unsigned i = 0; i < USER_BLOCKS; ++i)
        {
            if (task_pt[task][view][i] && !lume_page_free((uintptr_t)task_pt[task][view][i]))
                lume_fail("user page table release");
            task_pt[task][view][i] = NULL;
        }
    memset(task_root[task], 0, sizeof(task_root[task]));
}

static uint32_t *entry(unsigned task, unsigned view, uint64_t va, bool create)
{
    task = owner(task);
    unsigned block = (unsigned)((va - LUME_USER_BASE) / BLOCK);
    if (!task_pt[task][view][block] && create)
    {
        uint64_t page = lume_page_alloc(0);
        if (!page)
            return NULL;
        task_pt[task][view][block] = (void *)(uintptr_t)page;
        task_root[task][view][(unsigned)(va >> 22)] = (uint32_t)page | PRESENT | WRITE | USER;
    }
    return task_pt[task][view][block] ? &task_pt[task][view][block][(va >> 12) & 1023] : NULL;
}

static bool map_user_views(unsigned task, uint64_t va, uint64_t pa, bool write, bool execute,
                           bool jit)
{
    if (task >= LUME_TASK_LIMIT || va < LUME_USER_BASE || va >= LUME_USER_LIMIT || !pa ||
        pa >= UINT64_C(0x80000000) || ((va | pa) & 4095) || (write && execute && !jit))
        return false;
    uint32_t *entries[2] = {entry(task, 0, va, true), entry(task, 1, va, true)};
    if (!entries[0] || !entries[1] || *entries[0] || *entries[1])
        return false;
    lume_user_buffer_invalidate(owner(task), va, 4096);
    for (unsigned view = 0; view < 2; ++view)
    {
        bool writable = write && (!jit || !execute || view == 1);
        bool executable = execute && (!jit || view == 0);
        *entries[view] =
            (uint32_t)pa | PRESENT | USER | (writable ? WRITE : 0) | (executable ? EXECUTE : 0);
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
    uint32_t *entries[LUME_MMU_BATCH_PAGES][2], values[LUME_MMU_BATCH_PAGES][2];
    for (unsigned i = 0; i < count; ++i)
    {
        uint64_t va = addresses[i];
        if ((va < LUME_USER_BASE || va >= LUME_USER_LIMIT) || (va & 4095) || !physical[i] ||
            (physical[i] & 4095))
            return false;
        for (unsigned view = 0; view < 2; ++view)
        {
            entries[i][view] = entry(task, view, va, false);
            if (!entries[i][view] || !(*entries[i][view] & PRESENT) ||
                (*entries[i][view] & ADDRESS) != physical[i])
                return false;
            values[i][view] = (*entries[i][view] & ~(USER | WRITE | EXECUTE)) |
                              ((protection & LUME_PROT_READ) ? USER : 0) |
                              ((protection & LUME_PROT_WRITE) ? WRITE : 0) |
                              ((protection & LUME_PROT_EXEC) ? EXECUTE : 0);
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
    if (task >= LUME_TASK_LIMIT || !addresses || !count || count > LUME_MMU_UNMAP_PAGES)
        return false;
    uint32_t *entries[LUME_MMU_UNMAP_PAGES][2];
    for (unsigned i = 0; i < count; ++i)
    {
        uint64_t va = addresses[i];
        if (va < LUME_USER_BASE || va >= LUME_USER_LIMIT || (va & 4095))
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
    if (count == 1)
        lume_i686_paging_invalidate((uintptr_t)addresses[0]);
    else
        lume_arch_flush_user_tlb();
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
    lume_mmu_release_user(task);
    for (unsigned view = 0; view < 2; ++view)
        memcpy(task_root[task][view], root, sizeof(root));
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
        for (unsigned i = 0; i < USER_BLOCKS; ++i)
        {
            unsigned slot = (unsigned)(LUME_USER_BASE / BLOCK) + i;
            uint32_t value = task_root[a][view][slot];
            task_root[a][view][slot] = task_root[b][view][slot];
            task_root[b][view][slot] = value;
            uint32_t *page = task_pt[a][view][i];
            task_pt[a][view][i] = task_pt[b][view][i];
            task_pt[b][view][i] = page;
        }
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
    uint32_t active = lume_i686_paging_root();
    for (uint64_t p = pointer, end = pointer + bytes; p < end; p = (p & ~UINT64_C(4095)) + 4096)
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
    __asm__ volatile("lock\naddl $0, (%%esp)" : : : "memory", "cc");
}

void lume_sync_code(uint64_t address, size_t bytes)
{
    lume_sync_code_defer(address, bytes);
    lume_sync_code_finish();
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
