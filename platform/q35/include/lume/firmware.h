#ifndef LUME_Q35_FIRMWARE_H
#define LUME_Q35_FIRMWARE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Multiboot E820 records include a four-byte length before each BIOS entry. */
bool lume_q35_firmware_range(const void *map, size_t map_bytes, uint64_t address, size_t bytes);
#endif
