#include "lume/modules.h"
#include "lume/module.h"
#include "lume/abi.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static const struct lume_driver *registered_driver;
static unsigned releases;
static int unregister_error, release_error, hardware_error;
static bool late_probe, late_pin;
static const uintptr_t code_address = UINT32_C(0x10000000);

static int resolve(const char *name, uint64_t *address)
{
    (void)name;
    (void)address;
    return -LUME_ENOENT;
}

static int relocate(unsigned type, void *target, size_t available, uint64_t place, uint64_t symbol,
                    int64_t addend)
{
    (void)type;
    (void)target;
    (void)available;
    (void)place;
    (void)symbol;
    (void)addend;
    return 0;
}

static void synchronize(uintptr_t address, size_t bytes)
{
    (void)address;
    (void)bytes;
}

int lume_module_elf_load(const void *file, size_t bytes, const struct lume_module_linker *linker,
                         struct lume_module_elf *image)
{
    assert(file && bytes == sizeof(struct lume_module_descriptor) && linker);
    image->descriptor = (uintptr_t)file;
    image->memory.address = (void *)code_address;
    image->memory.bytes = 4096;
    return 0;
}

int lume_module_elf_release(struct lume_module_elf *image)
{
    if (release_error)
        return release_error;
    assert(image->memory.address == (void *)code_address);
    ++releases;
    memset(image, 0, sizeof(*image));
    return 0;
}

bool lume_module_elf_contains(const struct lume_module_elf *image, uintptr_t address, size_t bytes,
                              unsigned flags)
{
    if (!image->memory.address || !address || !bytes)
        return false;
    return !(flags & LUME_MODULE_EXECUTE) || address == code_address;
}

uint64_t lume_module_memory_available(void)
{
    return 4096;
}

int lume_driver_register(const struct lume_driver *driver)
{
    assert(!registered_driver);
    registered_driver = driver;
    return 0;
}

int lume_driver_unregister(const struct lume_driver *driver)
{
    assert(driver == registered_driver);
    if (unregister_error)
        return unregister_error;
    registered_driver = NULL;
    return 0;
}

int lume_hardware_add(const struct lume_hardware_description *description, uint32_t *id)
{
    assert(description && id);
    *id = 17;
    return 0;
}

int lume_hardware_remove(uint32_t id)
{
    assert(id == 17);
    return 0;
}

int lume_hardware_get(uint32_t id, struct lume_hardware_info *info)
{
    assert(id == 17 && info);
    *info = (struct lume_hardware_info){0};
    if (late_probe)
    {
        void *owner = NULL;
        if (!lume_module_pin(code_address, &owner))
        {
            late_pin = true;
            lume_module_unpin(owner);
        }
    }
    return hardware_error;
}

#include "../../src/module/manager.c"

int main(void)
{
    const struct lume_hardware_identity identity = {.kind = 1, .value = "test"};
    const struct lume_hardware_ops operations = {.start = (void *)UINT32_C(0x10000000),
                                                 .stop = (void *)UINT32_C(0x10000000)};
    const struct lume_driver driver = {.version = LUME_DRIVER_VERSION,
                                       .name = "test_driver",
                                       .matches = &identity,
                                       .match_count = 1,
                                       .ops = &operations};
    const struct lume_module_virtual device = {"test_device", &identity, 1};
    struct lume_module_descriptor descriptor = {.magic = LUME_MODULE_MAGIC,
                                                .size = sizeof(descriptor),
                                                .version = LUME_MODULE_VERSION,
                                                .ddk_version = LUME_DDK_VERSION,
                                                .machine = 183,
                                                .name = "test_module",
                                                .drivers = &driver,
                                                .driver_count = 1,
                                                .devices = &device,
                                                .device_count = 1};
    const struct lume_module_linker loader = {183, resolve, relocate, synchronize};
    unsigned char build[20] = {0};
    assert(!lume_modules_start(&loader, build));
    struct lume_module_info info;
    assert(!lume_module_load(&descriptor, sizeof(descriptor), &info));
    uint32_t id = info.id;
    void *owner = NULL;
    assert(!lume_module_pin(code_address, &owner));
    assert(lume_module_unload(id) == -LUME_EBUSY);
    assert(!lume_module_info(0, &info) && info.references == 1);
    assert(!registered_driver->owner_get(registered_driver->owner));
    assert(!lume_module_info(0, &info) && info.references == 2);
    registered_driver->owner_put(registered_driver->owner);
    lume_module_unpin(owner);
    hardware_error = -LUME_EBUSY;
    assert(lume_module_unload(id) == -LUME_EBUSY);
    assert(!lume_module_pin(code_address, &owner));
    lume_module_unpin(owner);
    hardware_error = 0;
    unregister_error = -LUME_EBUSY;
    assert(lume_module_unload(id) == -LUME_EBUSY);
    assert(!lume_module_pin(code_address, &owner));
    lume_module_unpin(owner);
    unregister_error = 0;
    release_error = -LUME_EIO;
    assert(lume_module_unload(id) == -LUME_EIO);
    assert(!lume_module_pin(code_address, &owner));
    lume_module_unpin(owner);
    release_error = 0;
    assert(!lume_module_unload(id) && releases == 1);
    assert(lume_module_pin(code_address, &owner) == -LUME_ENOENT);
    assert(!lume_module_load(&descriptor, sizeof(descriptor), &info));
    id = info.id;
    late_probe = true;
    assert(!lume_module_unload(id));
    if (late_pin)
    {
        puts("MODULE_LIFETIME_FAIL pin admitted during retirement");
        return 1;
    }
    assert(!lume_module_load(&descriptor, sizeof(descriptor), &info));
    assert(!lume_module_unload(info.id) && releases == 3);
    assert(!lume_module_inspect(&descriptor, sizeof(descriptor), &info) && releases == 4);
    puts("MODULE_LIFETIME_PASS references, error recovery, retirement, reuse and inspect");
    return 0;
}
