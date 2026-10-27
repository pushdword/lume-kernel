#ifndef LUME_ACPI_H
#define LUME_ACPI_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LUME_ACPI_TABLES 64u
#define LUME_ACPI_SEGMENTS 8u
#define LUME_ACPI_CPUS 256u

struct lume_acpi_table
{
    uint64_t address;
    uint32_t length;
    char signature[5];
};

struct lume_acpi_segment
{
    uint64_t base;
    uint16_t segment;
    uint8_t first, last;
};

struct lume_acpi
{
    uint64_t rsdp, lapic;
    unsigned revision, table_count, segment_count, cpus, ioapics;
    bool legacy_keyboard;
    struct lume_acpi_table tables[LUME_ACPI_TABLES];
    struct lume_acpi_segment segments[LUME_ACPI_SEGMENTS];
    uint32_t apic_ids[LUME_ACPI_CPUS];
};

typedef bool (*lume_acpi_reader)(void *, uint64_t, void *, size_t);
/* Physical reads must reject unmapped ranges. No pointers into firmware survive parsing. */
bool lume_acpi_read(struct lume_acpi *, lume_acpi_reader, void *, uint64_t rsdp);
bool lume_acpi_find(struct lume_acpi *, lume_acpi_reader, void *, uint64_t ebda);
const struct lume_acpi *lume_q35_acpi(void);
#endif
