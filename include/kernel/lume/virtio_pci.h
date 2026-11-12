/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef LUME_VIRTIO_PCI_H
#define LUME_VIRTIO_PCI_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LUME_VIRTIO_PCI_QUEUESIZE 1024u
#define LUME_VIRTIO_PCI_VERSION_1 (UINT64_C(1) << 32)

struct lume_virtio_pci_range
{
    uintptr_t address;
    uint32_t bytes;
};

struct lume_virtio_pci_io
{
    void *context;
    uint32_t (*config)(void *, unsigned);
    bool (*range)(void *, unsigned, uint32_t, uint32_t, struct lume_virtio_pci_range *);
    uint32_t (*read)(void *, uintptr_t, unsigned);
    void (*write)(void *, uintptr_t, uint32_t, unsigned);
    void (*barrier)(void *);
    uint64_t (*ticks)(void *);
    void (*pause)(void *);
    uint64_t frequency;
};

struct lume_virtio_pci
{
    struct lume_virtio_pci_io io;
    struct lume_virtio_pci_range common, notify, isr, config;
    uint64_t features, epoch;
    uint32_t notify_multiplier;
    bool failed, ready, reset_complete;
};

struct lume_virtio_pci_descriptor
{
    uint64_t address;
    uint32_t bytes;
    uint16_t flags, next;
};

struct lume_virtio_pci_used
{
    uint32_t id, bytes;
};

struct lume_virtio_pci_queue
{
    struct lume_virtio_pci *device;
    struct lume_virtio_pci_descriptor *descriptors;
    volatile uint16_t *available, *used;
    volatile struct lume_virtio_pci_used *elements;
    uintptr_t notify;
    uint64_t epoch;
    uint64_t inflight[LUME_VIRTIO_PCI_QUEUESIZE / 64];
    uint32_t writable[LUME_VIRTIO_PCI_QUEUESIZE];
    uint16_t number, count, consumed, pending;
    bool enabled;
};

/* Control registers require per-device serialization. Queue operations require
 * per-queue serialization. Each in-flight chain owns all of its descriptors.
 * Published DMA buffers remain owned until completion
 * or an acknowledged device reset. The queue allocation must be DMA-coherent. */
int lume_virtio_pci_start(struct lume_virtio_pci *, const struct lume_virtio_pci_io *, uint16_t,
                          uint64_t, uint64_t);
int lume_virtio_pci_reset(struct lume_virtio_pci *);
size_t lume_virtio_pci_queue_bytes(unsigned);
int lume_virtio_pci_queue_start(struct lume_virtio_pci *, struct lume_virtio_pci_queue *, unsigned,
                                unsigned, void *, uint64_t, size_t);
int lume_virtio_pci_ready(struct lume_virtio_pci *);
int lume_virtio_pci_submit(struct lume_virtio_pci_queue *, unsigned);
int lume_virtio_pci_take(struct lume_virtio_pci_queue *, unsigned *, unsigned *);
unsigned lume_virtio_pci_interrupt(struct lume_virtio_pci *);
#endif
