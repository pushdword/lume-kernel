/* SPDX-License-Identifier: GPL-2.0-only */
#include "lume/module.h"
#include "lume/module_elf.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static _Alignas(4096) unsigned char pool[LUME_MODULE_FILE_LIMIT];
static char exports[256][96];
static unsigned export_count, allocated, resolved;

int lume_module_relocate_amd64(unsigned, void *, size_t, uint64_t, uint64_t, int64_t);

int lume_module_memory_allocate(size_t bytes, struct lume_module_memory *memory)
{
    assert(!allocated && bytes <= sizeof(pool) && !(bytes & 4095));
    memset(pool, 0, bytes);
    *memory = (struct lume_module_memory){pool, bytes};
    allocated = 1;
    return 0;
}

int lume_module_memory_release(struct lume_module_memory *memory)
{
    assert(allocated && memory->address == pool);
    allocated = 0;
    *memory = (struct lume_module_memory){0};
    return 0;
}

int lume_module_memory_protect(const struct lume_module_memory *memory, size_t offset, size_t bytes,
                               unsigned flags)
{
    assert(allocated && offset <= memory->bytes && bytes <= memory->bytes - offset);
    assert((flags & (LUME_MODULE_WRITE | LUME_MODULE_EXECUTE)) !=
           (LUME_MODULE_WRITE | LUME_MODULE_EXECUTE));
    return 0;
}

static int resolve(const char *name, uint64_t *address)
{
    for (unsigned i = 0; i < export_count; ++i)
        if (!strcmp(name, exports[i]))
        {
            *address = (uintptr_t)pool + 1024 + i * 16;
            ++resolved;
            return 0;
        }
    fprintf(stderr, "Missing kernel export: %s\n", name);
    return -1;
}

static void synchronize(uintptr_t address, size_t bytes)
{
    assert(address == (uintptr_t)pool && bytes);
}

int main(int argc, char **argv)
{
    assert(argc == 5);
    FILE *names = fopen(argv[2], "rb");
    assert(names);
    while (export_count < 256 && fscanf(names, "%95s", exports[export_count]) == 1)
        ++export_count;
    fclose(names);
    FILE *file = fopen(argv[1], "rb");
    assert(file && !fseek(file, 0, SEEK_END));
    long length = ftell(file);
    assert(length > 0 && (unsigned long)length <= LUME_MODULE_FILE_LIMIT);
    rewind(file);
    unsigned char *data = malloc((size_t)length);
    assert(data && fread(data, 1, (size_t)length, file) == (size_t)length);
    fclose(file);
    unsigned machine = (unsigned)strtoul(argv[3], NULL, 10);
    const struct lume_module_linker linker = {
        .machine = machine,
        .resolve = resolve,
        .relocate = machine == 183 ? lume_module_relocate_aarch64 : lume_module_relocate_amd64,
        .synchronize = synchronize,
    };
    struct lume_module_elf image;
    int result = lume_module_elf_load(data, (size_t)length, &linker, &image);
    assert(result == 0 && allocated && resolved);
    struct lume_module_descriptor *descriptor = (void *)image.descriptor;
    assert(
        lume_module_elf_contains(&image, image.descriptor, sizeof(*descriptor), LUME_MODULE_READ));
    assert(descriptor->magic == LUME_MODULE_MAGIC && descriptor->size == sizeof(*descriptor));
    assert(descriptor->machine == machine && descriptor->ddk_version == LUME_DDK_VERSION);
    assert(!strcmp(descriptor->name, argv[4]) && descriptor->driver_count == 1);
    const struct lume_driver *driver = descriptor->drivers;
    assert(lume_module_elf_contains(&image, (uintptr_t)driver, sizeof(*driver), LUME_MODULE_READ));
    assert(driver->match_count && driver->context_bytes && !strcmp(driver->name, argv[4]));
    const struct lume_hardware_ops *ops = driver->ops;
    assert(lume_module_elf_contains(&image, (uintptr_t)ops, sizeof(*ops), LUME_MODULE_READ));
    assert(lume_module_elf_contains(&image, (uintptr_t)ops->probe, 1, LUME_MODULE_EXECUTE));
    assert(lume_module_elf_contains(&image, (uintptr_t)ops->start, 1, LUME_MODULE_EXECUTE));
    assert(lume_module_elf_contains(&image, (uintptr_t)ops->stop, 1, LUME_MODULE_EXECUTE));
    assert(lume_module_elf_release(&image) == 0 && !allocated);
    free(data);
    printf("Linux module linker PASS: machine=%u exports=%u name=%s\n", machine, resolved, argv[4]);
    return 0;
}
