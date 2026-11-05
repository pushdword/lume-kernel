#include "lume/early.h"
#include "lume/path.h"
#include "lume/abi.h"
#include <string.h>

#define PATH_PAGE_BYTES 4096u
#define PATH_PAGE_PREFIX 64u
#define PATH_CLASSES 5u

struct path_page
{
    struct path_page *next, *previous;
    uint64_t used;
    unsigned slot_bytes, slots;
};

static struct path_page *path_pages[PATH_CLASSES];
_Static_assert(sizeof(struct path_page) <= PATH_PAGE_PREFIX, "path page prefix");

static void unlink_page(struct path_page *page, unsigned index)
{
    if (page->previous)
        page->previous->next = page->next;
    else
        path_pages[index] = page->next;
    if (page->next)
        page->next->previous = page->previous;
    page->next = page->previous = NULL;
}

static void link_page(struct path_page *page, unsigned index)
{
    page->next = path_pages[index];
    page->previous = NULL;
    if (page->next)
        page->next->previous = page;
    path_pages[index] = page;
}

extern uint32_t ff_wtoupper(uint32_t);

static uint32_t next_character(const char **cursor)
{
    const unsigned char *text = (const void *)*cursor;
    unsigned first = *text++, count;
    uint32_t value, minimum;
    if (first < 0x80)
    {
        *cursor = (const char *)text;
        return first;
    }
    if (first >= 0xc2 && first <= 0xdf)
    {
        count = 1;
        value = first & 0x1f;
        minimum = 0x80;
    }
    else if (first >= 0xe0 && first <= 0xef)
    {
        count = 2;
        value = first & 0xf;
        minimum = 0x800;
    }
    else if (first >= 0xf0 && first <= 0xf4)
    {
        count = 3;
        value = first & 7;
        minimum = 0x10000;
    }
    else
        return UINT32_MAX;
    while (count--)
    {
        unsigned byte = *text++;
        if ((byte & 0xc0) != 0x80)
            return UINT32_MAX;
        value = (value << 6) | (byte & 0x3f);
    }
    if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff))
        return UINT32_MAX;
    *cursor = (const char *)text;
    return value;
}

bool lume_path_equal_folded(const char *first, const char *second)
{
    while (*first && *second)
    {
        uint32_t a = next_character(&first), b = next_character(&second);
        if (a == UINT32_MAX || b == UINT32_MAX || ff_wtoupper(a) != ff_wtoupper(b))
            return false;
    }
    return !*first && !*second;
}

uint32_t lume_path_hash_folded(const char *text)
{
    uint32_t hash = UINT32_C(2166136261);
    while (*text)
    {
        uint32_t value = next_character(&text);
        if (value == UINT32_MAX)
            return 0;
        value = ff_wtoupper(value);
        for (unsigned i = 0; i < 4; ++i)
        {
            hash = (hash ^ (value & 255u)) * UINT32_C(16777619);
            value >>= 8;
        }
    }
    return hash;
}

int lume_path_final_spelling(const char *input, char output[LUME_FILES_PATH_MAX])
{
    const char *leaf = strrchr(input, '/');
    leaf = leaf ? leaf + 1 : input;
    size_t bytes = strlen(leaf);
    if (!bytes || !strcmp(leaf, ".") || !strcmp(leaf, ".."))
        return 0;
    char *destination = strrchr(output, '/');
    if (!destination)
        return -LUME_EINVAL;
    size_t prefix = (size_t)(destination + 1 - output);
    if (bytes > LUME_FILES_NAME_MAX || bytes >= LUME_FILES_PATH_MAX - prefix)
        return -LUME_ENAMETOOLONG;
    memcpy(destination + 1, leaf, bytes + 1);
    return 0;
}

char *lume_path_duplicate(const char *text)
{
    unsigned bytes = 0;
    while (bytes < LUME_FILES_PATH_MAX && text[bytes])
        ++bytes;
    if (bytes == LUME_FILES_PATH_MAX)
        return NULL;
    ++bytes;
    unsigned slot_bytes = 64, index = 0;
    while (slot_bytes < bytes)
    {
        slot_bytes *= 2;
        ++index;
    }
    struct path_page *page = path_pages[index];
    if (!page)
    {
        uint64_t physical = lume_page_alloc(0);
        if (!physical)
            return NULL;
        page = (void *)(uintptr_t)physical;
        page->slot_bytes = slot_bytes;
        page->slots = (PATH_PAGE_BYTES - PATH_PAGE_PREFIX) / slot_bytes;
        link_page(page, index);
    }
    unsigned slot = 0;
    while (page->used & (UINT64_C(1) << slot))
        ++slot;
    page->used |= UINT64_C(1) << slot;
    if (page->used == (UINT64_MAX >> (64 - page->slots)))
        unlink_page(page, index);
    char *result = (char *)page + PATH_PAGE_PREFIX + (size_t)slot * slot_bytes;
    memcpy(result, text, bytes);
    return result;
}

void lume_path_release(char *text)
{
    if (!text)
        return;
    uintptr_t address = (uintptr_t)text;
    struct path_page *page = (void *)(address & ~(uintptr_t)(PATH_PAGE_BYTES - 1));
    unsigned index = 0, slot_bytes = 64;
    while (index < PATH_CLASSES && slot_bytes != page->slot_bytes)
    {
        slot_bytes *= 2;
        ++index;
    }
    if (index == PATH_CLASSES)
        lume_fail("path slot size");
    size_t offset = address - (uintptr_t)page;
    if (offset < PATH_PAGE_PREFIX || (offset - PATH_PAGE_PREFIX) % slot_bytes)
        lume_fail("path slot address");
    unsigned slot = (unsigned)((offset - PATH_PAGE_PREFIX) / slot_bytes);
    if (slot >= page->slots || !(page->used & (UINT64_C(1) << slot)))
        lume_fail("path slot release");
    bool full = page->used == (UINT64_MAX >> (64 - page->slots));
    memset(text, 0, slot_bytes);
    page->used &= ~(UINT64_C(1) << slot);
    if (full)
        link_page(page, index);
    if (!page->used)
    {
        unlink_page(page, index);
        if (!lume_page_free((uintptr_t)page))
            lume_fail("path page release");
    }
}
