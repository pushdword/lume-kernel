#include "lume/acpi.h"
#include "lume/firmware.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned char low[0x100000], high[0x200000];
static const void *map;
static size_t map_bytes;

static bool read_memory(void *context, uint64_t address, void *out, size_t bytes)
{
    (void)context;
    if (map && !lume_q35_firmware_range(map, map_bytes, address, bytes))
        return false;
    if (address < sizeof(low) && bytes <= sizeof(low) - address)
    {
        memcpy(out, low + address, bytes);
        return true;
    }
    if (address >= 0x1fe00000 && address - 0x1fe00000 < sizeof(high) &&
        bytes <= sizeof(high) - (address - 0x1fe00000))
    {
        memcpy(out, high + address - 0x1fe00000, bytes);
        return true;
    }
    return false;
}

int main(int argc, char **argv)
{
    assert(argc == 3 || (argc == 4 && !strcmp(argv[3], "multiboot")));
    FILE *file = fopen(argv[1], "rb");
    assert(file && fread(low, 1, sizeof(low), file) == sizeof(low));
    fclose(file);
    if (argc == 4)
    {
        uint32_t bytes, address;
        memcpy(&bytes, low + 0x9500 + 44, sizeof(bytes));
        memcpy(&address, low + 0x9500 + 48, sizeof(address));
        assert(address < sizeof(low) && bytes && bytes <= sizeof(low) - address);
        map = low + address;
        map_bytes = bytes;
    }
    file = fopen(argv[2], "rb");
    assert(file && fread(high, 1, sizeof(high), file) == sizeof(high));
    fclose(file);
    struct lume_acpi acpi;
    uint64_t ebda = (uint64_t)(low[0x40e] | low[0x40f] << 8) << 4;
    assert(lume_acpi_find(&acpi, read_memory, NULL, ebda));
    assert(acpi.table_count >= 3 && acpi.cpus == 1 && acpi.ioapics == 1 && acpi.segment_count == 1);
    assert(acpi.segments[0].base == 0xb0000000 && !acpi.segments[0].segment);
    if (map)
        puts("MULTIBOOT_FIRMWARE_RANGE_PASS production E820 filtering");
    printf("ACPI_FIRMWARE_PASS rsdp=0x%llx tables=%u cpus=%u ioapics=%u ecam=0x%llx\n",
           (unsigned long long)acpi.rsdp, acpi.table_count, acpi.cpus, acpi.ioapics,
           (unsigned long long)acpi.segments[0].base);
    for (unsigned i = 0; i < acpi.table_count; ++i)
        printf("%s address=0x%llx bytes=%u\n", acpi.tables[i].signature,
               (unsigned long long)acpi.tables[i].address, acpi.tables[i].length);
    return 0;
}
