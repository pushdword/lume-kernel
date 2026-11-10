#include "../../../platform/raspberrypi5/fdt.c"
#include "../../../src/mm/pages.c"
#include <assert.h>
#include <stdio.h>
#include <string.h>

void lume_value(const char *name, uint64_t value)
{
    (void)name;
    (void)value;
}

void lume_worker_compute(void (*run)(void *), void *context)
{
    run(context);
}

void lume_worker_checkpoint(unsigned bytes)
{
    (void)bytes;
}

static void word(unsigned char *out, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i)
        out[i] = (unsigned char)(value >> (24 - i * 8));
}

static void argument(const char *text, unsigned expected)
{
    unsigned char blob[512] = {0};
    unsigned bytes = (unsigned)strlen(text) + 1;
    unsigned end = 88 + ((bytes + 3) & ~3u);
    word(blob, 0xd00dfeed);
    word(blob + 4, end + 21);
    word(blob + 8, 56);
    word(blob + 12, end + 12);
    word(blob + 16, 40);
    word(blob + 20, 17);
    word(blob + 24, 16);
    word(blob + 32, 9);
    word(blob + 36, end + 12 - 56);
    word(blob + 56, 1);
    word(blob + 64, 1);
    memcpy(blob + 68, "chosen", 7);
    word(blob + 76, 3);
    word(blob + 80, bytes);
    memcpy(blob + 88, text, bytes);
    word(blob + end, 2);
    word(blob + end + 4, 2);
    word(blob + end + 8, 9);
    memcpy(blob + end + 12, "bootargs", 9);
    struct lume_fdt fdt;
    assert(lume_fdt_read(blob, end + 21, &fdt));
    assert(fdt.memory_regions == expected);
    blob[88 + bytes - 1] = 'x';
    assert(!lume_fdt_read(blob, end + 21, &fdt));
}

int main(void)
{
    argument("", 0);
    argument("numa=fake=8 numa_policy=interleave", 8);
    argument("\tnuma=fake=16\tother=1", 16);
    argument("numa=fake=8 numa=fake=2", 2);
    argument("numa=fake=3", 0);
    argument("numa=fake=0", 0);
    argument("numa=fake=32", 0);
    argument("numa=fake=999999999999999999999999999", 0);
    argument("numa=fake=8x", 0);
    argument("numa=fake=8M", 0);
    argument("xnuma=fake=8", 0);
    struct lume_fdt fdt = {.pi5 = true, .memory_regions = 8, .ram_count = 8};
    for (unsigned i = 0; i < 8; ++i)
        fdt.ram[i] = (struct lume_range){(uint64_t)i * 0x40000000 + 0x20200000, 0x200000};
    fdt.reserved_count = 1;
    fdt.reserved[0] = (struct lume_range){0x60200000, 4096};
    lume_pages_start(&fdt, 0, 0, 0);
    assert(region_count == 8 && region_pages == 0x40000);
    uint64_t before = lume_pages_free(), pages[256];
    for (unsigned i = 0; i < 256; ++i)
    {
        pages[i] = lume_page_alloc_uninitialized(0);
        assert(pages[i] / 0x40000000 == i % 8);
        assert(pages[i] != 0x60200000);
        for (unsigned j = 0; j < i; ++j)
            assert(pages[i] != pages[j]);
    }
    assert(lume_pages_free() == before - 256);
    for (unsigned i = 0; i < 256; ++i)
        assert(lume_page_free(pages[i]));
    assert(lume_pages_free() == before);
    for (unsigned i = 0; i < 32; ++i)
        assert(lume_page_alloc_uninitialized(0x100000001) >= UINT64_C(0x100001000));
    uint64_t span = lume_pages_alloc_span_uninitialized(16);
    assert(span && !(span & 4095));
    for (unsigned i = 0; i < 16; ++i)
        assert(lume_page_free(span + i * 4096));
    fdt.memory_regions = 0;
    lume_pages_start(&fdt, 0, 0, 0);
    assert(!region_count);
    uint64_t previous = lume_page_alloc_uninitialized(0);
    assert(lume_page_alloc_uninitialized(0) == previous + 4096);
    puts("PASS_PAGE_REGIONS firmware parsing, bank rotation, reservations, spans, minimum");
}
