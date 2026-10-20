#define _GNU_SOURCE
#include "lume/early.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
static unsigned checks;
#define CHECK(x)                                                                                   \
    do                                                                                             \
    {                                                                                              \
        ++checks;                                                                                  \
        if (!(x))                                                                                  \
        {                                                                                          \
            fprintf(stderr, "PAGES FAIL line %d: %s\n", __LINE__, #x);                             \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

void lume_value(const char *name, uint64_t value)
{
    (void)name;
    (void)value;
}

int main(void)
{
    const uint64_t low = 0x400000, high = UINT64_C(0x100000000), bytes = 0x1000000;
    CHECK(mmap((void *)(uintptr_t)low, bytes, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) == (void *)(uintptr_t)low);
    CHECK(mmap((void *)(uintptr_t)high, bytes, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1,
               0) == (void *)(uintptr_t)high);
    struct lume_fdt fdt = {.size = 4096,
                           .ram_count = 2,
                           .ram = {{0x80000, 0xf80000}, {0x100000000, 0x1000000}},
                           .reserved_count = 2,
                           .reserved = {{0, 0x80000}, {0x600000, 8192}}};
    lume_pages_start(&fdt, 0x800000, 0x200000, 0x2ac000);
    uint64_t before = lume_pages_free();
    CHECK(before == (0xc00000 + 0x1000000) / 4096 - 3);
    CHECK(lume_pages_check());
    CHECK(lume_pages_free() == before);
    CHECK(!lume_page_free(0x2ac000)); /* Unmapped slack in the kernel image window. */
    CHECK(!lume_page_free(0x600000));
    CHECK(!lume_page_free(0x800000));
    CHECK(!lume_page_alloc(UINT64_MAX));
    uint64_t *pages = calloc((size_t)before, sizeof(*pages));
    CHECK(pages != NULL);
    for (uint64_t n = 0; n < before; ++n)
    {
        uint64_t p = pages[n] = lume_page_alloc(0);
        CHECK(p && !(p & 4095));
        CHECK((p >= low && p < 0x1000000) || (p >= high && p < high + bytes));
        CHECK(p != 0x600000 && p != 0x601000 && p != 0x800000);
        volatile uint64_t *data = (void *)(uintptr_t)p;
        CHECK(data[0] == 0 && data[511] == 0);
        data[0] = p;
        data[511] = ~p;
    }
    CHECK(lume_pages_free() == 0 && lume_page_alloc(0) == 0);
    for (uint64_t n = 0; n < before; ++n)
    {
        volatile uint64_t *data = (void *)(uintptr_t)pages[n];
        CHECK(data[0] == pages[n] && data[511] == ~pages[n]);
        CHECK(lume_page_free(pages[n]));
        CHECK(!lume_page_free(pages[n]));
    }
    CHECK(lume_pages_free() == before);
    free(pages);
    fdt.initrd_start = 0xa00000;
    fdt.initrd_end = 0xa03001;
    lume_pages_start(&fdt, 0x800000, 0x200000, 0x2ac000);
    CHECK(lume_pages_free() == before - 4);
    CHECK(!lume_page_free(0xa00000) && !lume_page_free(0xa03000));
    CHECK(munmap((void *)(uintptr_t)low, bytes) == 0);
    CHECK(munmap((void *)(uintptr_t)high, bytes) == 0);
    printf("PASS_PAGES checks=%u frames=%llu\n", checks, (unsigned long long)before);
}
