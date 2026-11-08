/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LUME_V3D_H
#define LUME_V3D_H
#include "lume/gpu_device.h"
#include "lume/dma.h"

enum
{
    V3D_HUB,
    V3D_CORE,
    V3D_SMS
};

struct lume_v3d_io
{
    void *context;
    uint32_t (*read)(void *, unsigned, unsigned);
    void (*write)(void *, unsigned, unsigned, uint32_t);
    void (*barrier)(void *);
    uint64_t (*ticks)(void *);
    uint64_t frequency;
    int (*power)(void *, bool);
    void (*interrupts)(void *, bool);
    /* Returns a page-aligned span whose complete contents are zero. */
    uint64_t (*allocate_zeroed)(void *, unsigned);
    void (*release)(void *, uint64_t, unsigned);
    bool (*map)(void *, uint64_t, size_t, uint64_t *);
    bool (*sync)(void *, const void *, size_t, size_t, unsigned, enum lume_dma_direction, bool);
    bool (*wait)(void *, volatile uint32_t *, uint32_t, uint64_t);
    void (*checkpoint)(void *);
    void (*fatal)(const char *);
};

struct lume_v3d
{
    struct lume_v3d_io io;
    struct lume_gpu_surface surface;
    uint64_t table, scratch, guard;
    unsigned scratch_pages, version;
    unsigned phase;
    uint32_t identity, debug, status, violation;
    unsigned completion;
    volatile uint32_t event;
    bool powered, bound, failed, linear_surface;
    uint64_t jobs, pixels;
};

int lume_v3d_bind(struct lume_v3d *, const struct lume_gpu_surface *);
int lume_v3d_copy(struct lume_v3d *, const struct lume_gpu_rect *, unsigned);
void lume_v3d_unbind(struct lume_v3d *);
void lume_v3d_interrupt(struct lume_v3d *);
bool lume_v3d_completed(struct lume_v3d *);
#endif
