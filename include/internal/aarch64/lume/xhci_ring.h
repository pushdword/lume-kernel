#ifndef LUME_XHCI_RING_H
#define LUME_XHCI_RING_H
#include <stdint.h>

struct lume_xhci_trb
{
    uint64_t parameter;
    uint32_t status, control;
};

_Static_assert(sizeof(struct lume_xhci_trb) == 16, "xHCI TRB layout");

struct lume_xhci_ring
{
    struct lume_xhci_trb *trbs;
    uint64_t dma;
    unsigned head, cycle;
};

/* One outstanding TD per endpoint; no producer can lap the hardware. */
static inline uint64_t lume_xhci_enqueue(struct lume_xhci_ring *r, uint64_t p, uint32_t s,
                                         uint32_t control)
{
    unsigned i = r->head;
    r->trbs[i] = (struct lume_xhci_trb){p, s, (control & ~1u) | r->cycle};
    if (++r->head == 255)
    {
        r->trbs[255] =
            (struct lume_xhci_trb){r->dma, 0, (6u << 10) | 2 | r->cycle | (control & 16)};
        r->head = 0;
        r->cycle ^= 1;
    }
    return r->dma + i * 16;
}
#endif
