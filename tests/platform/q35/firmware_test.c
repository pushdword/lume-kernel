#include "lume/firmware.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned char map[72];

static void put32(unsigned offset, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i)
        map[offset + i] = (unsigned char)(value >> (i * 8));
}

static void entry(unsigned index, uint64_t base, uint64_t length, unsigned type)
{
    unsigned offset = index * 24;
    put32(offset, 20);
    put32(offset + 4, (uint32_t)base);
    put32(offset + 8, (uint32_t)(base >> 32));
    put32(offset + 12, (uint32_t)length);
    put32(offset + 16, (uint32_t)(length >> 32));
    put32(offset + 20, type);
}

int main(void)
{
    entry(0, 0x100000, 0x1fede000, 1);
    entry(1, 0x1ffde000, 0x22000, 2);
    entry(2, 0xb0000000, 0x10000000, 2);
    assert(lume_q35_firmware_range(map, sizeof(map), 0xf52d0, 20));
    assert(lume_q35_firmware_range(map, sizeof(map), 0x9fc00, 20));
    assert(lume_q35_firmware_range(map, sizeof(map), 0x100000, 36));
    assert(lume_q35_firmware_range(map, sizeof(map), 0x1ffe21cd, 56));
    assert(lume_q35_firmware_range(map, sizeof(map), 0x1ffffff0, 16));
    assert(!lume_q35_firmware_range(map, sizeof(map), 0x1ffffff0, 17));
    assert(!lume_q35_firmware_range(map, sizeof(map), 0xa0000, 36));
    assert(!lume_q35_firmware_range(map, sizeof(map), 0xc0000, 36));
    assert(!lume_q35_firmware_range(map, sizeof(map), 0xb0000000, 36));
    assert(!lume_q35_firmware_range(map, sizeof(map), 0x40000000, 36));
    assert(!lume_q35_firmware_range(map, sizeof(map), UINT64_MAX - 10, 36));
    assert(!lume_q35_firmware_range(map, sizeof(map), 0x1ffe21cd, 0));
    entry(1, 0x1ffdf000, 0x21000, 2);
    assert(!lume_q35_firmware_range(map, sizeof(map), 0x1ffe21cd, 56));
    entry(1, 0x1ffdf000, 0x21000, 3);
    assert(lume_q35_firmware_range(map, sizeof(map), 0x1ffe21cd, 56));
    entry(1, 0x1ffdf000, 0x21000, 4);
    assert(lume_q35_firmware_range(map, sizeof(map), 0x1ffe21cd, 56));
    entry(1, 0x1ffde000, 0x22000, 2);
    put32(0, UINT32_MAX);
    assert(!lume_q35_firmware_range(map, sizeof(map), 0x1ffe21cd, 56));
    assert(!lume_q35_firmware_range(map, sizeof(map), 0xf52d0, 20));
    entry(0, 0x100000, 0x1fede000, 1);
    assert(!lume_q35_firmware_range(map, sizeof(map) - 1, 0x1ffe21cd, 56));
    entry(2, UINT64_MAX - 10, 20, 1);
    assert(!lume_q35_firmware_range(map, sizeof(map), 0x1ffe21cd, 56));
    memset(map, 0, sizeof(map));
    assert(!lume_q35_firmware_range(map, sizeof(map), 0xf52d0, 20));
    puts(
        "FIRMWARE_PASS SeaBIOS reserved ACPI tail, E820 bounds, MMIO exclusion and malformed maps");
    return 0;
}
