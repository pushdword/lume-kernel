#ifndef LUME_PI5_FDT_IRQ_H
#define LUME_PI5_FDT_IRQ_H
#include "lume/resource.h"

int lume_pi5_fdt_interrupt(const void *tree, int controller, const uint32_t *cells, unsigned count,
                           struct lume_resource *resource);
#endif
