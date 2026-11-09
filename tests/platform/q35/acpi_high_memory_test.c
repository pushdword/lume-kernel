#include "lume/acpi.h"
#include "lume/firmware.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define HIGH_BASE UINT64_C(0xbffe0000)
#define HIGH_BYTES 0x20000u

static unsigned char low[0x100000], high[HIGH_BYTES], map[6 * 24];
static unsigned checks, reads;

static void put32(unsigned char *p, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i)
        p[i] = (unsigned char)(value >> (i * 8));
}

static void put64(unsigned char *p, uint64_t value)
{
    put32(p, (uint32_t)value);
    put32(p + 4, (uint32_t)(value >> 32));
}

static void entry(unsigned index, uint64_t base, uint64_t bytes, unsigned type)
{
    unsigned char *p = map + index * 24;
    put32(p, 20);
    put64(p + 4, base);
    put64(p + 12, bytes);
    put32(p + 20, type);
}

static void checksum(unsigned char *p, unsigned bytes, unsigned field)
{
    p[field] = 0;
    unsigned sum = 0;
    for (unsigned i = 0; i < bytes; ++i)
        sum += p[i];
    p[field] = (unsigned char)-sum;
}

static void table(unsigned offset, const char *signature, unsigned bytes)
{
    unsigned char *p = high + offset;
    memcpy(p, signature, 4);
    put32(p + 4, bytes);
    checksum(p, bytes, 9);
}

static bool read_memory(void *context, uint64_t address, void *out, size_t bytes)
{
    (void)context;
    ++reads;
    if (!lume_q35_firmware_range(map, sizeof(map), address, bytes))
        return false;
    if (address < sizeof(low) && bytes <= sizeof(low) - address)
    {
        memcpy(out, low + (size_t)address, bytes);
        return true;
    }
    if (address >= HIGH_BASE && address - HIGH_BASE < sizeof(high) &&
        bytes <= sizeof(high) - (size_t)(address - HIGH_BASE))
    {
        memcpy(out, high + (size_t)(address - HIGH_BASE), bytes);
        return true;
    }
    return false;
}

static void fixture(bool extended)
{
    memset(low, 0, sizeof(low));
    memset(high, 0, sizeof(high));
    memset(map, 0, sizeof(map));
    reads = 0;
    entry(0, 0, 0x9fc00, 1);
    entry(1, 0x9fc00, 0x60400, 2);
    entry(2, 0x100000, HIGH_BASE - 0x100000, 1);
    entry(3, HIGH_BASE, sizeof(high), 2);
    entry(4, HIGH_BASE + sizeof(high), UINT64_C(0x100000000) - HIGH_BASE - sizeof(high), 2);
    entry(5, UINT64_C(0x100000000), UINT64_C(0x40000000), 1);

    unsigned width = extended ? 8 : 4;
    for (unsigned i = 0; i < 3; ++i)
        if (extended)
            put64(high + 0x100 + 36 + i * width, HIGH_BASE + 0x1000 + i * 0x1000);
        else
            put32(high + 0x100 + 36 + i * width, (uint32_t)(HIGH_BASE + 0x1000 + i * 0x1000));
    table(0x100, extended ? "XSDT" : "RSDT", 36 + 3 * width);
    unsigned char *apic = high + 0x1000;
    put32(apic + 36, 0xfee00000);
    const unsigned char ids[] = {0, 2, 6, 9};
    for (unsigned i = 0; i < 4; ++i)
    {
        unsigned char *cpu = apic + 44 + i * 8;
        cpu[0] = 0;
        cpu[1] = 8;
        cpu[2] = (unsigned char)i;
        cpu[3] = ids[i];
        put32(cpu + 4, 1);
    }
    apic[76] = 1;
    apic[77] = 12;
    put32(apic + 80, 0xfec00000);
    table(0x1000, "APIC", 88);
    put64(high + 0x2000 + 44, 0xe0000000);
    high[0x2000 + 55] = 255;
    table(0x2000, "MCFG", 60);
    high[0x3000 + 109] = 2;
    table(0x3000, "FACP", 111);
    unsigned char *rsdp = low + 0xe0100;
    memcpy(rsdp, "RSD PTR ", 8);
    rsdp[15] = extended ? 2 : 0;
    put32(rsdp + 16, (uint32_t)(HIGH_BASE + 0x100));
    put32(rsdp + 20, 36);
    if (extended)
        put64(rsdp + 24, HIGH_BASE + 0x100);
    checksum(rsdp, 20, 8);
    checksum(rsdp, 36, 32);
}

static void topology(void)
{
    for (unsigned extended = 0; extended < 2; ++extended)
    {
        fixture(extended);
        struct lume_acpi acpi;
        assert(lume_acpi_find(&acpi, read_memory, NULL, 0));
        assert(acpi.rsdp == 0xe0100 && acpi.table_count == 4 && acpi.cpus == 4);
        assert(acpi.apic_ids[0] == 0 && acpi.apic_ids[1] == 2 && acpi.apic_ids[2] == 6 &&
               acpi.apic_ids[3] == 9);
        assert(acpi.ioapics == 1 && acpi.lapic == 0xfee00000 && acpi.segment_count == 1);
        assert(acpi.segments[0].base == 0xe0000000 && acpi.legacy_keyboard);
        assert(reads);
        ++checks;
    }
}

static void bounds(void)
{
    fixture(true);
    assert(lume_q35_firmware_range(map, sizeof(map), HIGH_BASE, sizeof(high)));
    assert(!lume_q35_firmware_range(map, sizeof(map), HIGH_BASE, sizeof(high) + 1));
    assert(!lume_q35_firmware_range(map, sizeof(map), 0xe0000000, 36));
    assert(!lume_q35_firmware_range(map, sizeof(map), 0xfec00000, 36));
    assert(!lume_q35_firmware_range(map, sizeof(map), 0xfee00000, 36));
    assert(!lume_q35_firmware_range(map, sizeof(map), UINT64_C(0x100000000), 1));
    assert(!lume_q35_firmware_range(map, sizeof(map), UINT64_MAX - 10, 36));
    assert(!lume_q35_firmware_range(map, sizeof(map), HIGH_BASE, 0));
    checks += 8;

    entry(2, 0x100000, UINT64_C(0xb0000000) - 0x100000, 1);
    entry(3, 0xb0000000, 0x50000000, 2);
    assert(!lume_q35_firmware_range(map, sizeof(map), 0xb0000000, 36));
    assert(!lume_q35_firmware_range(map, sizeof(map), 0xfec00000, 36));
    checks += 2;

    fixture(true);
    entry(3, 0xfffff000, 4096, 3);
    assert(lume_q35_firmware_range(map, sizeof(map), 0xfffffff0, 16));
    assert(!lume_q35_firmware_range(map, sizeof(map), 0xfffffff0, 17));
    checks += 2;

    fixture(true);
    entry(3, HIGH_BASE + 4096, sizeof(high) - 4096, 2);
    assert(!lume_q35_firmware_range(map, sizeof(map), HIGH_BASE + 4096, 36));
    entry(3, HIGH_BASE + 4096, sizeof(high) - 4096, 3);
    assert(lume_q35_firmware_range(map, sizeof(map), HIGH_BASE + 4096, 36));
    entry(3, HIGH_BASE + 4096, sizeof(high) - 4096, 4);
    assert(lume_q35_firmware_range(map, sizeof(map), HIGH_BASE + 4096, 36));
    checks += 3;
}

static void malformed(void)
{
    fixture(true);
    assert(!lume_q35_firmware_range(map, sizeof(map) - 1, HIGH_BASE, 36));
    assert(!lume_q35_firmware_range(NULL, sizeof(map), HIGH_BASE, 36));
    put32(map, UINT32_MAX);
    assert(!lume_q35_firmware_range(map, sizeof(map), HIGH_BASE, 36));
    fixture(true);
    entry(5, UINT64_MAX - 10, 20, 1);
    assert(!lume_q35_firmware_range(map, sizeof(map), HIGH_BASE, 36));
    checks += 4;

    fixture(true);
    struct lume_acpi acpi = {0};
    high[0x1000 + 9]++;
    assert(!lume_acpi_read(&acpi, read_memory, NULL, 0xe0100) && !acpi.rsdp);
    fixture(true);
    put64(high + 0x100 + 36, 0xe0000000);
    table(0x100, "XSDT", 60);
    assert(!lume_acpi_read(&acpi, read_memory, NULL, 0xe0100) && !acpi.rsdp);
    checks += 2;
}

int main(void)
{
    _Static_assert(LUME_Q35_EARLY_GIB == 4, "AMD64 bootstrap covers four GiB");
    topology();
    bounds();
    malformed();
    printf(
        "ACPI_HIGH_MEMORY_PASS %u checks cover four-CPU tables above one GiB, E820 bounds and MMIO rejection\n",
        checks);
    return 0;
}
