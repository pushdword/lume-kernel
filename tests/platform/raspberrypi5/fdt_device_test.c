#include "lume/early.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks, at;
static unsigned char blob[4096];
static const char names[] = "#address-cells\0#size-cells\0compatible\0reg\0interrupts\0"
                            "ranges\0interrupt-parent\0phandle\0#interrupt-cells\0"
                            "interrupt-controller\0status\0interrupts-extended\0";
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

static void prop(const char *name, const void *data, unsigned bytes)
{
    unsigned offset = 0;
    while (strcmp(names + offset, name))
        offset += (unsigned)strlen(names + offset) + 1;
    property(offset, data, bytes);
}

static void value(const char *name, unsigned v)
{
    unsigned char data[4];
    put(data, v);
    prop(name, data, sizeof(data));
}

static void gic(unsigned nested, unsigned parent, unsigned bad)
{
    node("interrupt-controller@7fff9000");
    prop("compatible", "arm,gic-400", 12);
    value("phandle", parent);
    value("#interrupt-cells", bad == 1 ? 1 : 3);
    prop("interrupt-controller", "", 0);
    unsigned char reg[32];
    if (nested)
    {
        put(reg, 0x7fff9000);
        put(reg + 4, 0x1000);
        put(reg + 8, bad == 2 ? 0x7fffb000 : 0x7fffa000);
        put(reg + 12, 0x2000);
    }
    else
    {
        put(reg, 0x10);
        put(reg + 4, 0x7fff9000);
        put(reg + 8, 0);
        put(reg + 12, 0x1000);
        put(reg + 16, 0x10);
        put(reg + 20, bad == 2 ? 0x7fffb000 : 0x7fffa000);
        put(reg + 24, 0);
        put(reg + 28, 0x2000);
    }
    prop("reg", reg, nested ? 16 : 32);
    word(2);
}

static unsigned finish(void)
{
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

static unsigned make(unsigned order, uint64_t base, uint64_t size, unsigned spi, unsigned flags,
                     unsigned duplicates)
{
    memset(blob, 0, sizeof(blob));
    at = 56;
    node("");
    unsigned char two[4] = {0, 0, 0, 2};
    property(0, two, 4);
    property(15, two, 4);
    value("interrupt-parent", 7);
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
    gic(0, 7, 0);
    return finish();
}

static unsigned physical(unsigned variant)
{
    memset(blob, 0, sizeof(blob));
    at = 56;
    node("");
    value("#address-cells", 2);
    value("#size-cells", 2);
    value("interrupt-parent", variant == 1 || variant == 9 ? 9 : variant == 2 ? 0 : 7);
    node("soc@107c000000");
    prop("compatible", "simple-bus", 11);
    value("#address-cells", 1);
    value("#size-cells", 1);
    unsigned char mapping[32];
    put(mapping, 0);
    put(mapping + 4, 0x10);
    put(mapping + 8, 0);
    put(mapping + 12, variant == 4 ? 0xfff100 : 0x80000000);
    if (variant == 11)
    {
        put(mapping + 4, 0xffffffff);
        put(mapping + 8, 0xfffffff0);
    }
    memcpy(mapping + 16, mapping, 16);
    if (variant != 3)
        prop("ranges", mapping, variant == 5 ? 32 : variant == 6 ? 12 : 16);
    if (variant == 7)
        prop("status", "disabled", 9);
    if (variant == 8)
        value("interrupt-parent", 9);
    if (variant == 9)
        value("interrupt-parent", 7);
    if (variant == 10)
        value("#address-cells", 3);
    if (variant == 12)
        gic(1, 7, 0);
    node("mmc@fff000");
    unsigned char reg[16], irq[12];
    put(reg, 0xfff000);
    put(reg + 4, 0x260);
    put(reg + 8, 0xfff400);
    put(reg + 12, 0x200);
    put(irq, variant == 13 ? 1 : 0);
    put(irq + 4, 273);
    put(irq + 8, variant == 14 ? 1 : 4);
    prop("reg", reg, variant == 15 ? 12 : 16);
    prop("interrupts", irq, sizeof(irq));
    prop("compatible", "brcm,bcm2712-sdhci\0brcm,sdhci-brcmstb", 36);
    if (variant == 16)
        prop("status", "disabled", 9);
    if (variant == 17)
        prop("interrupts-extended", irq, sizeof(irq));
    word(2);
    if (variant != 12)
        gic(1, 7, variant == 18 ? 1 : variant == 19 ? 2 : 0);
    word(2);
    return finish();
}

static unsigned topology(unsigned count, unsigned width, unsigned variant)
{
    memset(blob, 0, sizeof(blob));
    at = 56;
    node("");
    node("cpus");
    value("#address-cells", width / 4);
    value("#size-cells", 0);
    for (unsigned i = 0; i < count; ++i)
    {
        char label[32];
        snprintf(label, sizeof(label), "cpu@%x", i * 256);
        node(label);
        unsigned char reg[8] = {0};
        put(reg + width - 4, variant == 2 ? 0 : i * 256);
        if (variant == 4)
            prop("status", "okay", 5);
        prop("reg", reg, variant == 3 ? 3 : width);
        if (variant == 1 && i == 1)
            prop("status", "disabled", 9);
        word(2);
    }
    word(2);
    return finish();
}

int main(int argc, char **argv)
{
    struct lume_fdt fdt;
    for (unsigned width = 4; width <= 8; width += 4)
    {
        for (unsigned count = 1; count <= 4; count *= 2)
        {
            unsigned size = topology(count, width, 0);
            T(lume_fdt_read(blob, size, &fdt) && fdt.cpu_count == count);
            for (unsigned i = 0; i < count; ++i)
                T(fdt.cpu_affinity[i] == i * 256);
        }
        unsigned size = topology(4, width, 1);
        T(lume_fdt_read(blob, size, &fdt) && fdt.cpu_count == 3);
        T(fdt.cpu_affinity[0] == 0 && fdt.cpu_affinity[1] == 512 && fdt.cpu_affinity[2] == 768);
        size = topology(4, width, 2);
        T(!lume_fdt_read(blob, size, &fdt));
        size = topology(4, width, 3);
        T(!lume_fdt_read(blob, size, &fdt));
        size = topology(4, width, 4);
        T(lume_fdt_read(blob, size, &fdt) && fdt.cpu_count == 4);
        size = topology(5, width, 0);
        T(!lume_fdt_read(blob, size, &fdt));
    }
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
    for (unsigned variant = 0; variant < 20; ++variant)
    {
        size = physical(variant);
        if (variant == 7 || variant == 16)
            T(lume_fdt_read(blob, size, &fdt) && !fdt.sdhci_valid);
        else if (variant == 0 || variant == 9 || variant == 12)
        {
            T(lume_fdt_read(blob, size, &fdt) && fdt.sdhci_valid);
            T(fdt.sdhci_base == UINT64_C(0x1000fff000) && fdt.sdhci_size == 0x260 &&
              fdt.sdhci_irq == 305);
        }
        else
            T(!lume_fdt_read(blob, size, &fdt));
    }
    if (argc == 2)
    {
        FILE *file = fopen(argv[1], "rb");
        T(file != NULL && fseek(file, 0, SEEK_END) == 0);
        long bytes = ftell(file);
        T(bytes >= 40 && bytes <= 0x200000 && fseek(file, 0, SEEK_SET) == 0);
        unsigned char *data = malloc((size_t)bytes);
        T(data != NULL && fread(data, 1, (size_t)bytes, file) == (size_t)bytes);
        T(fclose(file) == 0);
        T(lume_fdt_read(data, (size_t)bytes, &fdt) && fdt.sdhci_valid);
        T(fdt.sdhci_base == UINT64_C(0x1000fff000) && fdt.sdhci_size == 0x260 &&
          fdt.sdhci_irq == 305);
        T(fdt.cpu_count == 4);
        for (unsigned i = 0; i < 4; ++i)
            T(fdt.cpu_affinity[i] == i * 256);
        free(data);
    }
    printf("FDT_DEVICE_PASS %u checks\n", checks);
    return 0;
}
