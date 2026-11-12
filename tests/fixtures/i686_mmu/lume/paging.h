#ifndef LUME_I686_PAGING_H
#define LUME_I686_PAGING_H
#include <stdint.h>
static uint32_t active_root;
static unsigned invalidations;

static inline uint32_t lume_i686_paging_root(void)
{
    return active_root;
}

static inline void lume_i686_paging_switch(uint32_t physical)
{
    active_root = physical;
}

static inline void lume_i686_paging_invalidate(uintptr_t address)
{
    (void)address;
    ++invalidations;
}

static inline void lume_i686_paging_enable(uint32_t physical)
{
    lume_i686_paging_switch(physical);
}
#endif
