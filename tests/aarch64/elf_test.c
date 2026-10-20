#include "lume/elf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks;
#define CHECK(x)                                                                                   \
    do                                                                                             \
    {                                                                                              \
        ++checks;                                                                                  \
        if (!(x))                                                                                  \
        {                                                                                          \
            fprintf(stderr, "ELF FAIL line %d: %s\n", __LINE__, #x);                               \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

static uint64_t read_le(const unsigned char *p, unsigned bytes)
{
    uint64_t n = 0;
    for (unsigned i = 0; i < bytes; ++i)
        n |= (uint64_t)p[i] << (8 * i);
    return n;
}

static void put(unsigned char *p, unsigned bytes, uint64_t n)
{
    for (unsigned i = 0; i < bytes; ++i)
        p[i] = (unsigned char)(n >> (8 * i));
}

int main(int argc, char **argv)
{
    CHECK(argc == 2 || argc == 3);
    unsigned expected_pages = argc == 3 ? (unsigned)strtoul(argv[2], NULL, 10) : 3;
    FILE *f = fopen(argv[1], "rb");
    CHECK(f != NULL);
    CHECK(fseek(f, 0, SEEK_END) == 0);
    long size = ftell(f);
    CHECK(size >= 64 && size < 0x100000);
    rewind(f);
    unsigned char *b = malloc((size_t)size), *m = malloc((size_t)size);
    CHECK(b && m);
    CHECK(fread(b, 1, (size_t)size, f) == (size_t)size);
    fclose(f);
    struct lume_elf_plan plan;
    CHECK(lume_elf_plan(b, (size_t)size, &plan));
    CHECK(plan.count == 3 && (expected_pages ? plan.pages == expected_pages : plan.pages >= 3) &&
          plan.entry == LUME_ELF_BASE);
    CHECK(plan.segments[2].memory_size > plan.segments[2].file_size);
    uint64_t required = 0;
    for (unsigned i = 0; i < plan.count; ++i)
        if (plan.segments[i].offset + plan.segments[i].file_size > required)
            required = plan.segments[i].offset + plan.segments[i].file_size;
    for (uint64_t n = 0; n < required; ++n)
        CHECK(!lume_elf_plan(b, (size_t)n, &plan));
    CHECK(lume_elf_plan(b, (size_t)required, &plan)); /* Section tables are unnecessary. */
    uint64_t ph = read_le(b + 32, 8);
#define BAD(offset, bytes, value)                                                                  \
    do                                                                                             \
    {                                                                                              \
        memcpy(m, b, (size_t)size);                                                                \
        put(m + (offset), (bytes), (value));                                                       \
        CHECK(!lume_elf_plan(m, (size_t)size, &plan));                                             \
    } while (0)
    BAD(0, 1, 0);
    BAD(4, 1, 1);
    BAD(5, 1, 2);
    BAD(6, 1, 2);
    BAD(8, 1, 1);
    BAD(16, 2, 3);
    BAD(18, 2, 243);
    BAD(20, 4, 0);
    BAD(32, 8, UINT64_MAX);
    BAD(48, 4, 1);
    BAD(52, 2, 63);
    BAD(54, 2, 55);
    BAD(56, 2, 0);
    BAD(56, 2, 17);
    BAD(24, 8, LUME_USER_DATA);
    BAD(24, 8, LUME_ELF_LIMIT);
    BAD(ph, 4, 2);
    BAD(ph, 4, 3);
    BAD(ph + 4, 4, 7);
    BAD(ph + 4, 4, 1);
    BAD(ph + 8, 8, UINT64_MAX);
    BAD(ph + 16, 8, LUME_ELF_BASE - 4096);
    BAD(ph + 16, 8, LUME_ELF_LIMIT);
    BAD(ph + 40, 8, 0);
    BAD(ph + 40, 8, UINT64_MAX);
    BAD(ph + 32, 8, read_le(b + ph + 40, 8) + 1);
    BAD(ph + 48, 8, 3);
    BAD(ph + 2 * 56 + 16, 8, LUME_ELF_BASE); /* Overlapping pages. */
    CHECK(lume_elf_plan(b, (size_t)size, &plan));
    free(m);
    free(b);
    printf("PASS_ELF64 checks=%u pages=%u\n", checks, plan.pages);
}
