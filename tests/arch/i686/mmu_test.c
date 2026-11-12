#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../../arch/i686/mm/mmu.c"

unsigned char __image_start[1], __text_end[1], __readonly_end[1], __image_end[1];
unsigned char __stack_guard[1], __worker_guard[1], __trap_guard[1], __fault_guard[1];
_Alignas(4096) unsigned char __user_code_start[4096], __user_code_end[1];
static _Alignas(4096) unsigned char pool[64][4096];
static unsigned used[64], pages, denied;

uint64_t lume_page_alloc(uint64_t minimum)
{
    (void)minimum;
    if (denied)
        return 0;
    for (unsigned i = 0; i < 64; ++i)
        if (!used[i])
        {
            used[i] = 1;
            ++pages;
            memset(pool[i], 0, 4096);
            return (uintptr_t)pool[i];
        }
    return 0;
}

bool lume_page_free(uint64_t physical)
{
    for (unsigned i = 0; i < 64; ++i)
        if (physical == (uintptr_t)pool[i] && used[i])
        {
            used[i] = 0;
            --pages;
            return true;
        }
    return false;
}

_Noreturn void lume_fail(const char *why)
{
    fprintf(stderr, "%s\n", why);
    abort();
}

void lume_user_buffer_invalidate_root(unsigned task)
{
    (void)task;
}

void lume_user_buffer_invalidate(unsigned task, uint64_t address, uint64_t bytes)
{
    (void)task;
    (void)address;
    (void)bytes;
}

bool lume_vm_resolve_buffer(uint64_t root, uint64_t address, bool write)
{
    (void)root;
    (void)address;
    (void)write;
    return false;
}

int main(void)
{
    assert((uintptr_t)pool < UINT32_C(0x80000000));
    root[0] = PRESENT;
    memcpy(task_root[0][0], root, sizeof(root));
    memcpy(task_root[0][1], root, sizeof(root));
    memcpy(task_root[2][0], root, sizeof(root));
    memcpy(task_root[2][1], root, sizeof(root));
    uint64_t address = LUME_VM_BASE, frame = (uintptr_t)pool[63], physical;
    assert(lume_mmu_map_user_jit(0, address, frame, true, true));
    uint64_t reader = lume_mmu_task_view_root(0, false);
    uint64_t writer = lume_mmu_task_view_root(0, true);
    assert(reader && writer && reader != writer);
    assert(lume_mmu_translate(reader, address, false, true, true, &physical) && physical == frame);
    assert(!lume_mmu_translate(reader, address, true, true, false, &physical));
    assert(lume_mmu_translate(writer, address, true, true, false, &physical));
    assert(!lume_mmu_translate(writer, address, false, true, true, &physical));
    lume_mmu_switch(reader);
    assert(lume_mmu_jit_scope(0, false) == -LUME_EINVAL);
    assert(!lume_mmu_jit_scope(0, true) && lume_mmu_jit_active(0));
    assert(!lume_mmu_jit_scope(0, true));
    assert(!lume_mmu_jit_scope(0, false) && lume_mmu_jit_active(0));
    lume_mmu_share_user(1, 0);
    assert(lume_mmu_task_root(1) == reader && lume_mmu_task_root(0) == writer);
    assert(lume_mmu_root_matches(1, reader) && lume_mmu_root_matches(1, writer));
    lume_mmu_jit_copy(1, 0);
    assert(lume_mmu_task_root(1) == writer);
    assert(!lume_mmu_jit_scope(0, false) && !lume_mmu_jit_active(0));
    assert(lume_mmu_protect_user_batch(0, &address, &frame, 1, LUME_PROT_READ));
    assert(!lume_mmu_translate(reader, address, true, true, false, &physical));
    assert(!lume_mmu_translate(writer, address, true, true, false, &physical));
    assert(lume_mmu_unmap_user_batch(1, &address, 1));
    assert(!lume_mmu_translate(reader, address, false, true, false, &physical));
    assert(!lume_mmu_translate(writer, address, false, true, false, &physical));
    assert(!lume_mmu_map_user(0, LUME_USER_LIMIT, frame, true, false));
    assert(!lume_mmu_map_user(0, address, frame, true, true));
    assert(lume_mmu_map_user(0, LUME_LARGE_ELF_BASE, frame, false, true));
    assert(lume_mmu_swap_user(0, 2));
    assert(lume_mmu_translate(lume_mmu_task_root(2), LUME_LARGE_ELF_BASE, false, true, true,
                              &physical));
    lume_mmu_release_user(1);
    lume_mmu_release_user(0);
    lume_mmu_release_user(2);
    assert(!pages && invalidations);
    puts("PASS_I686_MMU dual views, nested JIT scopes, sharing, protection, unmap and release");
    return 0;
}
