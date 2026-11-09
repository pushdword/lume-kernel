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

/* Multiboot E820 records include a four-byte length before each BIOS entry. */
bool lume_q35_firmware_range(const void *map, size_t map_bytes, uint64_t address, size_t bytes);
#endif
#endif
