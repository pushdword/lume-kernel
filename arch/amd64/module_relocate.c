#include "lume/module_arch.h"
#include "lume/abi.h"
#include <string.h>

int lume_module_relocate_amd64(unsigned type, void *target, size_t available, uint64_t place,
                               uint64_t symbol, int64_t addend)
{
    uint64_t value = symbol + (uint64_t)addend;
    if (type == 0)
        return 0;
    if (type == 1)
    {
        if (available < 8)
            return -LUME_ENOEXEC;
        memcpy(target, &value, 8);
        return 0;
    }
    if (type == 2 || type == 4)
        value -= place;
    if (available < 4 || (type != 2 && type != 4 && type != 10 && type != 11))
        return -LUME_ENOEXEC;
    if (type == 10 ? value > UINT32_MAX : (int64_t)value != (int64_t)(int32_t)value)
        return -LUME_ENOEXEC;
    uint32_t word = (uint32_t)value;
    memcpy(target, &word, 4);
    return 0;
}
