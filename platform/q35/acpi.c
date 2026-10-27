#include "lume/acpi.h"
#include <string.h>

static uint32_t u32(const unsigned char *p)
{
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t u64(const unsigned char *p)
{
    return u32(p) | (uint64_t)u32(p + 4) << 32;
}

static bool checksum(lume_acpi_reader read, void *context, uint64_t address, uint32_t length)
{
    unsigned char bytes[128], sum = 0;
    if (!address || address > UINT64_MAX - length || length > 1048576)
        return false;
    while (length)
    {
        unsigned n = length < sizeof(bytes) ? length : sizeof(bytes);
        if (!read(context, address, bytes, n))
            return false;
        for (unsigned i = 0; i < n; ++i)
            sum = (unsigned char)(sum + bytes[i]);
        length -= n;
        address += n;
    }
    return !sum;
}

static bool header(lume_acpi_reader read, void *context, uint64_t address,
                   struct lume_acpi_table *out)
{
    unsigned char bytes[36];
    if (!read(context, address, bytes, sizeof(bytes)))
        return false;
    uint32_t length = u32(bytes + 4);
    if (length < 36 || !checksum(read, context, address, length))
        return false;
    *out = (struct lume_acpi_table){.address = address, .length = length};
    memcpy(out->signature, bytes, 4);
    return true;
}

static bool parse(struct lume_acpi *out, lume_acpi_reader read, void *context,
                  const struct lume_acpi_table *table)
{
    unsigned char data[44];
    uint64_t address = table->address;
    uint32_t length = table->length;
    if (!strcmp(table->signature, "MCFG"))
    {
        if (length < 44 || (length - 44) % 16 || out->segment_count)
            return false;
        for (unsigned offset = 44; offset < length; offset += 16)
        {
            if (!read(context, address + offset, data, 16) ||
                out->segment_count == LUME_ACPI_SEGMENTS)
                return false;
            struct lume_acpi_segment s = {u64(data), (uint16_t)(data[8] | data[9] << 8), data[10],
                                          data[11]};
            uint64_t size = ((uint64_t)s.last + 1) << 20;
            if (!s.base || (s.base & 0xfffff) || s.first > s.last || u32(data + 12) ||
                s.base > UINT64_MAX - size)
                return false;
            for (unsigned i = 0; i < out->segment_count; ++i)
            {
                struct lume_acpi_segment *p = out->segments + i;
                if ((s.segment == p->segment && s.first <= p->last && p->first <= s.last) ||
                    (s.base + ((uint64_t)s.first << 20) <
                         p->base + (((uint64_t)p->last + 1) << 20) &&
                     p->base + ((uint64_t)p->first << 20) < s.base + size))
                    return false;
            }
            out->segments[out->segment_count++] = s;
        }
    }
    else if (!strcmp(table->signature, "APIC"))
    {
        if (length < 44 || !read(context, address + 36, data, 8))
            return false;
        out->lapic = u32(data);
        for (unsigned offset = 44; offset < length;)
        {
            if (length - offset < 2 || !read(context, address + offset, data, 2))
                return false;
            unsigned kind = data[0], bytes = data[1];
            if (bytes < 2 || bytes > length - offset)
                return false;
            if (kind == 0 || kind == 1 || kind == 5 || kind == 9)
            {
                unsigned minimum = kind == 0 ? 8 : kind == 9 ? 16 : 12;
                if (bytes < minimum || !read(context, address + offset, data, minimum))
                    return false;
                if ((kind == 0 && (u32(data + 4) & 1)) || (kind == 9 && (u32(data + 8) & 1)))
                {
                    uint32_t id = kind == 0 ? data[3] : u32(data + 4);
                    if (out->cpus == LUME_ACPI_CPUS)
                        return false;
                    for (unsigned i = 0; i < out->cpus; ++i)
                        if (out->apic_ids[i] == id)
                            return false;
                    out->apic_ids[out->cpus++] = id;
                }
                if (kind == 1)
                    ++out->ioapics;
                if (kind == 5)
                    out->lapic = u64(data + 4);
            }
            offset += bytes;
        }
    }
    else if (!strcmp(table->signature, "FACP") && length >= 111)
    {
        if (!read(context, address + 109, data, 2))
            return false;
        out->legacy_keyboard = (data[0] & 2) != 0;
    }
    return true;
}

bool lume_acpi_read(struct lume_acpi *out, lume_acpi_reader read, void *context, uint64_t rsdp)
{
    struct lume_acpi candidate = {0};
    unsigned char data[36];
    if (!out || !read || !read(context, rsdp, data, 20) || memcmp(data, "RSD PTR ", 8) ||
        !checksum(read, context, rsdp, 20))
        return false;
    candidate.rsdp = rsdp;
    candidate.revision = data[15];
    uint64_t root = u32(data + 16);
    unsigned width = 4;
    if (candidate.revision >= 2)
    {
        if (!read(context, rsdp, data, 36) || u32(data + 20) < 36 ||
            !checksum(read, context, rsdp, u32(data + 20)))
            return false;
        if (u64(data + 24))
        {
            root = u64(data + 24);
            width = 8;
        }
    }
    struct lume_acpi_table table;
    if (!header(read, context, root, &table) ||
        strcmp(table.signature, width == 8 ? "XSDT" : "RSDT") || (table.length - 36) % width ||
        (table.length - 36) / width >= LUME_ACPI_TABLES)
        return false;
    candidate.tables[candidate.table_count++] = table;
    unsigned entries = (table.length - 36) / width;
    for (unsigned i = 0; i < entries; ++i)
    {
        if (!read(context, root + 36 + (uint64_t)i * width, data, width))
            return false;
        uint64_t address = width == 8 ? u64(data) : u32(data);
        for (unsigned n = 0; n < candidate.table_count; ++n)
            if (candidate.tables[n].address == address)
                return false;
        if (!header(read, context, address, &table) || !parse(&candidate, read, context, &table))
            return false;
        candidate.tables[candidate.table_count++] = table;
    }
    *out = candidate;
    return true;
}

bool lume_acpi_find(struct lume_acpi *out, lume_acpi_reader read, void *context, uint64_t ebda)
{
    if (!out || !read)
        return false;
    const uint64_t starts[] = {ebda >= 0x80000 && ebda <= 0x9fc00 ? ebda : 0, 0xe0000};
    const unsigned sizes[] = {1024, 0x20000};
    for (unsigned n = 0; n < 2; ++n)
        if (starts[n])
            for (unsigned offset = 0; offset < sizes[n]; offset += 16)
                if (lume_acpi_read(out, read, context, starts[n] + offset))
                    return true;
    return false;
}
