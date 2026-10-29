#include "lume/storage.h"

static uint32_t u32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static bool partition(const unsigned char *p, bool boot, uint32_t first, uint32_t count)
{
    const unsigned char fields[8] = {boot ? 0x80 : 0, 0xfe, 0xff, 0xff, 0x0c, 0xfe, 0xff, 0xff};
    for (unsigned i = 0; i < sizeof(fields); ++i)
        if (p[i] != fields[i])
            return false;
    return u32(p + 8) == first && u32(p + 12) == count;
}

static bool dedicated(const unsigned char p[512], uint64_t sectors, struct lume_volume *out)
{
    if (sectors > UINT32_MAX || sectors <= LUME_CARD_DATA_FIRST + 65536 || p[444] || p[445])
        return false;
    for (unsigned i = 0; i < 440; ++i)
        if (p[i])
            return false;
    if (!partition(p + 446, true, LUME_CARD_BOOT_FIRST, LUME_CARD_BOOT_SECTORS) ||
        !partition(p + 462, false, LUME_CARD_DATA_FIRST, sectors - LUME_CARD_DATA_FIRST))
        return false;
    for (unsigned i = 478; i < 510; ++i)
        if (p[i])
            return false;
    *out = (struct lume_volume){LUME_CARD_DATA_FIRST, sectors - LUME_CARD_DATA_FIRST};
    return true;
}

bool lume_volume_select(const unsigned char p[512], uint64_t sectors, struct lume_volume *out)
{
    *out = (struct lume_volume){0};
    if (p[510] != 0x55 || p[511] != 0xaa)
        return false;
#if defined(LUME_ARCH_AMD64)
    if (u32(p + 440) == LUME_Q35_DISK_ID)
    {
        if (sectors > UINT32_MAX || sectors <= 2048 + 65536 || p[444] || p[445] ||
            !partition(p + 446, false, 2048, (uint32_t)(sectors - 2048)))
            return false;
        for (unsigned i = 0; i < 440; ++i)
            if (p[i])
                return false;
        for (unsigned i = 462; i < 510; ++i)
            if (p[i])
                return false;
        *out = (struct lume_volume){2048, sectors - 2048};
        return true;
    }
#endif
#if defined(LUME_ARCH_I686)
    if (u32(p + 440) == LUME_PC_DISK_ID)
    {
        if (sectors > UINT32_MAX || sectors <= LUME_PC_DATA_FIRST + 65536 || p[444] || p[445] ||
            !partition(p + 446, false, LUME_PC_DATA_FIRST,
                       (uint32_t)(sectors - LUME_PC_DATA_FIRST)))
            return false;
        for (unsigned i = 462; i < 510; ++i)
            if (p[i])
                return false;
        *out = (struct lume_volume){LUME_PC_DATA_FIRST, sectors - LUME_PC_DATA_FIRST};
        return true;
    }
#endif
    if (u32(p + 440) == LUME_CARD_DISK_ID)
        return dedicated(p, sectors, out);
    if (u32(p + 440) != 0xb8d65c68 || sectors != 121503744)
        return false;
    if (p[450] != 0x0c || u32(p + 454) != 16384 || u32(p + 458) != 1048576 || p[466] != 0x83 ||
        u32(p + 470) != 1064960 || u32(p + 474) != 86884352 || p[478] != 0 || p[482] != 0x0c ||
        u32(p + 486) != 87949312 || u32(p + 490) != 33554432)
        return false;
    for (unsigned i = 494; i < 510; ++i)
        if (p[i])
            return false;
    *out = (struct lume_volume){87949312, 33554432};
    return true;
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
