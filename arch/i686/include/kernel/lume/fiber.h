#ifndef LUME_ARCH_FIBER_H
#define LUME_ARCH_FIBER_H
#include <stdint.h>

struct lume_fiber
{
    uint32_t saved[4], sp;
};

void lume_fiber_switch(struct lume_fiber *, const struct lume_fiber *);

static inline void lume_fiber_init(struct lume_fiber *fiber, uintptr_t top, void (*entry)(void))
{
    top -= 4;
    *(uint32_t *)top = (uintptr_t)entry;
    *fiber = (struct lume_fiber){.sp = top};
}
#endif
