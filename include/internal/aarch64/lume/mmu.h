#ifndef LUME_MMU_H
#define LUME_MMU_H
#include "early.h"

struct lume_layout
{
    uint64_t start, text_end, readonly_end, end, guard, worker_guard, secondary_guard;
};

bool lume_mmu_tables(const struct lume_fdt *, const struct lume_layout *);
bool lume_mmu_translate(uint64_t root, uint64_t va, bool write, bool user, bool execute,
                        uint64_t *physical);
#endif
