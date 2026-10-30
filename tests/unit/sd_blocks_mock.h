#ifndef LUME_SD_BLOCKS_MOCK_H
#define LUME_SD_BLOCKS_MOCK_H
#include "lume/sd.h"
#include <stddef.h>

bool lume_sd_read_blocks(uint64_t sector, unsigned count, void *data)
{
    if (!data || !count || count > LUME_SD_MAX_SECTORS || sector >= lume_sd_sectors() ||
        count > lume_sd_sectors() - sector)
        return false;
    for (unsigned i = 0; i < count; ++i)
        if (!lume_sd_read(sector + i, (unsigned char *)data + (size_t)i * 512))
            return false;
    return true;
}

bool lume_sd_write_blocks(uint64_t sector, unsigned count, const void *data)
{
    if (!data || !count || count > LUME_SD_MAX_SECTORS || sector >= lume_sd_sectors() ||
        count > lume_sd_sectors() - sector)
        return false;
    for (unsigned i = 0; i < count; ++i)
        if (!lume_sd_write(sector + i, (const unsigned char *)data + (size_t)i * 512))
            return false;
    return true;
}
#endif
