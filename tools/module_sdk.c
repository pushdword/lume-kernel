#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t little(const unsigned char *p, unsigned bytes)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < bytes; ++i)
        value |= (uint64_t)p[i] << (i * 8);
    return value;
}

static int generate(const unsigned char *file, size_t bytes, const char *path)
{
    if (bytes < 64 || memcmp(file, "\177ELF\2\1\1", 7))
        return 1;
    uint64_t table = little(file + 40, 8), count = little(file + 60, 2);
    if (little(file + 58, 2) != 64 || !count || table > bytes || count > (bytes - table) / 64)
        return 1;
    const unsigned char *identity = NULL;
    for (uint64_t i = 0; i < count; ++i)
    {
        const unsigned char *s = file + table + i * 64;
        if (little(s + 4, 4) != 7)
            continue;
        uint64_t offset = little(s + 24, 8), length = little(s + 32, 8);
        if (offset > bytes || length > bytes - offset || length != 36)
            continue;
        const unsigned char *note = file + offset;
        if (little(note, 4) == 4 && little(note + 4, 4) == 20 && little(note + 8, 4) == 3 &&
            !memcmp(note + 12, "GNU\0", 4))
        {
            if (identity)
                return 1;
            identity = note + 16;
        }
    }
    if (!identity)
        return 1;
    FILE *output = fopen(path, "wb");
    if (!output)
        return 1;
    int result = fprintf(output, "#ifndef LUME_MODULE_BUILD_H\n#define LUME_MODULE_BUILD_H\n"
                                 "#define LUME_KERNEL_BUILD_ID_BYTES {") < 0;
    for (unsigned i = 0; i < 20; ++i)
        result |= fprintf(output, "%s0x%02x", i ? ", " : "", identity[i]) < 0;
    result |= fprintf(output, "}\n#endif\n") < 0;
    result |= fclose(output) != 0;
    return result;
}

int main(int argc, char **argv)
{
    if (argc != 3)
    {
        fputs("usage: module-sdk kernel.elf module_build.h\n", stderr);
        return 1;
    }
    FILE *input = fopen(argv[1], "rb");
    if (!input || fseek(input, 0, SEEK_END))
        return 1;
    long length = ftell(input);
    if (length < 64 || length > 64 * 1024 * 1024 || fseek(input, 0, SEEK_SET))
    {
        fclose(input);
        return 1;
    }
    unsigned char *file = malloc((size_t)length);
    int result = 1;
    if (file && fread(file, 1, (size_t)length, input) == (size_t)length)
        result = generate(file, (size_t)length, argv[2]);
    fclose(input);
    free(file);
    if (result)
        fputs("Kernel build identity is unavailable\n", stderr);
    return result;
}
