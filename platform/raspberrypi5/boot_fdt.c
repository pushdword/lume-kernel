#include "lume/hardware_platform.h"
#include "lume/abi.h"
#include <libfdt.h>
#include <string.h>

static bool selected(const char *config, size_t bytes, const char *path)
{
    size_t length = strlen(path);
    for (size_t offset = 0; offset < bytes;)
    {
        size_t end = offset;
        while (end < bytes && config[end] != '\n')
            ++end;
        size_t stop = end;
        if (stop > offset && config[stop - 1] == '\r')
            --stop;
        if (stop - offset == length && !memcmp(config + offset, path, length))
            return true;
        offset = end + 1;
    }
    return false;
}

static int copy_node(const void *tree, int node, void *output, const char *enabled,
                     size_t enabled_bytes, const struct lume_fdt_window *window)
{
    const char *name = fdt_get_name(tree, node, NULL);
    char path[256];
    if (!name || fdt_get_path(tree, node, path, sizeof(path)))
        return -LUME_EINVAL;
    bool enable = selected(enabled, enabled_bytes, path);
    bool pcie = !strcmp(path, "/axi/pcie@1000120000") &&
                !fdt_node_check_compatible(tree, node, "brcm,bcm2712-pcie");
    bool rp1 = !strcmp(path, "/axi/pcie@1000120000/rp1") &&
               !fdt_node_check_compatible(tree, node, "simple-bus");
    if (pcie &&
        (!window || window->cpu != UINT64_C(0x1c00000000) || window->size < 0x400000 ||
         window->size > UINT64_MAX - window->bus || window->size > UINT64_MAX - window->cpu))
        return -LUME_ENOSYS;
    uint32_t space =
        window && (window->bus > UINT32_MAX || window->size > UINT64_C(0x100000000) - window->bus)
            ? 0x03000000
            : 0x02000000;
    int result = fdt_begin_node(output, name);
    if (result)
        return result;
    for (int property = fdt_first_property_offset(tree, node); property >= 0;
         property = fdt_next_property_offset(tree, property))
    {
        int bytes;
        const char *key;
        const void *value = fdt_getprop_by_offset(tree, property, &key, &bytes);
        if (!value || bytes < 0)
            return -LUME_EINVAL;
        if (enable && !strcmp(key, "status"))
            continue;
        if (pcie && !strcmp(key, "ranges"))
        {
            /* Native boot inherits the active window instead of assigning Linux PCI resources. */
            fdt32_t ranges[] = {cpu_to_fdt32(space),
                                cpu_to_fdt32((uint32_t)(window->bus >> 32)),
                                cpu_to_fdt32((uint32_t)window->bus),
                                cpu_to_fdt32((uint32_t)(window->cpu >> 32)),
                                cpu_to_fdt32((uint32_t)window->cpu),
                                cpu_to_fdt32((uint32_t)(window->size >> 32)),
                                cpu_to_fdt32((uint32_t)window->size)};
            result = fdt_property(output, key, ranges, sizeof(ranges));
        }
        else if (rp1 && !strcmp(key, "ranges"))
        {
            const fdt32_t *old = value;
            uint64_t old_bus =
                bytes == 28 ? (uint64_t)fdt32_to_cpu(old[3]) << 32 | fdt32_to_cpu(old[4]) : 0;
            if (!window || bytes != 28 || (fdt32_to_cpu(old[2]) & 0x03000000) != 0x02000000 ||
                (old_bus && old_bus != window->bus))
                return -LUME_ENOSYS;
            fdt32_t ranges[7];
            memcpy(ranges, old, sizeof(ranges));
            ranges[2] = cpu_to_fdt32(space);
            ranges[3] = cpu_to_fdt32((uint32_t)(window->bus >> 32));
            ranges[4] = cpu_to_fdt32((uint32_t)window->bus);
            result = fdt_property(output, key, ranges, sizeof(ranges));
        }
        else
            result = fdt_property(output, key, value, bytes);
        if (result)
            return result;
    }
    if (enable && (result = fdt_property_string(output, "status", "okay")))
        return result;
    int child;
    fdt_for_each_subnode(child, tree, node)
    {
        result = copy_node(tree, child, output, enabled, enabled_bytes, window);
        if (result)
            return result;
    }
    return fdt_end_node(output);
}

int lume_platform_fdt_copy(const void *tree, size_t bytes, const char *enabled,
                           size_t enabled_bytes, const struct lume_fdt_window *window, void *output,
                           size_t capacity)
{
    if (!tree || !output || tree == output || bytes > 0x200000 || capacity > 0x200000 ||
        enabled_bytes > 4096 || (!enabled && enabled_bytes) || fdt_check_full(tree, bytes))
        return -LUME_EINVAL;
    int result = fdt_create(output, (int)capacity);
    if (result)
        return result;
    int reservations = fdt_num_mem_rsv(tree);
    for (int i = 0; i < reservations; ++i)
    {
        uint64_t address, size;
        if (fdt_get_mem_rsv(tree, i, &address, &size) ||
            (result = fdt_add_reservemap_entry(output, address, size)))
            return result ? result : -LUME_EINVAL;
    }
    if ((result = fdt_finish_reservemap(output)) ||
        (result = copy_node(tree, 0, output, enabled, enabled_bytes, window)) ||
        (result = fdt_finish(output)))
        return result;
    fdt_set_boot_cpuid_phys(output, fdt_boot_cpuid_phys(tree));
    return fdt_check_full(output, fdt_totalsize(output)) ? -LUME_EINVAL : 0;
}
