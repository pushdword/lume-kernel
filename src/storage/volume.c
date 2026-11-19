#include "lume/storage.h"

static uint32_t u32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static bool typed_partition(const unsigned char *p, bool boot, unsigned type, uint32_t first,
                            uint32_t count)
{
    const unsigned char fields[8] = {boot ? 0x80 : 0,     0xfe, 0xff, 0xff,
                                     (unsigned char)type, 0xfe, 0xff, 0xff};
    for (unsigned i = 0; i < sizeof(fields); ++i)
        if (p[i] != fields[i])
            return false;
    return u32(p + 8) == first && u32(p + 12) == count;
}

static bool partition(const unsigned char *p, bool boot, uint32_t first, uint32_t count)
{
    return typed_partition(p, boot, 0x0c, first, count);
}

static bool empty(const unsigned char *p, unsigned length)
{
    for (unsigned i = 0; i < length; ++i)
        if (p[i])
            return false;
    return true;
}

static enum lume_volume_format data_partition(const unsigned char *p, uint32_t first,
                                              uint32_t count)
{
    if (partition(p, false, first, count))
        return LUME_VOLUME_FAT32;
    if (typed_partition(p, false, 0x83, first, count))
        return LUME_VOLUME_EXT4;
    return LUME_VOLUME_NONE;
}

static struct lume_volume journal_partition(const unsigned char *p, uint64_t sectors,
                                            uint64_t data_end)
{
    uint32_t first = u32(p + 8), count = u32(p + 12);
    if (first >= data_end && count && (uint64_t)first + count <= sectors &&
        typed_partition(p, false, 0x83, first, count))
        return (struct lume_volume){first, count};
    return (struct lume_volume){0};
}

#if defined(LUME_ARCH_AMD64) || defined(LUME_ARCH_I686)
static enum lume_volume_format pc_data(const unsigned char p[512], uint64_t sectors, uint32_t first,
                                       bool allow_boot_code, struct lume_volume *out)
{
    uint32_t count = u32(p + 458);
    if (sectors > UINT32_MAX || sectors <= first + 65536 || count < 65536 ||
        count > sectors - first || p[444] || p[445] || (!allow_boot_code && !empty(p, 440)) ||
        !empty(p + 478, 32))
        return LUME_VOLUME_NONE;
    enum lume_volume_format format = data_partition(p + 446, first, count);
    uint64_t end = (uint64_t)first + count;
    if (format == LUME_VOLUME_NONE ||
        (empty(p + 462, 16)
             ? end != sectors
             : format != LUME_VOLUME_EXT4 || !journal_partition(p + 462, sectors, end).count))
        return LUME_VOLUME_NONE;
    *out = (struct lume_volume){first, count};
    return format;
}
#endif

static bool secondary(const unsigned char p[512], uint64_t sectors, uint64_t data_end)
{
    uint32_t first = u32(p + 486), count = u32(p + 490);
    uint32_t root = u32(p + 502), root_count = u32(p + 506);
    return first >= data_end && !(first & 2047u) && count >= 65536 &&
           (uint64_t)first + count <= root && !(root & 2047u) && root_count >= 65536 &&
           (uint64_t)root + root_count <= sectors && partition(p + 478, false, first, count) &&
           typed_partition(p + 494, false, 0x83, root, root_count);
}

static enum lume_volume_format dedicated(const unsigned char p[512], uint64_t sectors,
                                         struct lume_volume *out)
{
    if (sectors > UINT32_MAX || sectors <= LUME_CARD_DATA_FIRST + 65536 || p[444] || p[445])
        return LUME_VOLUME_NONE;
    for (unsigned i = 0; i < 440; ++i)
        if (p[i])
            return LUME_VOLUME_NONE;
    uint32_t data_count = u32(p + 474);
    enum lume_volume_format format = data_partition(p + 462, LUME_CARD_DATA_FIRST, data_count);
    if (data_count < 65536 || data_count > sectors - LUME_CARD_DATA_FIRST ||
        !partition(p + 446, true, LUME_CARD_BOOT_FIRST, LUME_CARD_BOOT_SECTORS) ||
        format == LUME_VOLUME_NONE)
        return LUME_VOLUME_NONE;
    uint64_t end = LUME_CARD_DATA_FIRST + data_count;
    bool has_journal = format == LUME_VOLUME_EXT4 && empty(p + 494, 16) &&
                       journal_partition(p + 478, sectors, end).count;
    if (!empty(p + 478, 32) && !has_journal && !secondary(p, sectors, end))
        return LUME_VOLUME_NONE;
    *out = (struct lume_volume){LUME_CARD_DATA_FIRST, data_count};
    return format;
}

enum lume_volume_format lume_volume_identify(const unsigned char p[512], uint64_t sectors,
                                             struct lume_volume *out)
{
    *out = (struct lume_volume){0};
    if (p[510] != 0x55 || p[511] != 0xaa)
        return LUME_VOLUME_NONE;
#if defined(LUME_ARCH_AMD64)
    if (u32(p + 440) == LUME_Q35_DISK_ID)
        return pc_data(p, sectors, 2048, false, out);
#endif
#if defined(LUME_ARCH_I686)
    if (u32(p + 440) == LUME_PC_DISK_ID)
        return pc_data(p, sectors, (uint32_t)LUME_PC_DATA_FIRST, true, out);
#endif
    if (u32(p + 440) == LUME_CARD_DISK_ID)
        return dedicated(p, sectors, out);
    if (u32(p + 440) != 0xb8d65c68 || sectors != 121503744)
        return LUME_VOLUME_NONE;
    if (p[450] != 0x0c || u32(p + 454) != 16384 || u32(p + 458) != 1048576 || p[466] != 0x83 ||
        u32(p + 470) != 1064960 || u32(p + 474) != 86884352 || p[478] != 0 || p[482] != 0x0c ||
        u32(p + 486) != 87949312 || u32(p + 490) != 33554432)
        return LUME_VOLUME_NONE;
    for (unsigned i = 494; i < 510; ++i)
        if (p[i])
            return LUME_VOLUME_NONE;
    *out = (struct lume_volume){87949312, 33554432};
    return LUME_VOLUME_FAT32;
}

bool lume_volume_select(const unsigned char p[512], uint64_t sectors, struct lume_volume *out)
{
    return lume_volume_identify(p, sectors, out) != LUME_VOLUME_NONE;
}

bool lume_volume_boot_select(const unsigned char p[512], uint64_t sectors, struct lume_volume *out)
{
    struct lume_volume data;
    *out = (struct lume_volume){0};
    if (p[510] != 0x55 || p[511] != 0xaa || u32(p + 440) != LUME_CARD_DISK_ID ||
        !dedicated(p, sectors, &data))
        return false;
    *out = (struct lume_volume){LUME_CARD_BOOT_FIRST, LUME_CARD_BOOT_SECTORS};
    return true;
}

bool lume_volume_alternate_boot_select(const unsigned char p[512], uint64_t sectors,
                                       struct lume_volume *out)
{
    struct lume_volume data;
    *out = (struct lume_volume){0};
    if (!lume_volume_boot_select(p, sectors, &data) ||
        !secondary(p, sectors, LUME_CARD_DATA_FIRST + u32(p + 474)))
        return false;
    *out = (struct lume_volume){u32(p + 486), u32(p + 490)};
    return true;
}

bool lume_volume_journal_select(const unsigned char p[512], uint64_t sectors,
                                struct lume_volume *out)
{
    struct lume_volume data;
    *out = (struct lume_volume){0};
    if (lume_volume_identify(p, sectors, &data) != LUME_VOLUME_EXT4)
        return false;
    const unsigned char *entry = p + 462;
    if (u32(p + 440) == LUME_CARD_DISK_ID)
    {
        if (!empty(p + 494, 16))
            return false;
        entry = p + 478;
    }
    *out = journal_partition(entry, sectors, data.first + data.count);
    return out->count != 0;
}
