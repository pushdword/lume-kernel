#include "lume/firmware.h"

#define EARLY_LIMIT UINT64_C(0x40000000)

static uint32_t u32(const unsigned char *p)
{
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t u64(const unsigned char *p)
{
    return u32(p) | (uint64_t)u32(p + 4) << 32;
}

static bool contains(uint64_t base, uint64_t length, uint64_t address, size_t bytes)
{
    return address >= base && address - base <= length && bytes <= length - (address - base);
}

bool lume_q35_firmware_range(const void *map, size_t map_bytes, uint64_t address, size_t bytes)
{
    if (!map || !map_bytes || map_bytes > 65536 || !address || !bytes || address >= EARLY_LIMIT ||
        bytes > EARLY_LIMIT - address)
        return false;
    const unsigned char *records = map;
    unsigned count = 0;
    bool allowed =
        contains(0x80000, 0x20000, address, bytes) || contains(0xe0000, 0x20000, address, bytes);
    uint64_t reserved_base = 0;
    for (size_t offset = 0; offset < map_bytes;)
    {
        if (map_bytes - offset < 4 || ++count > 256)
            return false;
        const unsigned char *p = records + offset;
        uint64_t record_bytes = (uint64_t)u32(p) + 4;
        if (record_bytes < 24 || record_bytes > map_bytes - offset)
            return false;
        uint64_t base = u64(p + 4), length = u64(p + 12);
        unsigned type = u32(p + 20);
        if (length > UINT64_MAX - base)
            return false;
        if (contains(base, length, address, bytes))
        {
            if (type == 1 || type == 3 || type == 4)
                allowed = true;
            /* SeaBIOS keeps its ACPI allocation in the reserved tail of low RAM. */
            if (type == 2 && base >= 0x100000 && base < EARLY_LIMIT && length <= EARLY_LIMIT - base)
                reserved_base = base;
        }
        offset += (size_t)record_bytes;
    }
    if (allowed)
        return true;
    if (reserved_base)
        for (size_t offset = 0; offset < map_bytes;)
        {
            const unsigned char *p = records + offset;
            if (u32(p + 20) == 1 && u64(p + 12) && u64(p + 4) + u64(p + 12) == reserved_base)
                return true;
            offset += (size_t)u32(p) + 4;
        }
    return false;
}
