#ifndef LUME_HARDWARE_PLATFORM_H
#define LUME_HARDWARE_PLATFORM_H
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

struct lume_fdt_window
{
    uint64_t cpu, bus, size;
};

int lume_platform_fdt_copy(const void *tree, size_t bytes, const char *enabled,
                           size_t enabled_bytes, const struct lume_fdt_window *window, void *output,
                           size_t capacity);
int lume_platform_fdt_configuration(char *buffer, size_t capacity, size_t *bytes);
bool lume_boot_rp1_window(struct lume_fdt_window *window);

int lume_platform_hardware_start(const void *tree, size_t bytes);
#endif
