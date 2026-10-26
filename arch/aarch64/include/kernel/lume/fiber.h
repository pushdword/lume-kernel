#ifndef LUME_ARCH_FIBER_H
#define LUME_ARCH_FIBER_H
#include <stddef.h>
#include <stdint.h>

struct lume_fiber
{
    uint64_t saved[12], sp, reserved, vector[8], fpcr, fpsr;
};

_Static_assert(sizeof(struct lume_fiber) == 192, "kernel fiber layout");
_Static_assert(offsetof(struct lume_fiber, sp) == 96, "kernel fiber stack offset");
_Static_assert(offsetof(struct lume_fiber, vector) == 112, "kernel fiber SIMD offset");
_Static_assert(offsetof(struct lume_fiber, fpcr) == 176, "kernel fiber FP offset");
void lume_fiber_switch(struct lume_fiber *from, const struct lume_fiber *to);

static inline void lume_fiber_init(struct lume_fiber *fiber, uintptr_t top, void (*entry)(void))
{
    *fiber = (struct lume_fiber){.sp = top};
    fiber->saved[11] = (uintptr_t)entry;
}
#endif
