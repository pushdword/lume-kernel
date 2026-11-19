#ifndef LUME_Q35_FIRMWARE_H
#define LUME_Q35_FIRMWARE_H

#if defined(LUME_ARCH_I686)
#define LUME_Q35_EARLY_GIB 1
#else
#define LUME_Q35_EARLY_GIB 4
#endif

#ifndef __ASSEMBLER__
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LUME_Q35_EARLY_LIMIT (UINT64_C(0x40000000) * LUME_Q35_EARLY_GIB)
#define LUME_MULTIBOOT2_MAGIC UINT64_C(0x36d76289)

#if defined(LUME_ARCH_AMD64)
struct lume_pc_framebuffer
{
    uint64_t base;
    uint32_t pitch, width, height;
    bool red_low;
};
struct lume_fdt;
const struct lume_pc_framebuffer *lume_q35_boot_framebuffer(void);
bool lume_amd64_framebuffer_start(const struct lume_fdt *);
bool lume_amd64_entropy_start(void);
#endif

/* Multiboot E820 records include a four-byte length before each BIOS entry. */
bool lume_q35_firmware_range(const void *map, size_t map_bytes, uint64_t address, size_t bytes);
#endif
#endif
