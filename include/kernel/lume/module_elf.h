#ifndef LUME_MODULE_ELF_H
#define LUME_MODULE_ELF_H
#include "module_memory.h"
#include <stdbool.h>
#include <stdint.h>

#define LUME_MODULE_SECTION_LIMIT 256u
#define LUME_MODULE_FILE_LIMIT (2u * 1024u * 1024u)

struct lume_module_section
{
    uintptr_t address;
    size_t bytes;
    unsigned flags;
};

struct lume_module_elf
{
    struct lume_module_memory memory;
    struct lume_module_section sections[LUME_MODULE_SECTION_LIMIT];
    unsigned section_count;
    uintptr_t descriptor;
};

struct lume_module_linker
{
    unsigned machine;
    int (*resolve)(const char *name, uint64_t *address);
    int (*relocate)(unsigned type, void *target, size_t available, uint64_t place, uint64_t symbol,
                    int64_t addend);
    void (*synchronize)(uintptr_t address, size_t bytes);
};

int lume_module_elf_load(const void *file, size_t bytes, const struct lume_module_linker *,
                         struct lume_module_elf *);
int lume_module_elf_release(struct lume_module_elf *);
bool lume_module_elf_contains(const struct lume_module_elf *, uintptr_t address, size_t bytes,
                              unsigned flags);
int lume_module_relocate_aarch64(unsigned type, void *target, size_t available, uint64_t place,
                                 uint64_t symbol, int64_t addend);
#endif
