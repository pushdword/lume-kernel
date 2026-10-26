#include "lume/module_elf.h"
#include "lume/abi.h"

static uint32_t load32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void store(unsigned char *p, uint64_t value, unsigned bytes)
{
    for (unsigned i = 0; i < bytes; ++i)
        p[i] = (unsigned char)(value >> (i * 8));
}

static bool signed_range(uint64_t value, uint64_t origin, unsigned bits)
{
    uint64_t limit = UINT64_C(1) << (bits - 1);
    return value >= origin ? value - origin < limit : origin - value <= limit;
}

int lume_module_relocate_aarch64(unsigned type, void *target, size_t available, uint64_t place,
                                 uint64_t symbol, int64_t addend)
{
    if (!type)
        return 0;
    uint64_t magnitude = addend < 0 ? 0 - (uint64_t)addend : (uint64_t)addend;
    if ((addend < 0 && symbol < magnitude) || (addend >= 0 && symbol > UINT64_MAX - magnitude))
        return -LUME_ENOEXEC;
    uint64_t value = addend < 0 ? symbol - magnitude : symbol + magnitude;
    if (type == 257 || type == 260)
    {
        if (available < 8)
            return -LUME_ENOEXEC;
        store(target, type == 260 ? value - place : value, 8);
        return 0;
    }
    if (available < 4)
        return -LUME_ENOEXEC;
    if (type == 258 || type == 261)
    {
        if ((type == 258 && value > UINT32_MAX) || (type == 261 && !signed_range(value, place, 32)))
            return -LUME_ENOEXEC;
        store(target, type == 261 ? value - place : value, 4);
        return 0;
    }
    if (place & 3)
        return -LUME_ENOEXEC;
    uint32_t instruction = load32(target);
    if (type == 282 || type == 283)
    {
        uint32_t opcode = type == 282 ? UINT32_C(0x14000000) : UINT32_C(0x94000000);
        if ((value & 3) || !signed_range(value, place, 28) ||
            (instruction & UINT32_C(0xfc000000)) != opcode)
            return -LUME_ENOEXEC;
        instruction = opcode | ((uint32_t)((value - place) >> 2) & UINT32_C(0x03ffffff));
    }
    else if (type == 274 || type == 275)
    {
        uint64_t to = type == 275 ? value >> 12 : value;
        uint64_t from = type == 275 ? place >> 12 : place;
        uint32_t opcode = type == 275 ? UINT32_C(0x90000000) : UINT32_C(0x10000000);
        if (!signed_range(to, from, 21) || (instruction & UINT32_C(0x9f000000)) != opcode)
            return -LUME_ENOEXEC;
        uint32_t immediate = (uint32_t)(to - from) & UINT32_C(0x1fffff);
        instruction &= ~((UINT32_C(3) << 29) | (UINT32_C(0x7ffff) << 5));
        instruction |= (immediate & 3) << 29 | (immediate >> 2) << 5;
    }
    else if (type == 277)
    {
        if ((instruction & UINT32_C(0x7fc00000)) != UINT32_C(0x11000000))
            return -LUME_ENOEXEC;
        instruction = (instruction & ~(UINT32_C(0xfff) << 10)) | ((uint32_t)(value & 4095) << 10);
    }
    else if (type == 278 || type == 284 || type == 285 || type == 286 || type == 299)
    {
        unsigned scale = type == 278 ? 0 : type == 284 ? 1 : type == 285 ? 2 : type == 286 ? 3 : 4;
        if ((value & ((UINT64_C(1) << scale) - 1)) ||
            (instruction & UINT32_C(0x3b000000)) != UINT32_C(0x39000000))
            return -LUME_ENOEXEC;
        instruction =
            (instruction & ~(UINT32_C(0xfff) << 10)) | ((uint32_t)((value & 4095) >> scale) << 10);
    }
    else
        return -LUME_ENOEXEC;
    store(target, instruction, 4);
    return 0;
}
