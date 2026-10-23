#ifndef LUME_ELF_H
#define LUME_ELF_H
#include "early.h"
#define LUME_ELF_BASE (LUME_USER_BASE + 0x40000)
#define LUME_ELF_LIMIT LUME_HEAP_BASE
#define LUME_ELF_PAGES ((LUME_ELF_LIMIT - LUME_ELF_BASE) / 4096u)
#define LUME_ELF_SEGMENTS 8u

struct lume_elf_segment
{
    uint64_t offset, address, file_size, memory_size;
    unsigned flags;
};

struct lume_elf_plan
{
    uint64_t entry;
    uint64_t tls_address, tls_file, tls_size, tls_align;
    unsigned count, pages;
    struct lume_elf_segment segments[LUME_ELF_SEGMENTS];
};

struct lume_elf_image
{
    uint64_t entry;
    uint64_t tls_address, tls_file, tls_size, tls_align;
    unsigned count, task, metadata_pages;

    struct
    {
        uint64_t address, physical;
    } *pages;
};

bool lume_elf_plan(const void *, size_t, struct lume_elf_plan *);
bool lume_elf_load(unsigned task, const void *, size_t, struct lume_elf_image *);
void lume_elf_release(struct lume_elf_image *);
bool lume_mmu_map_user(unsigned task, uint64_t va, uint64_t pa, bool write, bool execute);
void lume_mmu_unmap_user(unsigned task, uint64_t va);
void lume_sync_code(uint64_t address, size_t bytes);
#endif
