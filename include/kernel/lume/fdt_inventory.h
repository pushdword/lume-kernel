#ifndef LUME_FDT_INVENTORY_H
#define LUME_FDT_INVENTORY_H
#include "lume/hardware.h"

struct lume_fdt_inventory_ops
{
    int (*interrupt)(const void *tree, int controller, const uint32_t *cells, unsigned count,
                     struct lume_resource *);
};

/* The validated tree remains immutable and resident while its devices exist. */
int lume_hardware_import_fdt(const void *tree, size_t bytes, const struct lume_fdt_inventory_ops *,
                             unsigned *added);
int lume_fdt_mmio_resource(const void *tree, int node, unsigned index, struct lume_resource *);
#endif
