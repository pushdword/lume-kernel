#ifndef LUME_Q35_VIRTIO_H
#define LUME_Q35_VIRTIO_H
#include "lume/q35.h"
#define LUME_VIRTQUEUE_LIMIT 1024u

struct lume_virtio_desc
{
    uint64_t address;
    uint32_t bytes;
    uint16_t flags, next;
};

struct lume_virtio_used
{
    uint32_t id, bytes;
};

struct lume_virtqueue
{
    uint16_t port, number, count, consumed;
    struct lume_virtio_desc *descriptors;
    volatile uint16_t *available, *used;
    volatile struct lume_virtio_used *elements;
};

struct lume_virtio
{
    uint16_t port;
    uint32_t features;
    bool failed;
};

bool lume_virtio_open(struct lume_virtio *, uint16_t, uint32_t);
bool lume_virtio_queue(struct lume_virtio *, unsigned, struct lume_virtqueue *);
void lume_virtio_ready(struct lume_virtio *);
void lume_virtio_fail(struct lume_virtio *);
void lume_virtio_submit(struct lume_virtqueue *, unsigned);
bool lume_virtio_take(struct lume_virtqueue *, unsigned *, unsigned *);
bool lume_virtio_wait(struct lume_virtio *, struct lume_virtqueue *, unsigned *);
void lume_q35_block_start(void);
void lume_q35_net_start(void);
void lume_q35_entropy_start(void);
#endif
