#include "lume/elf.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint64_t lume_pages_alloc_span(unsigned count)
{
    (void)count;
    abort();
}

uint64_t lume_page_alloc(uint64_t minimum)
{
    (void)minimum;
    abort();
}

bool lume_page_free(uint64_t page)
{
    (void)page;
    abort();
}

bool lume_mmu_map_user(unsigned task, uint64_t va, uint64_t pa, bool write, bool execute)
{
    (void)task;
    (void)va;
    (void)pa;
    (void)write;
    (void)execute;
    abort();
}

void lume_mmu_unmap_user(unsigned task, uint64_t address)
{
    (void)task;
    (void)address;
    abort();
}

uint64_t lume_mmu_task_root(unsigned task)
{
    (void)task;
    abort();
}

bool lume_mmu_translate(uint64_t root, uint64_t va, bool write, bool user, bool execute,
                        uint64_t *physical)
{
    (void)root;
    (void)va;
    (void)write;
    (void)user;
    (void)execute;
    (void)physical;
    abort();
}

void lume_sync_code(uint64_t address, size_t bytes)
{
    (void)address;
    (void)bytes;
    abort();
}

_Noreturn void lume_fail(const char *why)
{
    (void)why;
    abort();
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    FILE *file = fopen(argv[1], "rb");
    assert(file && !fseek(file, 0, SEEK_END));
    long length = ftell(file);
    assert(length > 52 && !fseek(file, 0, SEEK_SET));
    unsigned char *data = malloc((size_t)length);
    assert(data && fread(data, 1, (size_t)length, file) == (size_t)length);
    fclose(file);
    struct lume_elf_plan plan;
    assert(lume_elf_plan(data, (size_t)length, &plan));
    assert(plan.entry >= LUME_ELF_BASE && plan.count && plan.pages);
    for (size_t i = 0; i < 52; ++i)
        assert(!lume_elf_plan(data, i, &plan));
    const unsigned offsets[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 16, 18, 20, 36, 40, 42};
    for (unsigned i = 0; i < sizeof(offsets) / sizeof(offsets[0]); ++i)
    {
        data[offsets[i]] ^= 128;
        assert(!lume_elf_plan(data, (size_t)length, &plan));
        data[offsets[i]] ^= 128;
    }
    unsigned char original[4];
    memcpy(original, data + 24, 4);
    memset(data + 24, 0, 4);
    assert(!lume_elf_plan(data, (size_t)length, &plan));
    memcpy(data + 24, original, 4);
    memcpy(original, data + 28, 4);
    memset(data + 28, 255, 4);
    assert(!lume_elf_plan(data, (size_t)length, &plan));
    memcpy(data + 28, original, 4);
    uint32_t phoff = (uint32_t)data[28] | (uint32_t)data[29] << 8 | (uint32_t)data[30] << 16 |
                     (uint32_t)data[31] << 24;
    unsigned char flags = data[phoff + 24];
    data[phoff + 24] = flags | 3;
    assert(!lume_elf_plan(data, (size_t)length, &plan));
    data[phoff + 24] = flags;
    assert(lume_elf_plan(data, (size_t)length, &plan));
    free(data);
    puts("PASS i686 ELF32 shell, invalid headers, truncated tables, entry and W^X checks");
    return 0;
}
