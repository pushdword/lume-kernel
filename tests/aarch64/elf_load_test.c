#include "lume/elf.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FRAMES 2048u
static _Alignas(4096) unsigned char memory[FRAMES][4096];
static bool used[FRAMES];
static uint64_t virtuals[FRAMES], physicals[FRAMES];
static unsigned allocated, mappings, syncs, tasks[FRAMES];
static bool writable[FRAMES], executable[FRAMES];
static int allocation_budget = -1, map_budget = -1;

uint64_t lume_pages_alloc_span(unsigned count)
{
    if (!count || !allocation_budget)
        return 0;
    for (unsigned first = 0; first + count <= FRAMES; ++first)
    {
        unsigned i = 0;
        while (i < count && !used[first + i])
            ++i;
        if (i != count)
            continue;
        for (i = 0; i < count; ++i)
            used[first + i] = true;
        allocated += count;
        if (allocation_budget > 0)
            --allocation_budget;
        memset(memory[first], 0, count * 4096u);
        return (uintptr_t)memory[first];
    }
    return 0;
}

uint64_t lume_page_alloc(uint64_t minimum)
{
    assert(!minimum);
    return lume_pages_alloc_span(1);
}

bool lume_page_free(uint64_t page)
{
    for (unsigned i = 0; i < FRAMES; ++i)
        if (page == (uintptr_t)memory[i] && used[i])
        {
            used[i] = false;
            --allocated;
            return true;
        }
    return false;
}

bool lume_mmu_map_user(unsigned task, uint64_t va, uint64_t pa, bool write, bool execute)
{
    assert((task == 3 || task == 4) && !(write && execute));
    if (!map_budget)
        return false;
    if (map_budget > 0)
        --map_budget;
    for (unsigned i = 0; i < mappings; ++i)
        assert(tasks[i] != task || virtuals[i] != va);
    assert(mappings < FRAMES);
    virtuals[mappings] = va;
    tasks[mappings] = task;
    writable[mappings] = write;
    executable[mappings] = execute;
    physicals[mappings++] = pa;
    return true;
}

void lume_mmu_unmap_user(unsigned task, uint64_t va)
{
    assert(task == 3 || task == 4);
    for (unsigned i = 0; i < mappings; ++i)
        if (tasks[i] == task && virtuals[i] == va)
        {
            --mappings;
            virtuals[i] = virtuals[mappings];
            physicals[i] = physicals[mappings];
            tasks[i] = tasks[mappings];
            writable[i] = writable[mappings];
            executable[i] = executable[mappings];
            return;
        }
    abort();
}

uint64_t lume_mmu_task_root(unsigned task)
{
    assert(task == 3 || task == 4);
    return task + 1;
}

bool lume_mmu_translate(uint64_t root, uint64_t va, bool write, bool user, bool execute,
                        uint64_t *physical)
{
    assert(user && (root == 4 || root == 5));
    for (unsigned i = 0; i < mappings; ++i)
        if (tasks[i] + 1 == root && virtuals[i] == va)
        {
            if ((write && !writable[i]) || (execute && !executable[i]))
                return false;
            *physical = physicals[i];
            return true;
        }
    return false;
}

void lume_sync_code(uint64_t address, size_t bytes)
{
    assert(address && bytes == 4096);
    ++syncs;
}

_Noreturn void lume_fail(const char *message)
{
    fputs(message, stderr);
    abort();
}

static void put(unsigned char *p, unsigned count, uint64_t value)
{
    for (unsigned i = 0; i < count; ++i)
        p[i] = (unsigned char)(value >> (i * 8));
}

int main(void)
{
    unsigned char elf[8192] = {0x7f, 'E', 'L', 'F', 2, 1, 1};
    put(elf + 16, 2, 2);
    put(elf + 18, 2, 183);
    put(elf + 20, 4, 1);
    put(elf + 24, 8, LUME_ELF_BASE);
    put(elf + 32, 8, 64);
    put(elf + 52, 2, 64);
    put(elf + 54, 2, 56);
    put(elf + 56, 2, 2);
    for (unsigned i = 0; i < 2; ++i)
    {
        unsigned char *h = elf + 64 + i * 56;
        put(h, 4, 1);
        put(h + 4, 4, i ? 6 : 5);
        put(h + 8, 8, i ? 4096 : 0);
        put(h + 16, 8, LUME_ELF_BASE + (uint64_t)i * 0x200000);
        put(h + 32, 8, 4096);
        put(h + 40, 8, i ? 5 * 1024 * 1024 : 4096);
        put(h + 48, 8, 4096);
    }
    memset(elf + 4096, 0x5a, 4096);
    struct lume_elf_image image;
    assert(lume_elf_load(3, elf, sizeof(elf), &image));
    assert(image.count == 1281 && image.metadata_pages == 6 && mappings == 1281 && syncs == 1);
    for (unsigned i = 0; i < mappings; ++i)
    {
        unsigned char *p = (void *)(uintptr_t)physicals[i];
        unsigned char expected = virtuals[i] == LUME_ELF_BASE + 0x200000 ? 0x5a : 0;
        if (virtuals[i] != LUME_ELF_BASE)
            for (unsigned j = 0; j < 4096; ++j)
                assert(p[j] == expected);
    }
    lume_elf_release(&image);
    assert(!allocated && !mappings && !image.pages && !image.count);
    for (int budget = 0; budget < 8; ++budget)
    {
        allocation_budget = budget;
        assert(!lume_elf_load(3, elf, sizeof(elf), &image));
        assert(!allocated && !mappings && !image.pages);
    }
    allocation_budget = -1;
    for (int budget = 0; budget < 8; ++budget)
    {
        map_budget = budget;
        assert(!lume_elf_load(3, elf, sizeof(elf), &image));
        assert(!allocated && !mappings && !image.pages);
    }
    map_budget = -1;
    put(elf + 64 + 56 + 40, 8, 4096);
    assert(lume_elf_load(3, elf, sizeof(elf), &image));
    assert(image.count == 2 && allocated == 3 && mappings == 2);
    struct lume_elf_image child;
    assert(lume_elf_clone(4, &image, &child));
    assert(child.count == 2 && child.task == 4 && child.entry == image.entry);
    for (unsigned i = 0; i < child.count; ++i)
    {
        assert(child.pages[i].physical != image.pages[i].physical);
        assert(!memcmp((void *)(uintptr_t)child.pages[i].physical,
                       (void *)(uintptr_t)image.pages[i].physical, 4096));
        uint64_t physical;
        assert(lume_mmu_translate(5, child.pages[i].address, i != 0, true, i == 0, &physical));
        assert(!lume_mmu_translate(5, child.pages[i].address, i == 0, true, i != 0, &physical));
    }
    unsigned char original = *(unsigned char *)(uintptr_t)image.pages[1].physical;
    *(unsigned char *)(uintptr_t)child.pages[1].physical ^= 255;
    assert(*(unsigned char *)(uintptr_t)image.pages[1].physical == original);
    lume_elf_release(&child);
    for (int budget = 0; budget < 3; ++budget)
    {
        allocation_budget = budget;
        assert(!lume_elf_clone(4, &image, &child));
        assert(allocated == 3 && mappings == 2 && !child.pages);
    }
    allocation_budget = -1;
    for (int budget = 0; budget < 2; ++budget)
    {
        map_budget = budget;
        assert(!lume_elf_clone(4, &image, &child));
        assert(allocated == 3 && mappings == 2 && !child.pages);
    }
    map_budget = -1;
    lume_elf_release(&image);
    assert(!allocated && !mappings);
    put(elf + 64 + 56 + 40, 8, 5 * 1024 * 1024);
    struct lume_elf_plan plan;
    unsigned char *tls = elf + 64 + 2 * 56;
    put(elf + 56, 2, 3);
    put(tls, 4, 7);
    put(tls + 8, 8, 4096 + 32);
    put(tls + 16, 8, LUME_ELF_BASE + 0x200000 + 32);
    put(tls + 32, 8, 16);
    put(tls + 40, 8, 96);
    put(tls + 48, 8, 16);
    assert(lume_elf_plan(elf, sizeof(elf), &plan) && plan.tls_file == 16 && plan.tls_size == 96 &&
           plan.tls_align == 16);
    put(tls + 48, 8, 128);
    assert(!lume_elf_plan(elf, sizeof(elf), &plan));
    put(tls + 48, 8, 3);
    assert(!lume_elf_plan(elf, sizeof(elf), &plan));
    put(tls + 48, 8, 16);
    put(tls + 32, 8, 97);
    assert(!lume_elf_plan(elf, sizeof(elf), &plan));
    put(tls + 32, 8, 16);
    put(tls + 40, 8, 65537);
    assert(!lume_elf_plan(elf, sizeof(elf), &plan));
    put(tls + 40, 8, 96);
    put(tls + 8, 8, 4096 + 64);
    assert(!lume_elf_plan(elf, sizeof(elf), &plan));
    put(tls + 8, 8, 4096 + 32);
    put(tls + 16, 8, LUME_ELF_BASE + 0x100000);
    assert(!lume_elf_plan(elf, sizeof(elf), &plan));
    put(tls + 16, 8, LUME_ELF_BASE + 0x200000 + 32);
    put(elf + 56, 2, 4);
    memcpy(tls + 56, tls, 56);
    assert(!lume_elf_plan(elf, sizeof(elf), &plan));
    puts(
        "PASS_ELF_LOAD large image/BSS/fork isolation/rollback/TLS alignment, containment and duplicates");
}
