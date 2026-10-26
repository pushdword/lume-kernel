#include "lume/module_elf.h"
#include "lume/abi.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static _Alignas(4096) unsigned char pool[LUME_MODULE_FILE_LIMIT];
static unsigned allocations, protections, synchronizations;
static bool missing_export, fail_protection;

int lume_module_memory_allocate(size_t bytes, struct lume_module_memory *memory)
{
    assert(!allocations && bytes && bytes <= sizeof(pool) && !(bytes & 4095));
    memset(pool, 0, bytes);
    *memory = (struct lume_module_memory){pool, bytes};
    allocations++;
    return 0;
}

int lume_module_memory_release(struct lume_module_memory *memory)
{
    assert(allocations == 1 && memory->address == pool);
    allocations--;
    memset(memory, 0, sizeof(*memory));
    return 0;
}

int lume_module_memory_protect(const struct lume_module_memory *memory, size_t offset, size_t bytes,
                               unsigned flags)
{
    assert(allocations == 1 && offset + bytes <= memory->bytes);
    assert((flags & 6) != 6);
    protections |= 1u << flags;
    return fail_protection ? -LUME_EIO : 0;
}

static int resolve(const char *name, uint64_t *address)
{
    if (strcmp(name, "lume_fixture_export") || missing_export)
        return -LUME_ENOENT;
    *address = (uintptr_t)pool + 1024;
    return 0;
}

static void synchronize(uintptr_t address, size_t bytes)
{
    assert(address == (uintptr_t)pool && bytes);
    synchronizations++;
}

static uint64_t read_le(const unsigned char *p, unsigned bytes)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < bytes; ++i)
        value |= (uint64_t)p[i] << (i * 8);
    return value;
}

static void write_le(unsigned char *p, uint64_t value, unsigned bytes)
{
    for (unsigned i = 0; i < bytes; ++i)
        p[i] = (unsigned char)(value >> (i * 8));
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    FILE *input = fopen(argv[1], "rb");
    assert(input && !fseek(input, 0, SEEK_END));
    long length = ftell(input);
    assert(length > 64 && !fseek(input, 0, SEEK_SET));
    unsigned char *file = malloc((size_t)length), *mutant = malloc((size_t)length);
    assert(file && mutant && fread(file, 1, (size_t)length, input) == (size_t)length);
    fclose(input);
    const struct lume_module_linker linker = {183, resolve, lume_module_relocate_aarch64,
                                              synchronize};
    struct lume_module_elf image;
    for (unsigned round = 0; round < 8; ++round)
    {
        assert(!lume_module_elf_load(file, (size_t)length, &linker, &image));
        assert(allocations == 1 && synchronizations == round + 1);
        assert(protections == ((1u << 1) | (1u << 3) | (1u << 5)));
        assert(lume_module_elf_contains(&image, image.descriptor, 32, LUME_MODULE_READ));
        const unsigned char *descriptor = (void *)image.descriptor;
        uintptr_t run = (uintptr_t)read_le(descriptor, 8);
        uintptr_t counter = (uintptr_t)read_le(descriptor + 8, 8);
        uintptr_t storage = (uintptr_t)read_le(descriptor + 16, 8);
        assert(lume_module_elf_contains(&image, run, 4, LUME_MODULE_EXECUTE));
        assert(lume_module_elf_contains(&image, counter, 4, LUME_MODULE_WRITE));
        assert(lume_module_elf_contains(&image, storage, 512, LUME_MODULE_WRITE));
        assert(*(unsigned *)counter == 7 && !memcmp((void *)storage, (unsigned char[512]){0}, 512));
        assert(read_le(descriptor + 24, 8) == UINT64_C(0x123456789abcdef0));
        assert(!lume_module_elf_release(&image) && !allocations);
    }
    for (size_t truncated = 0; truncated < (size_t)length; ++truncated)
    {
        assert(lume_module_elf_load(file, truncated, &linker, &image) < 0);
        assert(!allocations);
    }
    missing_export = true;
    assert(lume_module_elf_load(file, (size_t)length, &linker, &image) == -LUME_ENOENT);
    assert(!allocations);
    missing_export = false;
    fail_protection = true;
    assert(lume_module_elf_load(file, (size_t)length, &linker, &image) == -LUME_EIO);
    assert(!allocations);
    fail_protection = false;
    const unsigned header_offsets[] = {0, 4, 5, 16, 18, 20, 24, 32, 40, 48, 52, 56, 58, 60, 62};
    for (unsigned i = 0; i < sizeof(header_offsets) / sizeof(*header_offsets); ++i)
    {
        memcpy(mutant, file, (size_t)length);
        mutant[header_offsets[i]] ^= 255;
        assert(lume_module_elf_load(mutant, (size_t)length, &linker, &image) < 0 && !allocations);
    }
    size_t table = (size_t)read_le(file + 40, 8);
    unsigned count = (unsigned)read_le(file + 60, 2), tested = 0;
    for (unsigned i = 1; i < count; ++i)
    {
        size_t entry = table + i * 64;
        unsigned type = (unsigned)read_le(file + entry + 4, 4);
        uint64_t flags = read_le(file + entry + 8, 8);
        memcpy(mutant, file, (size_t)length);
        if (flags & 2)
        {
            write_le(mutant + entry + 8, flags | 5, 8);
            tested++;
        }
        else if (type == 4 && read_le(file + entry + 32, 8))
        {
            size_t relocation = (size_t)read_le(file + entry + 24, 8);
            write_le(mutant + relocation + 8, UINT64_C(0xffffffff), 4);
            tested++;
        }
        else
            continue;
        assert(lume_module_elf_load(mutant, (size_t)length, &linker, &image) < 0 && !allocations);
    }
    assert(tested >= 4);
    unsigned char branch[8] = {0, 0, 0, 0x94};
    assert(!lume_module_relocate_aarch64(283, branch, 4, 0x10000000, 0x17fffffc, 0));
    branch[3] = 0x94;
    assert(lume_module_relocate_aarch64(283, branch, 4, 0x10000000, 0x18000000, 0) < 0);
    assert(lume_module_relocate_aarch64(257, branch, 8, 0, 0, -1) < 0);
    assert(lume_module_relocate_aarch64(257, branch, 7, 0, 42, 0) < 0);
    free(file);
    free(mutant);
    puts("PASS_MODULE_ELF cycles=8 truncations=all malformed=header,permissions,relocations");
    return 0;
}
