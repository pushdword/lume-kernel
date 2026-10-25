#include "lume/modules.h"
#include "lume/module.h"
#include "lume/abi.h"
#include <string.h>

struct module
{
    struct lume_module_elf image;
    struct lume_module_info info;
    struct lume_driver drivers[LUME_MODULE_DRIVERS];
    struct lume_hardware_ops ops[LUME_MODULE_DRIVERS];
    uint32_t devices[LUME_MODULE_DEVICES];
};

static struct module modules[LUME_MODULE_LIMIT];
static const struct lume_module_linker *linker;
static unsigned char kernel_build[LUME_MODULE_BUILD_BYTES];
static uint32_t next_id = 1;

static bool readable(const struct module *m, const void *p, size_t bytes, size_t alignment)
{
    return !((uintptr_t)p & (alignment - 1)) &&
           lume_module_elf_contains(&m->image, (uintptr_t)p, bytes, LUME_MODULE_READ);
}

static bool text(const struct module *m, const char *p, size_t capacity, bool identifier)
{
    for (size_t i = 0; i < capacity; ++i)
    {
        if (!readable(m, p + i, 1, 1))
            return false;
        unsigned char c = (unsigned char)p[i];
        if (!c)
            return i != 0;
        if (identifier && !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                            (c >= '0' && c <= '9') || c == '_' || c == '-'))
            return false;
    }
    return false;
}

static bool callback(const struct module *m, uintptr_t p, bool required)
{
    return (!p && !required) ||
           (!(p & 3) && lume_module_elf_contains(&m->image, p, 4, LUME_MODULE_EXECUTE));
}

static int owner_get(void *owner)
{
    struct module *m = owner;
    if (!m->info.id || m->info.references == UINT32_MAX)
        return -LUME_EBUSY;
    m->info.references++;
    return 0;
}

static void owner_put(void *owner)
{
    struct module *m = owner;
    if (m->info.references)
        m->info.references--;
}

int lume_module_pin(uintptr_t address, void **owner)
{
    if (!owner)
        return -LUME_EINVAL;
    for (unsigned i = 0; i < LUME_MODULE_LIMIT; ++i)
        if (modules[i].info.id &&
            lume_module_elf_contains(&modules[i].image, address, 4, LUME_MODULE_EXECUTE))
        {
            int result = owner_get(modules + i);
            if (!result)
                *owner = modules + i;
            return result;
        }
    return -LUME_ENOENT;
}

void lume_module_unpin(void *owner)
{
    if (owner)
        owner_put(owner);
}

int lume_modules_start(const struct lume_module_linker *ops, const unsigned char build[20])
{
    if (linker || !ops || !build || !ops->resolve || !ops->relocate || !ops->synchronize)
        return -LUME_EINVAL;
    linker = ops;
    memcpy(kernel_build, build, sizeof(kernel_build));
    return 0;
}

static int validate(struct module *m, bool unique)
{
    const struct lume_module_descriptor *d = (void *)m->image.descriptor;
    if (!readable(m, d, sizeof(*d), _Alignof(struct lume_module_descriptor)) ||
        d->magic != LUME_MODULE_MAGIC || d->size != sizeof(*d) ||
        d->version != LUME_MODULE_VERSION || d->ddk_version != LUME_DDK_VERSION ||
        d->machine != linker->machine || d->reserved ||
        memcmp(d->kernel_build, kernel_build, sizeof(kernel_build)) ||
        !text(m, d->name, sizeof(d->name), true) || !d->driver_count ||
        d->driver_count > LUME_MODULE_DRIVERS || d->device_count > LUME_MODULE_DEVICES ||
        !readable(m, d->drivers, d->driver_count * sizeof(*d->drivers),
                  _Alignof(struct lume_driver)) ||
        (d->device_count && !readable(m, d->devices, d->device_count * sizeof(*d->devices),
                                      _Alignof(struct lume_module_virtual))))
        return -LUME_ENOEXEC;
    for (unsigned i = 0; i < LUME_MODULE_LIMIT; ++i)
        if (unique && modules + i != m && modules[i].info.id &&
            !strcmp(modules[i].info.name, d->name))
            return -LUME_EEXIST;
    for (size_t i = 0; i < d->driver_count; ++i)
    {
        const struct lume_driver *driver = d->drivers + i;
        if (driver->version != LUME_DRIVER_VERSION || !text(m, driver->name, 64, true) ||
            !driver->match_count || driver->match_count > LUME_DRIVER_MATCH_LIMIT ||
            driver->context_bytes > 1024u * 1024u || driver->owner || driver->owner_get ||
            driver->owner_put ||
            !readable(m, driver->matches, driver->match_count * sizeof(*driver->matches),
                      _Alignof(struct lume_hardware_identity)) ||
            !readable(m, driver->ops, sizeof(*driver->ops), _Alignof(struct lume_hardware_ops)))
            return -LUME_ENOEXEC;
        for (size_t j = 0; j < driver->match_count; ++j)
            if (!text(m, driver->matches[j].value, LUME_HARDWARE_ID_BYTES, false))
                return -LUME_ENOEXEC;
        const struct lume_hardware_ops *ops = driver->ops;
        if (!callback(m, (uintptr_t)ops->probe, false) ||
            !callback(m, (uintptr_t)ops->start, true) || !callback(m, (uintptr_t)ops->stop, true) ||
            !callback(m, (uintptr_t)ops->release, false) ||
            !callback(m, (uintptr_t)ops->read_register, false))
            return -LUME_ENOEXEC;
        m->ops[i] = *ops;
        m->drivers[i] = *driver;
        m->drivers[i].ops = m->ops + i;
        m->drivers[i].owner = m;
        m->drivers[i].owner_get = owner_get;
        m->drivers[i].owner_put = owner_put;
    }
    for (size_t i = 0; i < d->device_count; ++i)
    {
        const struct lume_module_virtual *v = d->devices + i;
        if (!text(m, v->name, 64, true) || !v->identity_count ||
            v->identity_count > LUME_HARDWARE_IDS ||
            !readable(m, v->identities, v->identity_count * sizeof(*v->identities),
                      _Alignof(struct lume_hardware_identity)))
            return -LUME_ENOEXEC;
        for (size_t j = 0; j < v->identity_count; ++j)
            if (!text(m, v->identities[j].value, LUME_HARDWARE_ID_BYTES, false))
                return -LUME_ENOEXEC;
    }
    memcpy(m->info.name, d->name, strlen(d->name) + 1);
    memcpy(m->info.kernel_build, kernel_build, sizeof(kernel_build));
    return 0;
}

static int remove_registrations(struct module *m)
{
    while (m->info.devices)
    {
        int result = lume_hardware_remove(m->devices[m->info.devices - 1]);
        if (result)
            return result;
        m->info.devices--;
    }
    while (m->info.drivers)
    {
        int result = lume_driver_unregister(m->drivers + m->info.drivers - 1);
        if (result)
            return result;
        m->info.drivers--;
    }
    return 0;
}

int lume_module_load(const void *file, size_t bytes, struct lume_module_info *info)
{
    if (!linker)
        return -LUME_ENOSYS;
    if (!info || !next_id)
        return -LUME_EINVAL;
    struct module *m = NULL;
    for (unsigned i = 0; i < LUME_MODULE_LIMIT; ++i)
        if (!modules[i].image.memory.address)
        {
            m = modules + i;
            break;
        }
    if (!m)
        return -LUME_ENOSPC;
    memset(m, 0, sizeof(*m));
    int result = lume_module_elf_load(file, bytes, linker, &m->image);
    if (!result)
        result = validate(m, true);
    const struct lume_module_descriptor *d = (void *)m->image.descriptor;
    if (!result)
    {
        m->info.id = next_id++;
        m->info.bytes = m->image.memory.bytes;
        for (size_t i = 0; i < d->driver_count && !result; ++i)
        {
            result = lume_driver_register(m->drivers + i);
            if (!result)
                m->info.drivers++;
        }
        for (size_t i = 0; i < d->device_count && !result; ++i)
        {
            const struct lume_module_virtual *v = d->devices + i;
            char path[LUME_HARDWARE_PATH] = "/virtual/";
            size_t end = strlen(path);
            memcpy(path + end, m->info.name, strlen(m->info.name));
            end += strlen(m->info.name);
            path[end++] = '/';
            memcpy(path + end, v->name, strlen(v->name) + 1);
            const struct lume_hardware_description description = {.origin = LUME_HARDWARE_VIRTUAL,
                                                                  .state = LUME_HARDWARE_DISCOVERED,
                                                                  .name = v->name,
                                                                  .path = path,
                                                                  .identities = v->identities,
                                                                  .identity_count =
                                                                      v->identity_count};
            result = lume_hardware_add(&description, m->devices + i);
            if (!result)
                m->info.devices++;
        }
    }
    if (result)
    {
        int cleanup = remove_registrations(m);
        if (cleanup)
            return cleanup;
        cleanup = lume_module_elf_release(&m->image);
        if (cleanup)
            return cleanup;
        memset(m, 0, sizeof(*m));
        return result;
    }
    *info = m->info;
    return 0;
}

int lume_module_unload(uint32_t id)
{
    for (unsigned i = 0; i < LUME_MODULE_LIMIT; ++i)
    {
        struct module *m = modules + i;
        if (!id || m->info.id != id)
            continue;
        if (m->info.references)
            return -LUME_EBUSY;
        for (unsigned j = 0; j < m->info.devices; ++j)
        {
            struct lume_hardware_info info;
            int result = lume_hardware_get(m->devices[j], &info);
            if (result || info.references || info.driver[0])
                return result ? result : -LUME_EBUSY;
        }
        int result = remove_registrations(m);
        if (!result)
            result = lume_module_elf_release(&m->image);
        if (!result)
            memset(m, 0, sizeof(*m));
        return result;
    }
    return -LUME_ENOENT;
}

int lume_module_inspect(const void *file, size_t bytes, struct lume_module_info *info)
{
    if (!linker)
        return -LUME_ENOSYS;
    if (!info)
        return -LUME_EINVAL;
    struct module *m = NULL;
    for (unsigned i = 0; i < LUME_MODULE_LIMIT; ++i)
        if (!modules[i].image.memory.address)
        {
            m = modules + i;
            break;
        }
    if (!m)
        return -LUME_ENOSPC;
    memset(m, 0, sizeof(*m));
    int result = lume_module_elf_load(file, bytes, linker, &m->image);
    if (!result)
        result = validate(m, false);
    if (!result)
    {
        const struct lume_module_descriptor *d = (void *)m->image.descriptor;
        m->info.bytes = m->image.memory.bytes;
        m->info.drivers = (uint32_t)d->driver_count;
        m->info.devices = (uint32_t)d->device_count;
        *info = m->info;
    }
    int cleanup = lume_module_elf_release(&m->image);
    if (!cleanup)
        memset(m, 0, sizeof(*m));
    return cleanup ? cleanup : result;
}

int lume_module_info(unsigned index, struct lume_module_info *info)
{
    if (!info)
        return -LUME_EINVAL;
    for (unsigned i = 0; i < LUME_MODULE_LIMIT; ++i)
        if (modules[i].info.id && !index--)
        {
            *info = modules[i].info;
            return 0;
        }
    return -LUME_ENOENT;
}

void lume_module_kernel_info(struct lume_module_info *info)
{
    memset(info, 0, sizeof(*info));
    memcpy(info->kernel_build, kernel_build, sizeof(kernel_build));
    info->bytes = lume_module_memory_available();
}
