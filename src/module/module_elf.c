#include "lume/module_elf.h"
#include "lume/abi.h"
#include <string.h>

#define SHF_WRITE UINT64_C(1)
#define SHF_ALLOC UINT64_C(2)
#define SHF_EXEC UINT64_C(4)
#define PAGE 4096u

static uint16_t le16(const unsigned char *p)
{
    return (uint16_t)p[0] | (uint16_t)p[1] << 8;
}

static uint32_t le32(const unsigned char *p)
{
    return (uint32_t)le16(p) | (uint32_t)le16(p + 2) << 16;
}

static uint64_t le64(const unsigned char *p)
{
    return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32;
}

static bool bounded(uint64_t offset, uint64_t bytes, size_t limit)
{
    return offset <= limit && bytes <= limit - offset;
}

static bool align_size(size_t value, uint64_t alignment, size_t *out)
{
    if (!alignment)
        alignment = 1;
    if (alignment > PAGE || (alignment & (alignment - 1)) || value > LUME_MODULE_FILE_LIMIT ||
        alignment - 1 > LUME_MODULE_FILE_LIMIT - value)
        return false;
    *out = (value + (size_t)alignment - 1) & ~((size_t)alignment - 1);
    return true;
}

static unsigned category(uint64_t flags)
{
    return flags & SHF_EXEC ? 0 : flags & SHF_WRITE ? 2 : 1;
}

static unsigned permissions(uint64_t flags)
{
    return LUME_MODULE_READ | (flags & SHF_WRITE ? LUME_MODULE_WRITE : 0) |
           (flags & SHF_EXEC ? LUME_MODULE_EXECUTE : 0);
}

struct elf_source
{
    const unsigned char *file, *sections, *symbols, *strings;
    size_t bytes, symbol_count, string_bytes;
    unsigned section_count;
};

static const unsigned char *section(const struct elf_source *source, unsigned index)
{
    return source->sections + (size_t)index * 64;
}

static const char *symbol_name(const struct elf_source *source, const unsigned char *symbol)
{
    uint32_t offset = le32(symbol);
    if (offset >= source->string_bytes ||
        !memchr(source->strings + offset, 0, source->string_bytes - offset))
        return NULL;
    return (const char *)source->strings + offset;
}

static int symbol_value(const struct elf_source *source, const struct lume_module_elf *image,
                        const struct lume_module_linker *linker, unsigned index, uint64_t *out)
{
    if (index >= source->symbol_count)
        return -LUME_ENOEXEC;
    const unsigned char *symbol = source->symbols + (size_t)index * 24;
    uint16_t target = le16(symbol + 6);
    if (!target)
    {
        const char *name = symbol_name(source, symbol);
        if (!name)
            return -LUME_ENOEXEC;
        if (!index)
        {
            *out = 0;
            return 0;
        }
        int result = linker->resolve(name, out);
        return result;
    }
    if (target == 0xfff1)
    {
        *out = le64(symbol + 8);
        return 0;
    }
    if (target >= source->section_count || !image->sections[target].address ||
        le64(symbol + 8) > image->sections[target].bytes ||
        le64(symbol + 16) > image->sections[target].bytes - le64(symbol + 8))
        return -LUME_ENOEXEC;
    *out = image->sections[target].address + le64(symbol + 8);
    return 0;
}

static int source_plan(const void *file, size_t bytes, const struct lume_module_linker *linker,
                       struct elf_source *source, size_t *offsets, size_t sizes[3])
{
    const unsigned char *header = file;
    if (!file || bytes < 64 || bytes > LUME_MODULE_FILE_LIMIT ||
        memcmp(header, "\177ELF\2\1\1\0", 8) || header[8] || le16(header + 16) != 1 ||
        le16(header + 18) != linker->machine || le32(header + 20) != 1 || le64(header + 24) ||
        le64(header + 32) || le32(header + 48) || le16(header + 52) != 64 || le16(header + 56) ||
        le16(header + 58) != 64)
        return -LUME_ENOEXEC;
    unsigned count = le16(header + 60);
    uint64_t table_offset = le64(header + 40);
    if (count < 2 || count > LUME_MODULE_SECTION_LIMIT || table_offset < 64 ||
        !bounded(table_offset, (uint64_t)count * 64, bytes) || le16(header + 62) >= count)
        return -LUME_ENOEXEC;
    *source = (struct elf_source){
        .file = file, .sections = header + table_offset, .bytes = bytes, .section_count = count};
    unsigned symbol_tables = 0;
    for (unsigned i = 0; i < count; ++i)
    {
        const unsigned char *s = section(source, i);
        uint32_t type = le32(s + 4);
        uint64_t flags = le64(s + 8), length = le64(s + 32), alignment = le64(s + 48);
        if (type != 8 && !bounded(le64(s + 24), length, bytes))
            return -LUME_ENOEXEC;
        if (!i)
        {
            for (unsigned j = 0; j < 64; ++j)
                if (s[j])
                    return -LUME_ENOEXEC;
            continue;
        }
        if (type == 2)
        {
            uint32_t strings = le32(s + 40);
            if (++symbol_tables != 1 || le64(s + 56) != 24 || length % 24 || length / 24 > 8192 ||
                !length || strings >= count)
                return -LUME_ENOEXEC;
            const unsigned char *str = section(source, strings);
            if (le32(str + 4) != 3 || !bounded(le64(str + 24), le64(str + 32), bytes) ||
                !le64(str + 32))
                return -LUME_ENOEXEC;
            source->symbols = header + le64(s + 24);
            source->symbol_count = (size_t)(length / 24);
            source->strings = header + le64(str + 24);
            source->string_bytes = (size_t)le64(str + 32);
        }
        if (!(flags & SHF_ALLOC))
            continue;
        if ((type != 1 && type != 8) || (flags & ~UINT64_C(0x37)) ||
            (flags & (SHF_WRITE | SHF_EXEC)) == (SHF_WRITE | SHF_EXEC) ||
            length > LUME_MODULE_FILE_LIMIT)
            return -LUME_ENOEXEC;
        unsigned group = category(flags);
        if (!align_size(sizes[group], alignment, offsets + i) ||
            length > LUME_MODULE_FILE_LIMIT - offsets[i])
            return -LUME_ENOEXEC;
        sizes[group] = offsets[i] + (size_t)length;
    }
    if (symbol_tables != 1 || source->strings[0] || source->strings[source->string_bytes - 1])
        return -LUME_ENOEXEC;
    for (unsigned i = 0; i < 24; ++i)
        if (source->symbols[i])
            return -LUME_ENOEXEC;
    return 0;
}

int lume_module_elf_load(const void *file, size_t bytes, const struct lume_module_linker *linker,
                         struct lume_module_elf *image)
{
    if (!image || !linker || !linker->resolve || !linker->relocate || !linker->synchronize)
        return -LUME_EINVAL;
    memset(image, 0, sizeof(*image));
    struct elf_source source;
    size_t offsets[LUME_MODULE_SECTION_LIMIT] = {0}, sizes[3] = {0}, starts[3] = {0};
    int result = source_plan(file, bytes, linker, &source, offsets, sizes);
    if (result)
        return result;
    for (unsigned group = 0; group < 3; ++group)
    {
        if (!align_size(sizes[group], PAGE, sizes + group))
            return -LUME_ENOEXEC;
        if (group)
            starts[group] = starts[group - 1] + sizes[group - 1];
        if (starts[group] > LUME_MODULE_FILE_LIMIT ||
            sizes[group] > LUME_MODULE_FILE_LIMIT - starts[group])
            return -LUME_ENOEXEC;
    }
    size_t total = starts[2] + sizes[2];
    if (!total)
        return -LUME_ENOEXEC;
    result = lume_module_memory_allocate(total, &image->memory);
    if (result)
        return result;
    image->section_count = source.section_count;
    for (unsigned i = 1; i < source.section_count; ++i)
    {
        const unsigned char *s = section(&source, i);
        uint64_t flags = le64(s + 8), length = le64(s + 32);
        if (!(flags & SHF_ALLOC))
            continue;
        uintptr_t address = (uintptr_t)image->memory.address + starts[category(flags)] + offsets[i];
        image->sections[i] =
            (struct lume_module_section){address, (size_t)length, permissions(flags)};
        if (le32(s + 4) != 8 && length)
            memcpy((void *)address, source.file + le64(s + 24), (size_t)length);
    }
    size_t relocation_count = 0;
    for (unsigned i = 1; i < source.section_count && !result; ++i)
    {
        const unsigned char *s = section(&source, i);
        uint32_t type = le32(s + 4), target = le32(s + 44);
        if (type != 4 && type != 9)
            continue;
        if (target >= source.section_count || le32(s + 40) >= source.section_count ||
            le32(section(&source, le32(s + 40)) + 4) != 2)
        {
            result = -LUME_ENOEXEC;
            break;
        }
        if (!image->sections[target].address)
            continue;
        uint64_t length = le64(s + 32);
        if (type != 4 || le64(s + 56) != 24 || length % 24 ||
            length / 24 > 16384 - relocation_count)
        {
            result = -LUME_ENOEXEC;
            break;
        }
        relocation_count += (size_t)(length / 24);
        for (uint64_t offset = 0; offset < length && !result; offset += 24)
        {
            const unsigned char *relocation = source.file + le64(s + 24) + offset;
            uint64_t where = le64(relocation), detail = le64(relocation + 8), symbol;
            if (where > image->sections[target].bytes)
            {
                result = -LUME_ENOEXEC;
                break;
            }
            result = symbol_value(&source, image, linker, (unsigned)(detail >> 32), &symbol);
            if (result)
                break;
            uintptr_t place = image->sections[target].address + (size_t)where;
            result = linker->relocate((unsigned)detail, (void *)place,
                                      image->sections[target].bytes - (size_t)where, place, symbol,
                                      (int64_t)le64(relocation + 16));
        }
    }
    for (unsigned i = 1; i < source.symbol_count && !result; ++i)
    {
        const unsigned char *symbol = source.symbols + (size_t)i * 24;
        const char *name = symbol_name(&source, symbol);
        if (!name)
        {
            result = -LUME_ENOEXEC;
            break;
        }
        if (!le16(symbol + 6))
        {
            uint64_t address;
            result = linker->resolve(name, &address);
            if (result)
                break;
        }
        if (strcmp(name, "lume_module_descriptor"))
            continue;
        uint64_t address;
        if (image->descriptor || (symbol[4] & 15) != 1 || (symbol[4] >> 4) != 1 ||
            !le16(symbol + 6))
        {
            result = -LUME_ENOEXEC;
            break;
        }
        result = symbol_value(&source, image, linker, i, &address);
        if (!result)
            image->descriptor = (uintptr_t)address;
    }
    if (!result && !image->descriptor)
        result = -LUME_ENOEXEC;
    if (!result)
        linker->synchronize((uintptr_t)image->memory.address, image->memory.bytes);
    for (unsigned group = 0; group < 3 && !result; ++group)
        if (sizes[group])
            result = lume_module_memory_protect(&image->memory, starts[group], sizes[group],
                                                LUME_MODULE_READ |
                                                    (group == 0 ? LUME_MODULE_EXECUTE : 0) |
                                                    (group == 2 ? LUME_MODULE_WRITE : 0));
    if (result)
    {
        (void)lume_module_elf_release(image);
        return result;
    }
    return 0;
}

int lume_module_elf_release(struct lume_module_elf *image)
{
    if (!image)
        return -LUME_EINVAL;
    if (image->memory.address)
    {
        int result = lume_module_memory_release(&image->memory);
        if (result)
            return result;
    }
    memset(image, 0, sizeof(*image));
    return 0;
}

bool lume_module_elf_contains(const struct lume_module_elf *image, uintptr_t address, size_t bytes,
                              unsigned flags)
{
    if (!image || !bytes || !address || bytes > UINTPTR_MAX - address)
        return false;
    for (unsigned i = 0; i < image->section_count; ++i)
    {
        const struct lume_module_section *s = image->sections + i;
        if (s->address && (s->flags & flags) == flags && address >= s->address &&
            bytes <= s->bytes && address - s->address <= s->bytes - bytes)
            return true;
    }
    return false;
}
