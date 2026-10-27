#include "lume/acpi.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned char memory[0x100000];

static bool read_memory(void *context, uint64_t address, void *out, size_t bytes)
{
    (void)context;
    if (address >= sizeof(memory) || bytes > sizeof(memory) - address)
        return false;
    memcpy(out, memory + address, bytes);
    return true;
}

static void put32(unsigned address, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i)
        memory[address + i] = (unsigned char)(value >> (i * 8));
}

static void sum(unsigned address, unsigned bytes, unsigned field)
{
    memory[address + field] = 0;
    unsigned value = 0;
    for (unsigned i = 0; i < bytes; ++i)
        value += memory[address + i];
    memory[address + field] = (unsigned char)-value;
}

static void table(unsigned address, const char *signature, unsigned length)
{
    memcpy(memory + address, signature, 4);
    put32(address + 4, length);
    sum(address, length, 9);
}

static void fixture(bool extended)
{
    memset(memory, 0, sizeof(memory));
    unsigned root = 0x10000, apic = 0x11000, mcfg = 0x12000, fadt = 0x13000;
    put32(root + 36, apic);
    put32(root + 36 + (extended ? 8 : 4), mcfg);
    put32(root + 36 + (extended ? 16 : 8), fadt);
    table(root, extended ? "XSDT" : "RSDT", 36 + (extended ? 24 : 12));
    put32(apic + 36, 0xfee00000);
    memory[apic + 44] = 0;
    memory[apic + 45] = 8;
    put32(apic + 48, 1);
    memory[apic + 52] = 1;
    memory[apic + 53] = 12;
    put32(apic + 56, 0xfec00000);
    table(apic, "APIC", 64);
    put32(mcfg + 44, 0xb0000000);
    memory[mcfg + 55] = 255;
    table(mcfg, "MCFG", 60);
    memory[fadt + 109] = 2;
    table(fadt, "FACP", 111);
    memcpy(memory + 0xe0100, "RSD PTR ", 8);
    memory[0xe010f] = extended ? 2 : 0;
    put32(0xe0110, root);
    put32(0xe0114, 36);
    put32(0xe0118, root);
    sum(0xe0100, 20, 8);
    sum(0xe0100, 36, 32);
}

static void cpu_fixture(unsigned count)
{
    fixture(true);
    unsigned char ioapic[12];
    memcpy(ioapic, memory + 0x11034, sizeof(ioapic));
    for (unsigned i = 0; i < count; ++i)
    {
        unsigned address = 0x1102c + i * 8;
        memory[address] = 0;
        memory[address + 1] = 8;
        memory[address + 2] = (unsigned char)i;
        memory[address + 3] = (unsigned char)i;
        put32(address + 4, 1);
    }
    memcpy(memory + 0x1102c + count * 8, ioapic, sizeof(ioapic));
    table(0x11000, "APIC", 44 + count * 8 + sizeof(ioapic));
}

static void cpu_topology(void)
{
    struct lume_acpi acpi, saved;
    cpu_fixture(4);
    const unsigned char ids[] = {2, 0, 6, 9};
    for (unsigned i = 0; i < 4; ++i)
        memory[0x1102f + i * 8] = ids[i];
    sum(0x11000, 88, 9);
    assert(lume_acpi_read(&acpi, read_memory, NULL, 0xe0100) && acpi.cpus == 4);
    for (unsigned i = 0; i < 4; ++i)
        assert(acpi.apic_ids[i] == ids[i]);
    put32(0x11030 + 2 * 8, 2);
    sum(0x11000, 88, 9);
    assert(lume_acpi_read(&acpi, read_memory, NULL, 0xe0100) && acpi.cpus == 3);
    assert(acpi.apic_ids[2] == 9);
    saved = acpi;
    memory[0x1102f + 3 * 8] = 2;
    sum(0x11000, 88, 9);
    assert(!lume_acpi_read(&acpi, read_memory, NULL, 0xe0100));
    assert(!memcmp(&acpi, &saved, sizeof(acpi)));
    cpu_fixture(256);
    assert(lume_acpi_read(&acpi, read_memory, NULL, 0xe0100) && acpi.cpus == 256);
    assert(acpi.apic_ids[255] == 255);
    cpu_fixture(257);
    assert(!lume_acpi_read(&acpi, read_memory, NULL, 0xe0100));
    fixture(true);
    memset(memory + 0x1102c, 0, 16);
    memory[0x1102c] = 9;
    memory[0x1102d] = 16;
    put32(0x11030, 300);
    put32(0x11034, 1);
    table(0x11000, "APIC", 60);
    assert(lume_acpi_read(&acpi, read_memory, NULL, 0xe0100) && acpi.cpus == 1);
    assert(acpi.apic_ids[0] == 300);
    puts(
        "ACPI_CPU_PASS sparse IDs, enabled CPUs, duplicate rejection, capacity and x2APIC records");
}

int main(void)
{
    struct lume_acpi acpi;
    for (unsigned extended = 0; extended < 2; ++extended)
    {
        fixture(extended);
        assert(lume_acpi_find(&acpi, read_memory, NULL, 0));
        assert(acpi.rsdp == 0xe0100 && acpi.table_count == 4 && acpi.segment_count == 1);
        assert(acpi.cpus == 1 && acpi.ioapics == 1 && acpi.lapic == 0xfee00000);
        assert(acpi.legacy_keyboard && acpi.segments[0].base == 0xb0000000);
        assert(acpi.segments[0].first == 0 && acpi.segments[0].last == 255);
    }
    struct lume_acpi saved = acpi;
    memory[0x12009]++;
    assert(!lume_acpi_read(&acpi, read_memory, NULL, 0xe0100));
    assert(!memcmp(&acpi, &saved, sizeof(acpi)));
    fixture(true);
    memory[0x1102d] = 0;
    sum(0x11000, 64, 9);
    assert(!lume_acpi_read(&acpi, read_memory, NULL, 0xe0100));
    fixture(true);
    put32(0x1202c, 0xb0001000);
    sum(0x12000, 60, 9);
    assert(!lume_acpi_read(&acpi, read_memory, NULL, 0xe0100));
    fixture(true);
    memcpy(memory + 0x1203c, memory + 0x1202c, 16);
    table(0x12000, "MCFG", 76);
    assert(!lume_acpi_read(&acpi, read_memory, NULL, 0xe0100));
    fixture(true);
    put32(0x10024, 0xfffffff0);
    sum(0x10000, 60, 9);
    assert(!lume_acpi_read(&acpi, read_memory, NULL, 0xe0100));
    fixture(true);
    put32(0xe0114, UINT32_MAX);
    assert(!lume_acpi_read(&acpi, read_memory, NULL, 0xe0100));
    fixture(false);
    memcpy(memory + 0x90000, memory + 0xe0100, 36);
    memset(memory + 0xe0100, 0, 36);
    assert(lume_acpi_find(&acpi, read_memory, NULL, 0x90000) && acpi.rsdp == 0x90000);
    cpu_topology();
    puts("ACPI_PASS RSDT/XSDT, EBDA/BIOS search, checksums, bounds, MADT, FADT and MCFG conflicts");
    return 0;
}
