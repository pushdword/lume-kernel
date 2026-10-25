#include "lume/fdt_inventory.h"
#include "lume/abi.h"
#include <libfdt.h>
#include <string.h>

static int cell_count(const void *tree, int node, const char *name, unsigned fallback)
{
    int bytes;
    const fdt32_t *value = fdt_getprop(tree, node, name, &bytes);
    if (!value)
        return bytes == -FDT_ERR_NOTFOUND ? (int)fallback : -LUME_EINVAL;
    if (bytes != 4 || fdt32_to_cpu(*value) > 8)
        return -LUME_EINVAL;
    return (int)fdt32_to_cpu(*value);
}

static uint64_t number(const fdt32_t *cells, unsigned count)
{
    uint64_t value = fdt32_to_cpu(cells[0]);
    return count == 2 ? (value << 32) | fdt32_to_cpu(cells[1]) : value;
}

static bool enabled(const void *tree, int node)
{
    while (node >= 0)
    {
        int bytes;
        const char *status = fdt_getprop(tree, node, "status", &bytes);
        if (status && !((bytes == 5 && !memcmp(status, "okay", 5)) ||
                        (bytes == 3 && !memcmp(status, "ok", 3))))
            return false;
        node = fdt_parent_offset(tree, node);
    }
    return true;
}

static int address_value(const fdt32_t *cells, unsigned count, uint32_t *space, uint64_t *address)
{
    if (count < 1 || count > 3)
        return -LUME_ENOSYS;
    *space = count == 3 ? fdt32_to_cpu(cells[0]) & 0x03000000u : 0;
    if (count == 3 && *space != 0x02000000u && *space != 0x03000000u)
        return -LUME_ENOSYS;
    *address = number(cells + (count == 3), count == 3 ? 2 : count);
    return 0;
}

static int translate(const void *tree, int bus, uint32_t space, uint64_t *address, uint64_t size)
{
    while (bus > 0)
    {
        int parent = fdt_parent_offset(tree, bus);
        int child_cells = cell_count(tree, bus, "#address-cells", 2);
        int parent_cells = cell_count(tree, parent, "#address-cells", 2);
        int size_cells = cell_count(tree, bus, "#size-cells", 1);
        if (parent < 0 || child_cells < 1 || child_cells > 3 || parent_cells < 1 ||
            parent_cells > 3 || size_cells < 1 || size_cells > 2)
            return -LUME_ENOSYS;
        int bytes;
        const fdt32_t *ranges = fdt_getprop(tree, bus, "ranges", &bytes);
        if (!ranges)
            return -LUME_ENOSYS;
        unsigned stride = (unsigned)(child_cells + parent_cells + size_cells);
        if (bytes < 0 || (unsigned)bytes % (stride * 4))
            return -LUME_EINVAL;
        if (!bytes && child_cells != parent_cells)
            return -LUME_ENOSYS;
        bool matched = !bytes;
        uint64_t translated = *address;
        uint32_t translated_space = space;
        for (unsigned i = 0; i < (unsigned)bytes / 4; i += stride)
        {
            uint64_t base, target;
            uint32_t source_space, target_space;
            int result = address_value(ranges + i, (unsigned)child_cells, &source_space, &base);
            if (result && child_cells == 3 && (fdt32_to_cpu(ranges[i]) & 0x03000000u) != space)
                continue;
            if (result)
                return result;
            if (source_space != space)
                continue;
            result = address_value(ranges + i + child_cells, (unsigned)parent_cells, &target_space,
                                   &target);
            if (result)
                return result;
            uint64_t length = number(ranges + i + child_cells + parent_cells, (unsigned)size_cells);
            if (base > UINT64_MAX - length || target > UINT64_MAX - length)
                return -LUME_EINVAL;
            if (source_space == space && *address >= base && size <= length &&
                *address - base <= length - size)
            {
                if (matched)
                    return -LUME_EINVAL;
                translated = target + (*address - base);
                translated_space = target_space;
                matched = true;
            }
        }
        if (!matched ||
            (parent_cells == 1 && (translated > UINT32_MAX || size - 1 > UINT32_MAX - translated)))
            return -LUME_ENOSYS;
        *address = translated;
        space = translated_space;
        bus = parent;
    }
    return space ? -LUME_ENOSYS : 0;
}

static int memory_resources(const void *tree, int node, struct lume_resource *resources,
                            unsigned *count)
{
    int bytes;
    const fdt32_t *reg = fdt_getprop(tree, node, "reg", &bytes);
    if (!reg)
        return bytes == -FDT_ERR_NOTFOUND ? 0 : -LUME_EINVAL;
    int parent = fdt_parent_offset(tree, node);
    int address_cells = cell_count(tree, parent, "#address-cells", 2);
    int size_cells = cell_count(tree, parent, "#size-cells", 1);
    if (parent < 0 || address_cells < 1 || address_cells > 3 || size_cells < 0 || size_cells > 2)
        return -LUME_ENOSYS;
    /* A zero size-cell count describes a bus-local identifier, not MMIO. */
    if (!size_cells)
        return bytes > 0 && !(bytes % (address_cells * 4)) ? 0 : -LUME_EINVAL;
    unsigned stride = (unsigned)(address_cells + size_cells);
    if (bytes <= 0 || (unsigned)bytes % (stride * 4))
        return -LUME_EINVAL;
    for (unsigned i = 0; i < (unsigned)bytes / 4; i += stride)
    {
        if (*count == LUME_HARDWARE_RESOURCES)
            return -LUME_E2BIG;
        uint64_t address;
        uint32_t space;
        int result = address_value(reg + i, (unsigned)address_cells, &space, &address);
        if (result)
            return result;
        uint64_t size = number(reg + i + address_cells, (unsigned)size_cells);
        if (!size || address > UINT64_MAX - size ||
            (address_cells == 1 && size - 1 > UINT32_MAX - address))
            return -LUME_EINVAL;
        result = translate(tree, parent, space, &address, size);
        if (result)
            return result;
        resources[(*count)++] =
            (struct lume_resource){.base = address, .size = size, .kind = LUME_RESOURCE_MMIO};
    }
    return 0;
}

static int interrupt_parent(const void *tree, int node)
{
    while (node >= 0)
    {
        int bytes;
        const fdt32_t *parent = fdt_getprop(tree, node, "interrupt-parent", &bytes);
        if (parent)
            return bytes == 4 ? fdt_node_offset_by_phandle(tree, fdt32_to_cpu(*parent)) : -1;
        node = fdt_parent_offset(tree, node);
        if (node >= 0 && fdt_getprop(tree, node, "interrupt-controller", NULL))
            return node;
    }
    return -1;
}

int lume_fdt_mmio_resource(const void *tree, int node, unsigned index, struct lume_resource *out)
{
    if (!out || node < 0)
        return -LUME_EINVAL;
    struct lume_resource resources[LUME_HARDWARE_RESOURCES];
    unsigned count = 0;
    int result = memory_resources(tree, node, resources, &count);
    if (result)
        return result;
    if (index >= count)
        return -LUME_ENOENT;
    *out = resources[index];
    return 0;
}

static int irq_resources(const void *tree, int node, const struct lume_fdt_inventory_ops *ops,
                         struct lume_resource *resources, unsigned *count)
{
    int bytes;
    const fdt32_t *data = fdt_getprop(tree, node, "interrupts-extended", &bytes);
    bool extended = data != NULL;
    if (!extended)
        data = fdt_getprop(tree, node, "interrupts", &bytes);
    if (!data)
        return bytes == -FDT_ERR_NOTFOUND ? 0 : -LUME_EINVAL;
    if (!ops || !ops->interrupt)
        return -LUME_ENOSYS;
    if (bytes <= 0 || bytes % 4)
        return -LUME_EINVAL;
    int parent = extended ? -1 : interrupt_parent(tree, node);
    unsigned cursor = 0;
    while (cursor < (unsigned)bytes / 4)
    {
        if (extended)
            parent = fdt_node_offset_by_phandle(tree, fdt32_to_cpu(data[cursor++]));
        if (parent < 0 || !enabled(tree, parent) ||
            !fdt_getprop(tree, parent, "interrupt-controller", NULL))
            return -LUME_ENOSYS;
        int cells = cell_count(tree, parent, "#interrupt-cells", 0);
        if (cells < 1 || cells > 8 || (unsigned)cells > (unsigned)bytes / 4 - cursor)
            return -LUME_EINVAL;
        if (*count == LUME_HARDWARE_RESOURCES)
            return -LUME_E2BIG;
        uint32_t arguments[8];
        for (unsigned i = 0; i < (unsigned)cells; ++i)
            arguments[i] = fdt32_to_cpu(data[cursor++]);
        struct lume_resource irq = {0};
        int result = ops->interrupt(tree, parent, arguments, (unsigned)cells, &irq);
        if (result)
            return result;
        if (irq.kind != LUME_RESOURCE_IRQ)
            return -LUME_EINVAL;
        resources[(*count)++] = irq;
    }
    return 0;
}

static int reference_property(const void *tree, int node, const char *name, unsigned index,
                              struct lume_property *out)
{
    const char *cells_name = !strcmp(name, "clocks")          ? "#clock-cells"
                             : !strcmp(name, "resets")        ? "#reset-cells"
                             : !strcmp(name, "power-domains") ? "#power-domain-cells"
                             : !strcmp(name, "phys")          ? "#phy-cells"
                                                              : NULL;
    bool single = !strcmp(name, "phy-handle");
    if (!cells_name && !single)
        return -LUME_ENOSYS;
    int bytes;
    const fdt32_t *data = fdt_getprop(tree, node, name, &bytes);
    if (!data)
        return bytes == -FDT_ERR_NOTFOUND ? -LUME_ENOENT : -LUME_EINVAL;
    if (bytes <= 0 || bytes % 4)
        return -LUME_EINVAL;
    if (single && (bytes != 4 || index))
        return -LUME_EINVAL;
    unsigned cursor = 0;
    while (cursor < (unsigned)bytes / 4)
    {
        int target = fdt_node_offset_by_phandle(tree, fdt32_to_cpu(data[cursor++]));
        if (target < 0)
            return -LUME_EINVAL;
        int count = single ? 0 : cell_count(tree, target, cells_name, 8);
        if (count < 0 || count > 7 || (unsigned)count > (unsigned)bytes / 4 - cursor)
            return -LUME_EINVAL;
        if (!index--)
        {
            uint32_t id = lume_hardware_firmware_id(tree, (uint64_t)target);
            if (!id)
                return -LUME_EAGAIN;
            out->count = (uint32_t)count + 1;
            out->values[0] = id;
            for (unsigned i = 0; i < (unsigned)count; ++i)
                out->values[i + 1] = fdt32_to_cpu(data[cursor + i]);
            return 0;
        }
        cursor += (unsigned)count;
    }
    return -LUME_ENOENT;
}

static int property(const void *tree, uint64_t token, const char *name, unsigned kind,
                    unsigned index, struct lume_property *out)
{
    if (token > INT32_MAX)
        return -LUME_EINVAL;
    int node = (int)token;
    out->kind = kind;
    if (kind == LUME_PROPERTY_REFERENCE)
        return reference_property(tree, node, name, index, out);
    int bytes;
    const void *data = fdt_getprop(tree, node, name, &bytes);
    if (!data)
        return bytes == -FDT_ERR_NOTFOUND ? -LUME_ENOENT : -LUME_EINVAL;
    if (kind == LUME_PROPERTY_U8)
    {
        if (bytes < 0 || index >= (unsigned)bytes)
            return -LUME_EINVAL;
        out->values[0] = ((const unsigned char *)data)[index];
        out->count = 1;
        return 0;
    }
    if (kind == LUME_PROPERTY_STRING)
    {
        if (index > INT32_MAX)
            return -LUME_EINVAL;
        const char *text = fdt_stringlist_get(tree, node, name, (int)index, &bytes);
        if (!text || bytes < 0)
            return -LUME_EINVAL;
        if ((size_t)bytes >= sizeof(out->string))
            return -LUME_E2BIG;
        memcpy(out->string, text, (size_t)bytes + 1);
        out->count = 1;
        return 0;
    }
    if (kind == LUME_PROPERTY_BOOLEAN)
    {
        if (bytes || index)
            return -LUME_EINVAL;
        out->values[0] = 1;
        out->count = 1;
        return 0;
    }
    unsigned width = kind == LUME_PROPERTY_U32 ? 4 : kind == LUME_PROPERTY_U64 ? 8 : 0;
    if (!width || bytes < 0 || (unsigned)bytes % width || index >= (unsigned)bytes / width)
        return -LUME_EINVAL;
    out->values[0] = number((const fdt32_t *)data + (size_t)index * (width / 4), width / 4);
    out->count = 1;
    return 0;
}

static const struct lume_firmware_ops firmware = {.property = property};

static bool valid_handles(const void *tree)
{
    int node = -1;
    while ((node = fdt_next_node(tree, node, NULL)) >= 0)
    {
        int bytes, old_bytes;
        const fdt32_t *handle = fdt_getprop(tree, node, "phandle", &bytes);
        const fdt32_t *old = fdt_getprop(tree, node, "linux,phandle", &old_bytes);
        if ((handle && bytes != 4) || (old && old_bytes != 4) || (handle && old && *handle != *old))
            return false;
        if (!handle)
            handle = old;
        if (handle && (!fdt32_to_cpu(*handle) || fdt32_to_cpu(*handle) == UINT32_MAX ||
                       fdt_node_offset_by_phandle(tree, fdt32_to_cpu(*handle)) != node))
            return false;
    }
    return node == -FDT_ERR_NOTFOUND;
}

static bool excluded(const void *tree, int node)
{
    int current = node;
    while (current > 0)
    {
        int bytes;
        const char *type = fdt_getprop(tree, current, "device_type", &bytes);
        if (type &&
            ((bytes == 7 && !memcmp(type, "memory", 7)) || (bytes == 4 && !memcmp(type, "cpu", 4))))
            return true;
        const char *name = fdt_get_name(tree, current, NULL);
        int parent = fdt_parent_offset(tree, current);
        if (!parent && (!strcmp(name, "reserved-memory") || !strcmp(name, "cpus")))
            return true;
        current = parent;
    }
    return false;
}

int lume_hardware_import_fdt(const void *tree, size_t bytes,
                             const struct lume_fdt_inventory_ops *ops, unsigned *added)
{
    if (!added)
        return -LUME_EINVAL;
    *added = 0;
    if (!tree || ((uintptr_t)tree & 7) || bytes > 0x200000 || fdt_check_full(tree, bytes) ||
        !valid_handles(tree))
        return -LUME_EINVAL;
    uint32_t imported[LUME_HARDWARE_LIMIT];
    unsigned count = 0;
    int result = 0, depth = 0, node = -1;
    while ((node = fdt_next_node(tree, node, &depth)) >= 0 && depth >= 0)
    {
        int strings = fdt_stringlist_count(tree, node, "compatible");
        if (!node || excluded(tree, node))
            continue;
        if (strings == -FDT_ERR_NOTFOUND)
        {
            /* Register-addressed providers can omit a compatible identifier. */
            if (!fdt_get_phandle(tree, node) || !fdt_getprop(tree, node, "reg", NULL))
                continue;
            strings = 0;
        }
        else if (strings <= 0)
        {
            result = -LUME_EINVAL;
            break;
        }
        if (count == LUME_HARDWARE_LIMIT)
        {
            result = -LUME_ENOSPC;
            break;
        }
        char path[LUME_HARDWARE_PATH];
        int name_bytes;
        const char *name = fdt_get_name(tree, node, &name_bytes);
        if (!name || name_bytes <= 0 || name_bytes >= (int)LUME_HARDWARE_NAME ||
            fdt_get_path(tree, node, path, sizeof(path)))
        {
            result = -LUME_E2BIG;
            break;
        }
        struct lume_hardware_identity identities[LUME_HARDWARE_IDS] = {0};
        struct lume_resource resources[LUME_HARDWARE_RESOURCES] = {0};
        unsigned resource_count = 0, identity_count = 0;
        bool unsupported = strings > (int)LUME_HARDWARE_IDS;
        int resource_error = unsupported ? -LUME_E2BIG : 0;
        for (int i = 0; i < strings && i < (int)LUME_HARDWARE_IDS; ++i)
        {
            int length;
            const char *compatible = fdt_stringlist_get(tree, node, "compatible", i, &length);
            if (!compatible || length <= 0 || length >= (int)LUME_HARDWARE_ID_BYTES)
            {
                unsupported = true;
                resource_error = -LUME_E2BIG;
                continue;
            }
            identities[identity_count].kind = LUME_HARDWARE_ID_DT;
            memcpy(identities[identity_count++].value, compatible, (size_t)length + 1);
        }
        int error = memory_resources(tree, node, resources, &resource_count);
        if (error)
            resource_error = error;
        error = irq_resources(tree, node, ops, resources, &resource_count);
        if (error && !resource_error)
            resource_error = error;
        unsupported = unsupported || resource_error != 0;
        uint32_t parent_id = 0;
        for (int parent = fdt_parent_offset(tree, node); parent > 0 && !parent_id;
             parent = fdt_parent_offset(tree, parent))
            parent_id = lume_hardware_firmware_id(tree, (uint64_t)parent);
        struct lume_hardware_description description = {
            .parent = parent_id,
            .origin = LUME_HARDWARE_DT,
            .state = !enabled(tree, node) ? LUME_HARDWARE_DISABLED
                     : unsupported        ? LUME_HARDWARE_UNSUPPORTED
                                          : LUME_HARDWARE_DISCOVERED,
            .error = resource_error,
            .name = name,
            .path = path,
            .identities = identities,
            .identity_count = identity_count,
            .resources = resources,
            .resource_count = resource_count,
            .firmware = &firmware,
            .firmware_context = tree,
            .firmware_node = (uint64_t)node,
        };
        result = lume_hardware_add(&description, imported + count);
        if (result)
            break;
        ++count;
    }
    if (result)
    {
        while (count)
            (void)lume_hardware_remove(imported[--count]);
        return result;
    }
    *added = count;
    return 0;
}
