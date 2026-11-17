/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef LUME_V3D_DEVICE_H
#define LUME_V3D_DEVICE_H
#include "lume/dma.h"
#include "lume/v3d.h"

struct lume_v3d_device_ops
{
    int (*start)(void *);
    int (*stop)(void *);
    int (*parameter)(void *, unsigned, uint64_t *);
    int (*execute)(void *, uint64_t, const struct lume_v3d_request *);
    bool (*map)(void *, uint64_t, size_t, uint64_t *);
    bool (*sync)(void *, const void *, size_t, enum lume_dma_direction, bool);
    bool (*isolated)(void *);
};

int lume_v3d_device_register(const struct lume_v3d_device_ops *, void *);
int lume_v3d_device_unregister(const struct lume_v3d_device_ops *, void *);
#endif
