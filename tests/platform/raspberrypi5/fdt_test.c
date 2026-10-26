#include "lume/early.h"
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
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x);                                   \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

static unsigned read_be(const unsigned char *p)
{
    return (unsigned)p[0] << 24 | (unsigned)p[1] << 16 | (unsigned)p[2] << 8 | p[3];
}

static void write_be(unsigned char *p, unsigned v)
{
    p[0] = v >> 24;
    p[1] = v >> 16;
    p[2] = v >> 8;
    p[3] = v;
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    FILE *stream = fopen(argv[1], "rb");
    CHECK(stream != NULL);
    CHECK(fseek(stream, 0, SEEK_END) == 0);
    long size = ftell(stream);
    CHECK(size >= 40 && size <= 0x200000);
    rewind(stream);
    unsigned char *valid = malloc((size_t)size), *mutant = malloc((size_t)size);
    CHECK(valid != NULL && mutant != NULL);
    CHECK(fread(valid, 1, (size_t)size, stream) == (size_t)size);
    fclose(stream);
    struct lume_fdt fdt;
    CHECK(lume_fdt_read(valid, (size_t)size, &fdt));
    CHECK(fdt.pi5 && fdt.psci_smc && fdt.model[0]);
    const unsigned char mac[6] = {0x98, 0xfe, 0x54, 0x49, 0xe4, 0x47};
    CHECK(fdt.wifi_mac_valid && !memcmp(fdt.wifi_mac, mac, 6));
    CHECK(fdt.ram_count && fdt.reserved_count && fdt.gic400 && fdt.physical_timer);
    /* Every truncated prefix, including partial header/token/string fields. */
    for (long n = 0; n < size; ++n)
        CHECK(!lume_fdt_read(valid, (size_t)n, &fdt));
    static const unsigned offsets[] = {0, 4, 8, 12, 20, 24, 32, 36};
    for (unsigned i = 0; i < sizeof(offsets) / sizeof(offsets[0]); ++i)
    {
        memcpy(mutant, valid, (size_t)size);
        write_be(mutant + offsets[i], offsets[i] == 20 ? 16 : 0xffffffff);
        CHECK(!lume_fdt_read(mutant, (size_t)size, &fdt));
    }
    unsigned structure = read_be(valid + 8);
    memcpy(mutant, valid, (size_t)size);
    write_be(mutant + structure, 2); /* Unmatched END_NODE */
    CHECK(!lume_fdt_read(mutant, (size_t)size, &fdt));
    write_be(mutant + structure, 9); /* END without a root */
    CHECK(!lume_fdt_read(mutant, (size_t)size, &fdt));
    write_be(mutant + structure, 0xff); /* Unknown token */
    CHECK(!lume_fdt_read(mutant, (size_t)size, &fdt));
    CHECK(lume_fdt_read(valid, (size_t)size, &fdt));
    printf("PASS_FDT checks=%u model=%s\n", checks, fdt.model);
    free(mutant);
    free(valid);
    return 0;
}
