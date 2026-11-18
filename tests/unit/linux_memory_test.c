/* SPDX-License-Identifier: GPL-2.0-only */
#include <linux/slab.h>
#include <linux/err.h>
#include <linux/string.h>
#include <lume/linux_memory.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static unsigned checks, allocations, releases;
static bool fail;
static const unsigned char *watched;
static size_t watched_bytes;
static unsigned char expected;

#define T(expression)                                                                              \
    do                                                                                             \
    {                                                                                              \
        assert(expression);                                                                        \
        ++checks;                                                                                  \
    } while (0)

static void *allocate(size_t bytes)
{
    ++allocations;
    return fail ? NULL : malloc(bytes);
}

static void release(void *memory)
{
    for (size_t i = 0; i < watched_bytes; ++i)
        T(watched[i] == expected);
    watched = NULL;
    watched_bytes = 0;
    ++releases;
    free(memory);
}

int main(void)
{
    T(!kmalloc(32, GFP_KERNEL));
    T(lume_linux_memory_start(NULL, release) == -EINVAL);
    T(lume_linux_memory_start(allocate, NULL) == -EINVAL);
    T(lume_linux_memory_start(allocate, release) == 0);
    T(lume_linux_memory_start(allocate, release) == -EBUSY);
    T(!kmalloc(SIZE_MAX, GFP_KERNEL));
    T(!kmalloc_array(SIZE_MAX / 2 + 1, 2, GFP_KERNEL));
    T(!kcalloc(SIZE_MAX / 2 + 1, 2, GFP_KERNEL));
    T(!kmalloc(32, GFP_ATOMIC));
    T(allocations == 0);
    T(kmalloc(0, GFP_KERNEL) == ZERO_SIZE_PTR);
    T(kcalloc(0, SIZE_MAX, GFP_KERNEL) == ZERO_SIZE_PTR);
    T(kmalloc_array(SIZE_MAX, 0, GFP_KERNEL) == ZERO_SIZE_PTR);
    T(kmemdup(NULL, 0, GFP_KERNEL) == ZERO_SIZE_PTR);
    T(!kmemdup(NULL, 4, GFP_KERNEL));
    T(ksize(NULL) == 0 && ksize(ZERO_SIZE_PTR) == 0);
    kfree(NULL);
    kfree(ZERO_SIZE_PTR);
    kfree_sensitive(NULL);
    kfree_sensitive(ZERO_SIZE_PTR);
    memzero_explicit(NULL, 0);
    T(releases == 0 && lume_linux_memory_allocations() == 0);

    unsigned char *memory = kcalloc(17, 8, GFP_KERNEL);
    T(memory && !((uintptr_t)memory & 63));
    T(ksize(memory) == 136);
    for (unsigned i = 0; i < 136; ++i)
        T(memory[i] == 0);
    T(lume_linux_memory_allocations() == 1);
    T(lume_linux_memory_stop() == -EBUSY);
    memset(memory, 0xa5, 136);
    T(krealloc(memory, 64, GFP_KERNEL) == memory);
    T(!krealloc(memory, 512, GFP_ATOMIC));
    fail = true;
    T(!krealloc(memory, 512, GFP_KERNEL));
    T(!krealloc(memory, SIZE_MAX, GFP_KERNEL));
    T(lume_linux_memory_allocations() == 1 && ksize(memory) >= 136);
    for (unsigned i = 0; i < 136; ++i)
        T(memory[i] == 0xa5);
    fail = false;
    watched = memory;
    watched_bytes = 136;
    expected = 0xa5;
    unsigned char *grown = krealloc(memory, 512, GFP_KERNEL);
    T(grown && grown != memory && ksize(grown) >= 512);
    for (unsigned i = 0; i < 136; ++i)
        T(grown[i] == 0xa5);
    T(lume_linux_memory_allocations() == 1);

    unsigned char *duplicate = kmemdup(grown, 136, GFP_KERNEL);
    T(duplicate && !memcmp(duplicate, grown, 136));
    T(lume_linux_memory_allocations() == 2);
    watched = duplicate;
    watched_bytes = 136;
    expected = 0;
    kfree_sensitive(duplicate);
    T(lume_linux_memory_allocations() == 1);
    memset(grown, 0x5a, 512);
    watched = grown;
    watched_bytes = 512;
    expected = 0x5a;
    T(krealloc(grown, 0, GFP_KERNEL) == ZERO_SIZE_PTR);
    T(lume_linux_memory_allocations() == 0);
    memory = krealloc(ZERO_SIZE_PTR, 32, GFP_KERNEL);
    T(memory && ksize(memory) >= 32);
    kfree(memory);
    memory = krealloc(NULL, 32, GFP_KERNEL);
    T(memory && ksize(memory) >= 32);
    kfree(memory);
    T(lume_linux_memory_stop() == 0);
    T(!kmalloc(32, GFP_KERNEL));
    T(lume_linux_memory_start(allocate, release) == 0);
    T(lume_linux_memory_stop() == 0);
    printf("Linux allocation: %u checks passed\n", checks);
    return 0;
}
