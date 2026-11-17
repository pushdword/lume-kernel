/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LUME_LINUX_COMPAT_H
#define LUME_LINUX_COMPAT_H
#include <linux/platform_device.h>
#include <linux/virtio.h>
#include <linux/hw_random.h>

/* Each module owns a runtime. Calls require serialized task context.
 * Progress may dispatch its transport callback but must not reenter the runtime.
 * Atomic allocations are rejected until a nonblocking allocator is provided. */
struct lume_linux_host_ops
{
    void *(*allocate)(size_t);
    void (*free)(void *);
    uint64_t (*microseconds)(void);
    void (*pause)(unsigned);
    void (*relax)(void);
    unsigned long (*irq_save)(void);
    void (*irq_restore)(unsigned long);
    void *(*map)(uint64_t, size_t);
    void (*unmap)(void *, size_t);
    u32 (*read32)(const volatile void *);
    void (*write32)(u32, volatile void *);
    void (*log)(const char *, const char *);
};

int lume_linux_runtime_start(const struct lume_linux_host_ops *);
int lume_linux_runtime_stop(void);
unsigned lume_linux_allocations(void);
int lume_linux_device_release(struct device *);
int lume_linux_platform_probe(struct platform_driver *, struct platform_device *);
int lume_linux_hwrng_read(void *, size_t, bool);
bool lume_linux_hwrng_available(void);
int lume_linux_virtio_probe(struct virtio_driver *, struct virtio_device *,
                            const struct lume_linux_virtio_ops *, void *);
int lume_linux_virtio_poll(struct virtio_device *);
int lume_linux_virtio_remove(struct virtio_driver *, struct virtio_device *);
struct platform_driver *lume_linux_platform_descriptor(void);
struct virtio_driver *lume_linux_virtio_descriptor(void);
#endif
