#include "lume/early.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned checks, at;
static unsigned char blob[2048];
static const char names[] = "#address-cells\0#size-cells\0compatible\0reg\0interrupts\0";
#define T(x)                                                                                       \
    do                                                                                             \
    {                                                                                              \
        assert(x);                                                                                 \
        ++checks;                                                                                  \
    } while (0)

static void put(unsigned char *p, uint32_t v)
{
    p[0] = v >> 24;
    p[1] = v >> 16;
    p[2] = v >> 8;
    p[3] = v;
}

static void word(uint32_t v)
{
    put(blob + at, v);
    at += 4;
}

static void node(const char *name)
{
    word(1);
    unsigned bytes = (unsigned)strlen(name) + 1;
    memcpy(blob + at, name, bytes);
    at += (bytes + 3) & ~3u;
}

static void property(unsigned name, const void *data, unsigned bytes)
{
    word(3);
    word(bytes);
    word(name);
    memcpy(blob + at, data, bytes);
    at += (bytes + 3) & ~3u;
}

static unsigned make(unsigned order, uint64_t base, uint64_t size, unsigned spi, unsigned flags,
                     unsigned duplicates)
{
    memset(blob, 0, sizeof(blob));
    at = 56;
    node("");
    unsigned char two[4] = {0, 0, 0, 2};
    property(0, two, 4);
    property(15, two, 4);
    for (unsigned i = 0; i <= duplicates; ++i)
    {
        node("mmc@1000fff000");
        unsigned char reg[16], irq[12];
        put(reg, (uint32_t)(base >> 32));
        put(reg + 4, (uint32_t)base);
        put(reg + 8, (uint32_t)(size >> 32));
        put(reg + 12, (uint32_t)size);
        put(irq, 0);
        put(irq + 4, spi);
        put(irq + 8, flags);
        if (!order)
            property(27, "qemu,sdhci", 11);
        property(38, reg, sizeof(reg));
        property(42, irq, sizeof(irq));
        if (order)
            property(27, "qemu,sdhci", 11);
        word(2);
    }
    word(2);
    word(9);
    unsigned strings = at;
    memcpy(blob + at, names, sizeof(names));
    at += sizeof(names);
    put(blob, 0xd00dfeed);
    put(blob + 4, at);
    put(blob + 8, 56);
    put(blob + 12, strings);
    put(blob + 16, 40);
    put(blob + 20, 17);
    put(blob + 24, 16);
    put(blob + 32, sizeof(names));
    put(blob + 36, strings - 56);
    return at;
}

int main(void)
{
    struct lume_fdt fdt;
    for (unsigned order = 0; order < 2; ++order)
    {
        unsigned size = make(order, UINT64_C(0x1000fff000), 0x100, 273, 4, 0);
        T(lume_fdt_read(blob, size, &fdt) && fdt.sdhci_valid && fdt.sdhci_irq == 305);
        T(fdt.sdhci_base == UINT64_C(0x1000fff000) && fdt.sdhci_size == 0x100);
        for (unsigned n = 0; n < size; ++n)
            T(!lume_fdt_read(blob, n, &fdt));
    }
    unsigned size = make(0, UINT64_C(0x1000fff000), 0xff, 273, 4, 0);
    T(!lume_fdt_read(blob, size, &fdt));
    size = make(0, UINT64_C(0x1000fff000), 0x100, 988, 4, 0);
    T(!lume_fdt_read(blob, size, &fdt));
    size = make(0, UINT64_C(0x1000fff000), 0x100, 273, 1, 0);
    T(!lume_fdt_read(blob, size, &fdt));
    size = make(0, UINT64_C(0x1000fff000), 0x100, 273, 4, 1);
    T(!lume_fdt_read(blob, size, &fdt));
    size = make(0, UINT64_C(0x1001100000), 0x100, 273, 4, 0);
    T(lume_fdt_read(blob, size, &fdt) && !fdt.sdhci_valid);
    size = make(0, UINT64_MAX - 1, 0x100, 273, 4, 0);
    T(!lume_fdt_read(blob, size, &fdt));
    printf("FDT_DEVICE_PASS %u checks\n", checks);
    return 0;
}
