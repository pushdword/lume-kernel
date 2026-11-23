#include "../../src/module/module_boot.c"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static const char configuration[] = "load /lib/modules/test.lkm\r\n"
                                    "probe critical retain\nprobe regular\n"
                                    "invalid command\nbind /manual regular\n";
static const char module_file[] = "module";
static size_t positions[3];
static unsigned allocations, opens, loads, errors, stopped[8], stop_count;
static int stop_error;
static const struct lume_driver critical = {.name = "critical"};
static const struct lume_driver regular = {.name = "regular"};
static const struct lume_driver rtl8168h = {.name = "rtl8168h"};
static struct lume_hardware_info devices[5];

void *lume_kernel_allocate(size_t bytes)
{
    ++allocations;
    return malloc(bytes);
}

void lume_kernel_free(void *p)
{
    if (p)
    {
        assert(allocations);
        --allocations;
        free(p);
    }
}

bool lume_files_init(struct lume_files *files)
{
    memset(files, 0, sizeof(*files));
    return true;
}

void lume_files_release(struct lume_files *files)
{
    (void)files;
    assert(!opens);
}

int64_t lume_vfs_open(struct lume_files *files, const char *path, uint64_t flags)
{
    (void)files;
    assert(flags == LUME_O_RDONLY);
    unsigned fd = 0;
    if (!strcmp(path, "/etc/modules"))
        fd = 1;
    else if (!strcmp(path, "/lib/modules/test.lkm") || !strcmp(path, "/lib/modules/rtl8168h.lkm"))
        fd = 2;
    if (!fd)
        return -LUME_ENOENT;
    positions[fd] = 0;
    ++opens;
    return fd;
}

int64_t lume_vfs_stat(struct lume_files *files, uint64_t fd, struct lume_stat *out)
{
    (void)files;
    assert(fd == 1 || fd == 2);
    *out = (struct lume_stat){.kind = LUME_NODE_FILE,
                              .size = fd == 1 ? sizeof(configuration) - 1 : sizeof(module_file)};
    return 0;
}

int64_t lume_vfs_read(struct lume_files *files, uint64_t fd, void *out, uint64_t bytes)
{
    (void)files;
    const char *data = fd == 1 ? configuration : module_file;
    size_t size = fd == 1 ? sizeof(configuration) - 1 : sizeof(module_file);
    if (bytes > size - positions[fd])
        bytes = size - positions[fd];
    memcpy(out, data + positions[fd], (size_t)bytes);
    positions[fd] += (size_t)bytes;
    return (int64_t)bytes;
}

int64_t lume_vfs_close(struct lume_files *files, uint64_t fd)
{
    (void)files;
    assert((fd == 1 || fd == 2) && opens);
    --opens;
    return 0;
}

int lume_module_load(const void *file, size_t bytes, struct lume_module_info *info)
{
    assert(bytes == sizeof(module_file) && !memcmp(file, module_file, bytes));
    ++loads;
    *info = (struct lume_module_info){.id = loads};
    return 0;
}

const struct lume_driver *lume_driver_find(const char *name)
{
    if (!strcmp(name, rtl8168h.name))
        return loads > 1 ? &rtl8168h : NULL;
    return !strcmp(name, "critical") ? &critical : !strcmp(name, "regular") ? &regular : NULL;
}

unsigned lume_hardware_count(void)
{
    return 5;
}

int lume_hardware_info(unsigned index, struct lume_hardware_info *out)
{
    if (index >= 5)
        return -LUME_ENOENT;
    *out = devices[index];
    return 0;
}

bool lume_driver_matches(const struct lume_driver *driver, uint32_t id)
{
    return driver == &critical ? id == 1 : id >= 2 && id <= 4;
}

int lume_hardware_bind(uint32_t id, const struct lume_driver *driver)
{
    assert(id >= 1 && id <= 5 && devices[id - 1].state != LUME_HARDWARE_DISABLED);
    if (id == 4)
        return -LUME_EAGAIN;
    memcpy(devices[id - 1].driver, driver->name, strlen(driver->name) + 1);
    devices[id - 1].state = LUME_HARDWARE_READY;
    return 0;
}

int lume_hardware_unbind(uint32_t id)
{
    assert(id != 1 && id >= 2 && id <= 5);
    if (stop_error)
        return stop_error;
    stopped[stop_count++] = id;
    devices[id - 1].driver[0] = 0;
    devices[id - 1].state = LUME_HARDWARE_DISCOVERED;
    return 0;
}

void lume_value(const char *name, uint64_t value)
{
    if (!strcmp(name, "MODULE_BOOT_ERROR"))
    {
        assert((int64_t)value == -LUME_EINVAL);
        ++errors;
    }
}

int main(void)
{
    for (unsigned i = 0; i < 5; ++i)
    {
        devices[i].id = i + 1;
        devices[i].state = LUME_HARDWARE_DISCOVERED;
    }
    devices[2].state = LUME_HARDWARE_DISABLED;
    memcpy(devices[4].path, "/manual", 8);
    lume_platform_modules_boot();
    assert(loads == 1 && errors == 1 && !opens && !allocations);
    assert(!strcmp(devices[0].driver, "critical") && !strcmp(devices[1].driver, "regular"));
    assert(!devices[2].driver[0] && !devices[3].driver[0]);
    assert(!strcmp(devices[4].driver, "regular"));
    assert(retained_count == 1 && retained[0] == 1);
    assert(!lume_platform_modules_stop() && stop_count == 2);
    assert(stopped[0] == 5 && stopped[1] == 2);
    assert(devices[0].state == LUME_HARDWARE_READY && !strcmp(devices[0].driver, "critical"));
    assert(!lume_hardware_bind(2, &regular));
    stop_error = -LUME_EBUSY;
    assert(lume_platform_modules_stop() == -LUME_EBUSY && devices[1].driver[0]);
    stop_error = 0;
    assert(!lume_platform_modules_stop());
    struct lume_files files;
    char invalid[] = "probe regular unexpected";
    assert(entry(&files, invalid) == -LUME_EINVAL);
    char relative[] = "load relative.lkm";
    assert(entry(&files, relative) == -LUME_EINVAL);
    char absent[] = "autoload-pci pci,10ec:8168 /lib/modules/rtl8168h.lkm rtl8168h";
    assert(!entry(&files, absent) && loads == 1);
    devices[1].origin = LUME_HARDWARE_PCI;
    devices[1].identities[0] =
        (struct lume_hardware_identity){LUME_HARDWARE_ID_PCI, 0, "pci,10ec:8168"};
    devices[1].identity_count = 1;
    char present[] = "autoload-pci pci,10ec:8168 /lib/modules/rtl8168h.lkm rtl8168h";
    assert(!entry(&files, present) && loads == 2 && !strcmp(devices[1].driver, rtl8168h.name));
    assert(!opens && !allocations);
    puts("MODULE_BOOT_PASS Device probing, PCI autoload, retained services and reverse shutdown");
    return 0;
}
