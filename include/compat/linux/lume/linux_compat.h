/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LUME_LINUX_COMPAT_H
#define LUME_LINUX_COMPAT_H
#include <linux/platform_device.h>
#include <linux/virtio.h>
#include <linux/hw_random.h>
#include <linux/dma-mapping.h>
#include <linux/pci.h>

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
    /* Coherent providers return a naturally aligned CPU span and device address. */
    bool (*dma_mask_supported)(u64 mask);
    void *(*coherent_allocate)(size_t span, u64 mask, dma_addr_t *);
    void (*coherent_free)(void *, size_t span);
    /* Address translation retains no resources. Synchronization transfers buffer ownership. */
    bool (*streaming_address)(const void *, size_t, u64 mask, dma_addr_t *);
    bool (*streaming_sync)(const void *, size_t, enum dma_data_direction, bool for_cpu);
    int (*request_shared_irq)(unsigned, bool (*)(unsigned, void *), void *context, void **handle);
    int (*release_shared_irq)(unsigned, void *handle);
    int (*pci_config_read)(unsigned offset, unsigned width, u32 *);
    int (*pci_config_write)(unsigned offset, unsigned width, u32);
    void (*pci_resource)(unsigned bar, struct lume_linux_pci_bar *);
    unsigned pci_irq;
};

struct lume_linux_runtime_stats
{
    uint64_t completion_waits, completion_polls, completion_microseconds, completion_timeouts;
    uint64_t pause_calls, pause_requested_microseconds;
    uint64_t entropy_reads, entropy_bytes, entropy_microseconds;
};

int lume_linux_workqueue_start(void);
bool lume_linux_workqueue_busy(void);
void lume_linux_workqueue_stop(void);
int lume_linux_kthread_start(void);
bool lume_linux_kthread_busy(void);
void lume_linux_kthread_stop(void);

/* Counters cover one runtime instance and reset on successful startup.
 * Snapshot calls require the same serialization as driver operations. */
void lume_linux_runtime_snapshot(struct lume_linux_runtime_stats *);
int lume_linux_runtime_start(const struct lume_linux_host_ops *);
int lume_linux_runtime_stop(void);
unsigned lume_linux_allocations(void);
int lume_linux_pci_attach(struct pci_dev *);
int lume_linux_device_release(struct device *);
int lume_linux_platform_probe(struct platform_driver *, struct platform_device *);
int lume_linux_hwrng_read(void *, size_t, bool);
bool lume_linux_hwrng_available(void);
int lume_linux_virtio_attach(struct virtio_device *, const struct lume_linux_virtio_ops *, void *);
int lume_linux_virtio_detach(struct virtio_device *);
int lume_linux_virtio_probe(struct virtio_driver *, struct virtio_device *,
                            const struct lume_linux_virtio_ops *, void *);
int lume_linux_virtio_poll(struct virtio_device *);
int lume_linux_virtio_remove(struct virtio_driver *, struct virtio_device *);
struct platform_driver *lume_linux_platform_descriptor(void);
struct virtio_driver *lume_linux_virtio_descriptor(void);
#endif
