#include "lume/modules.h"
#include "lume/module.h"
#include "lume/hardware.h"
#include "lume/abi.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

static bool slots[32];
static unsigned allocations, registrations;
static size_t live_bytes;
static const struct lume_driver *drivers[16];

int lume_module_relocate_amd64(unsigned, void *, size_t, uint64_t, uint64_t, int64_t);

int lume_module_memory_allocate(size_t bytes, struct lume_module_memory *out)
{
    for (unsigned i = 0; i < 32; ++i)
        if (!slots[i])
        {
            void *address = VirtualAlloc((void *)(uintptr_t)(0x10000000u + i * 0x400000u), bytes,
                                         MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            if (!address)
                return -LUME_ENOMEM;
            slots[i] = true;
            ++allocations;
            live_bytes += bytes;
            *out = (struct lume_module_memory){address, bytes};
            return 0;
        }
    return -LUME_ENOMEM;
}

int lume_module_memory_protect(const struct lume_module_memory *memory, size_t offset, size_t bytes,
                               unsigned flags)
{
    assert(!((flags & LUME_MODULE_WRITE) && (flags & LUME_MODULE_EXECUTE)));
    DWORD old;
    DWORD mode = flags & LUME_MODULE_EXECUTE ? PAGE_EXECUTE_READ
                 : flags & LUME_MODULE_WRITE ? PAGE_READWRITE
                                             : PAGE_READONLY;
    return VirtualProtect((unsigned char *)memory->address + offset, bytes, mode, &old) ? 0
                                                                                        : -LUME_EIO;
}

int lume_module_memory_release(struct lume_module_memory *memory)
{
    unsigned index = (unsigned)(((uintptr_t)memory->address - 0x10000000u) / 0x400000u);
    assert(index < 32 && slots[index] && allocations && live_bytes >= memory->bytes);
    assert(VirtualFree(memory->address, 0, MEM_RELEASE));
    slots[index] = false;
    --allocations;
    live_bytes -= memory->bytes;
    memset(memory, 0, sizeof(*memory));
    return 0;
}

size_t lume_module_memory_available(void)
{
    return 2u * 1024 * 1024 - live_bytes;
}

int lume_driver_register(const struct lume_driver *driver)
{
    assert(registrations < 16);
    drivers[registrations++] = driver;
    return 0;
}

int lume_driver_unregister(const struct lume_driver *driver)
{
    for (unsigned i = 0; i < registrations; ++i)
        if (drivers[i] == driver)
        {
            memmove(drivers + i, drivers + i + 1, (--registrations - i) * sizeof(*drivers));
            return 0;
        }
    return -LUME_ENOENT;
}

int lume_hardware_add(const struct lume_hardware_description *description, uint32_t *id)
{
    (void)description;
    (void)id;
    assert(false);
    return -LUME_ENOSYS;
}

int lume_hardware_remove(uint32_t id)
{
    (void)id;
    assert(false);
    return -LUME_ENOSYS;
}

int lume_hardware_get(uint32_t id, struct lume_hardware_info *info)
{
    (void)id;
    (void)info;
    return -LUME_ENOENT;
}

static int resolve(const char *name, uint64_t *address)
{
    assert(*name);
    *address = 0x30000000;
    return 0;
}

static void synchronize(uintptr_t address, size_t bytes)
{
    (void)address;
    (void)bytes;
}

int main(int argc, char **argv)
{
    assert(argc == 7);
    const unsigned char build[20] = {0};
    const struct lume_module_linker linker = {62, resolve, lume_module_relocate_amd64, synchronize};
    assert(!lume_modules_start(&linker, build));
    uint32_t ids[6];
    for (unsigned i = 0; i < 6; ++i)
    {
        FILE *file = fopen(argv[i + 1], "rb");
        assert(file && !fseek(file, 0, SEEK_END));
        long size = ftell(file);
        assert(size > 64 && !fseek(file, 0, SEEK_SET));
        unsigned char *data = malloc((size_t)size);
        assert(data && fread(data, 1, (size_t)size, file) == (size_t)size);
        fclose(file);
        struct lume_module_info info;
        data[18] = 183;
        assert(lume_module_load(data, (size_t)size, &info) == -LUME_ENOEXEC);
        data[18] = 62;
        uint64_t magic = LUME_MODULE_MAGIC;
        size_t descriptor;
        for (descriptor = 0; descriptor + 44 < (size_t)size; ++descriptor)
            if (!memcmp(data + descriptor, &magic, 8))
                break;
        assert(descriptor + 44 < (size_t)size);
        data[descriptor + 24] = 1;
        assert(lume_module_load(data, (size_t)size, &info) == -LUME_ENOEXEC);
        data[descriptor + 24] = 0;
        assert(!lume_module_load(data, (size_t)size, &info));
        assert(info.drivers == 1 && !info.devices && info.bytes);
        ids[i] = info.id;
        assert(lume_module_load(data, (size_t)size, &info) == -LUME_EEXIST);
        free(data);
    }
    assert(registrations == 6 && allocations == 6 && live_bytes < 2u * 1024 * 1024);
    const struct lume_driver *driver = drivers[0];
    assert(!driver->owner_get(driver->owner));
    assert(lume_module_unload(ids[0]) == -LUME_EBUSY);
    driver->owner_put(driver->owner);
    for (unsigned i = 0; i < 6; ++i)
    {
        struct lume_module_info info;
        assert(!lume_module_info(i, &info) && info.id == ids[i]);
    }
    for (unsigned i = 6; i; --i)
        assert(!lume_module_unload(ids[i - 1]));
    assert(!registrations && !allocations && !live_bytes);
    puts(
        "Q35_MODULE_LOAD_PASS six real ELF modules, W^X, machine/build guards, duplicates and references");
    return 0;
}
