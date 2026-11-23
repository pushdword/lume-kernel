/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LUME_LINUX_VIRTIO_H
#define LUME_LINUX_VIRTIO_H
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/idr.h>
#include <linux/scatterlist.h>
struct virtio_device;

#define LUME_LINUX_VIRTQUEUE_SLOTS 32u

struct lume_linux_virtqueue_slot
{
    void *token;
    unsigned writable, written;
    bool completed;
};

struct virtqueue
{
    struct virtio_device *vdev;
    void (*callback)(struct virtqueue *);
    struct lume_linux_virtqueue_slot slots[LUME_LINUX_VIRTQUEUE_SLOTS];
    uint8_t completed[LUME_LINUX_VIRTQUEUE_SLOTS];
    unsigned pending, ready, next;
    uint32_t staged;
};

struct virtio_device_id
{
    unsigned device, vendor;
};

struct virtio_config_ops
{
    void (*del_vqs)(struct virtio_device *);
};

struct lume_linux_virtio_ops
{
    int (*create)(void *);
    /* Scatterlists are borrowed during submit. Their buffers belong to the queue until
     * completion or reset. Without atomic_submit, submit may allocate in task context. */
    int (*submit)(void *, unsigned slot, struct scatterlist *const *, unsigned out, unsigned in,
                  unsigned segments, gfp_t);
    /* Publish and notify only the slots selected by the mask. */
    void (*notify)(void *, uint32_t slots);
    int (*take)(void *, unsigned *slot, unsigned *written);
    void (*ready)(void *);
    int (*reset)(void *);
    void (*destroy)(void *);
    /* Return true after waiting for a queue event or reaching the time limit. */
    bool (*wait)(void *, unsigned microseconds);
    unsigned max_segments, max_pending;
    bool atomic_submit;
};

struct virtio_device
{
    struct device dev;
    void *priv;
    const struct virtio_config_ops *config;
    const struct lume_linux_virtio_ops *transport;
    void *transport_context;
    struct virtqueue queue;
    int error;
    bool reset, probed;
};

struct virtio_driver
{
    struct device_driver driver;
    const struct virtio_device_id *id_table;
    int (*probe)(struct virtio_device *);
    void (*remove)(struct virtio_device *);
    void (*scan)(struct virtio_device *);
    int (*freeze)(struct virtio_device *);
    int (*restore)(struct virtio_device *);
};
struct virtqueue *virtio_find_single_vq(struct virtio_device *, void (*)(struct virtqueue *),
                                        const char *);
int virtqueue_add_inbuf(struct virtqueue *, struct scatterlist *, unsigned, void *, gfp_t);
int virtqueue_add_outbuf(struct virtqueue *, struct scatterlist *, unsigned, void *, gfp_t);
int virtqueue_add_sgs(struct virtqueue *, struct scatterlist *[], unsigned, unsigned, void *,
                      gfp_t);
bool virtqueue_kick(struct virtqueue *);
void *virtqueue_get_buf(struct virtqueue *, unsigned *);
void virtio_device_ready(struct virtio_device *);
void virtio_reset_device(struct virtio_device *);
#define module_virtio_driver(driver)                                                               \
    struct virtio_driver *lume_linux_virtio_descriptor(void)                                       \
    {                                                                                              \
        return &(driver);                                                                          \
    }
#endif
