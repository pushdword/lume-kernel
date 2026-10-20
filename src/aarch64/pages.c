#include "lume/early.h"
#define MAX_PAGES (UINT64_C(0x200000000) / 4096)
static uint64_t used[MAX_PAGES/64], managed[MAX_PAGES/64], free_count;
static uint64_t cursor;

static bool overlaps(uint64_t a, uint64_t size, uint64_t b, uint64_t bytes)
{
    return a < b + bytes && b < a + size;
}

void lume_pages_start(const struct lume_fdt *fdt, uint64_t dtb, uint64_t image_start, uint64_t image_limit)
{
    /* The image's split 2 MiB window has unmapped slack and a stack guard.
     * Reserve the whole window, rather than handing those holes to alloc(). */
    uint64_t image_end = (image_limit + 0x1fffff) & ~UINT64_C(0x1fffff);
    free_count = 0; cursor = 0;
    for (unsigned i = 0; i < MAX_PAGES/64; ++i) { used[i] = UINT64_MAX; managed[i] = 0; }
    for (unsigned r = 0; r < fdt->ram_count; ++r) {
        if (fdt->ram[r].base >= 0x200000000) continue;
        uint64_t start = (fdt->ram[r].base + 0x1fffff) & ~UINT64_C(0x1fffff);
        uint64_t end = (fdt->ram[r].base + fdt->ram[r].size) & ~UINT64_C(0x1fffff);
        if (start < 0x200000) start = 0x200000;
        if (end > 0x200000000) end = 0x200000000;
        for (uint64_t p = start; p < end; p += 4096) {
            bool reserve = overlaps(p,4096,image_start,image_end-image_start) ||
                           overlaps(p,4096,dtb,fdt->size) ||
                           (fdt->initrd_end>fdt->initrd_start && overlaps(p,4096,fdt->initrd_start,fdt->initrd_end-fdt->initrd_start));
            for (unsigned n = 0; n < fdt->reserved_count; ++n)
                if (overlaps(p,4096,fdt->reserved[n].base,fdt->reserved[n].size)) reserve = true;
            uint64_t index = p/4096, mask = UINT64_C(1) << (index & 63);
            if (!reserve && !(managed[index/64] & mask)) {
                managed[index/64] |= mask; used[index/64] &= ~mask; ++free_count;
            }
        }
    }
}

uint64_t lume_page_alloc(uint64_t minimum)
{
    if (!free_count || minimum >= 0x200000000) return 0;
    uint64_t first = (minimum + 4095)/4096;
    if (!first) first = 1;
    uint64_t count = MAX_PAGES - first;
    uint64_t start = cursor >= first && cursor < MAX_PAGES ? cursor : first;
    for (uint64_t n = 0, p = start; n < count; ++n) {
        uint64_t mask = UINT64_C(1) << (p & 63);
        if (!(used[p/64] & mask)) {
            used[p/64] |= mask; --free_count; cursor = p + 1;
            volatile uint64_t *data = (void *)(uintptr_t)(p*4096);
            for (unsigned i = 0; i < 512; ++i) data[i] = 0;
            return p*4096;
        }
        if (++p == MAX_PAGES) p = first;
    }
    return 0;
}

bool lume_page_free(uint64_t page)
{
    if (!page || page >= 0x200000000 || (page & 4095)) return false;
    uint64_t p = page/4096, mask = UINT64_C(1) << (p & 63);
    if (!(managed[p/64] & mask) || !(used[p/64] & mask)) return false;
    used[p/64] &= ~mask; ++free_count;
    return true;
}
uint64_t lume_pages_free(void) { return free_count; }

bool lume_pages_check(void)
{
    uint64_t before = free_count, pages[129];
    if (before < 129) return false;
    for (unsigned n = 0; n < 129; ++n) {
        pages[n] = lume_page_alloc(n == 128 ? UINT64_C(0x100000000) : 0);
        if (!pages[n]) return false;
        volatile uint64_t *p = (void *)(uintptr_t)pages[n];
        if (p[0] || p[511]) return false;
        p[0] = pages[n] ^ UINT64_C(0x4c554d45);
        p[511] = ~p[0];
    }
    lume_value("PAGE_TEST_ABOVE_4G", pages[128]);
    for (unsigned n = 0; n < 129; ++n) {
        volatile uint64_t *p = (void *)(uintptr_t)pages[n];
        if (p[0] != (pages[n] ^ UINT64_C(0x4c554d45)) || p[511] != ~p[0]) return false;
    }
    for (unsigned n = 129; n; --n)
        if (!lume_page_free(pages[n-1])) return false;
    return free_count == before && !lume_page_free(pages[0]) && !lume_page_free(0x200000);
}
