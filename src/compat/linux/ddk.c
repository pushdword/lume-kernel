/* SPDX-License-Identifier: GPL-2.0-only */
#include <lume/linux_ddk.h>
#include <lume/kernel_memory.h>
#include <lume/mmio.h>
#include <lume/irq.h>
#include <lume/counter.h>
#include <lume/cpu.h>
#include <lume/worker.h>
#include <lume/early.h>
#if defined(LUME_ARCH_AMD64) || defined(LUME_ARCH_I686)
#include <lume/pci.h>
#endif

#if !defined(_WIN32)
_Static_assert(sizeof(unsigned long) == sizeof(void *),
               "Linux driver ABI requires pointer-sized unsigned long");
#endif

static struct lume_hardware *hardware;
static uint64_t frequency;
static volatile uint32_t entropy_idle = 1;
static unsigned entropy_users, entropy_owner = LUME_WORKER_LIMIT;

static uint64_t microseconds(void)
{
    uint64_t ticks = lume_counter_read();
    return ticks / frequency * UINT64_C(1000000) +
           ticks % frequency * UINT64_C(1000000) / frequency;
}

static void pause_us(unsigned us)
{
    uint64_t ticks = frequency / 1000000 * us;
    ticks += (frequency % 1000000 * us + 999999) / 1000000;
    if (!ticks)
        ticks = 1;
    uint64_t start = lume_counter_read();
    if (lume_worker_pause(start + ticks))
        return;
    while (lume_counter_read() - start < ticks)
        lume_cpu_relax();
}

static unsigned long irq_save(void)
{
    return (unsigned long)lume_irq_save();
}

static void irq_restore(unsigned long flags)
{
    lume_irq_restore(flags);
}

static void *map(uint64_t base, size_t bytes)
{
    if (base > UINTPTR_MAX || !bytes || bytes > UINTPTR_MAX - (uintptr_t)base)
        return ERR_PTR(-EINVAL);
    for (unsigned i = 0; i < LUME_RESOURCE_LIMIT; ++i)
    {
        const struct lume_resource *resource = lume_hardware_resource(hardware, i);
        if (!resource)
            break;
        if (resource->kind == LUME_RESOURCE_MMIO && base >= resource->base &&
            base - resource->base <= resource->size &&
            bytes <= resource->size - (base - resource->base))
            return (void *)(uintptr_t)base;
    }
    return ERR_PTR(-ENODEV);
}

static void unmap(void *address, size_t bytes)
{
    (void)address;
    (void)bytes;
    /* Platform MMIO mappings persist for the lifetime of the hardware inventory. */
}

static u32 read32(const volatile void *address)
{
    return lume_mmio_read32((uintptr_t)address);
}

static void write32(u32 value, volatile void *address)
{
    lume_mmio_write32_sync((uintptr_t)address, value);
}

static void log_message(const char *name, const char *message)
{
    lume_puts(name);
    lume_puts(": ");
    lume_puts(message);
}

static int request_shared_irq(unsigned irq, lume_irq_shared_handler handler, void *context,
                              void **handle)
{
    if (lume_worker_independent())
        return -EINVAL;
    for (unsigned i = 0; i < LUME_RESOURCE_LIMIT; ++i)
    {
        const struct lume_resource *resource = lume_hardware_resource(hardware, i);
        if (!resource)
            break;
        if (resource->kind != LUME_RESOURCE_IRQ || resource->base != irq || resource->domain ||
            resource->flags != LUME_IRQ_LEVEL)
            continue;
        struct lume_irq_action *action = lume_kernel_allocate(sizeof(*action));
        if (!action)
            return -ENOMEM;
        *action = (struct lume_irq_action){0};
        int result = lume_irq_request_shared(irq, action, handler, context);
        if (result)
        {
            lume_kernel_free(action);
            return result;
        }
        *handle = action;
        return 0;
    }
    return -ENODEV;
}

static int release_shared_irq(unsigned irq, void *handle)
{
    if (lume_worker_independent())
        return -EINVAL;
    int result = lume_irq_release_shared(irq, handle);
    if (!result)
        lume_kernel_free(handle);
    return result;
}

#if defined(LUME_ARCH_AMD64) || defined(LUME_ARCH_I686)
enum
{
    COHERENT_PAGE_BYTES = 4096
};

static uint32_t pci_address;

static int pci_config_read(unsigned offset, unsigned width, u32 *value)
{
    return lume_q35_pci_config_read(pci_address, offset, width, value);
}

static int pci_config_write(unsigned offset, unsigned width, u32 value)
{
    return lume_q35_pci_config_write(pci_address, offset, width, value);
}

static void pci_resource(unsigned bar, struct lume_linux_pci_bar *out)
{
    struct lume_property property;
    if (lume_hardware_property(hardware, "pci-bar", LUME_PROPERTY_U64, bar, &property) ||
        property.count != 5)
        return;
    unsigned long flags = IORESOURCE_IO;
    if (property.values[2] == LUME_RESOURCE_MMIO)
    {
        flags = IORESOURCE_MEM;
        if (property.values[3])
            flags |= IORESOURCE_PREFETCH;
        if (property.values[4])
            flags |= IORESOURCE_MEM_64;
    }
    *out = (struct lume_linux_pci_bar){property.values[0], property.values[1], flags};
}

static unsigned pci_interrupt(const struct lume_hardware *device)
{
    for (unsigned i = 0; i < LUME_RESOURCE_LIMIT; ++i)
    {
        const struct lume_resource *resource = lume_hardware_resource(device, i);
        if (!resource)
            break;
        if (resource->kind == LUME_RESOURCE_IRQ && !resource->domain &&
            resource->flags == LUME_IRQ_LEVEL && resource->base < LUME_IRQ_LIMIT)
            return (unsigned)resource->base;
    }
    return 0;
}

static bool dma_domain(u64 mask, struct lume_dma_domain *domain)
{
    if (lume_hardware_dma(hardware, domain))
        return false;
    if (mask < domain->mask)
        domain->mask = mask;
    return true;
}

static bool dma_mask_supported(u64 mask)
{
    struct lume_dma_domain domain;
    uint64_t bus;
    return dma_domain(mask, &domain) &&
           lume_dma_map(&domain, domain.physical_base, COHERENT_PAGE_BYTES, &bus);
}

static void free_coherent_pages(uint64_t first, unsigned pages)
{
    for (unsigned i = 0; i < pages; ++i)
        (void)lume_page_free(first + (uint64_t)i * COHERENT_PAGE_BYTES);
}

static void *allocate_coherent(size_t span, u64 mask, dma_addr_t *handle)
{
    struct lume_dma_domain domain;
    size_t pages = span / COHERENT_PAGE_BYTES;
    if (!pages || pages > UINT32_MAX / 2 || !dma_domain(mask, &domain))
        return NULL;
    unsigned total = 2 * (unsigned)pages - 1;
    uint64_t base = lume_pages_alloc_span_uninitialized(total);
    if (!base)
        return NULL;
    uint64_t padding = (-base) & (span - 1);
    uint64_t first = base + padding;
    uint64_t bus;
    if (first < base || first > UINTPTR_MAX || span > UINTPTR_MAX - first ||
        !lume_dma_map(&domain, first, span, &bus) || (bus & (span - 1)))
    {
        free_coherent_pages(base, total);
        return NULL;
    }
    unsigned prefix = (unsigned)(padding / COHERENT_PAGE_BYTES);
    free_coherent_pages(base, prefix);
    free_coherent_pages(first + span, total - prefix - (unsigned)pages);
    *handle = bus;
    return (void *)(uintptr_t)first;
}

static void free_coherent(void *cpu, size_t span)
{
    free_coherent_pages((uintptr_t)cpu, (unsigned)(span / COHERENT_PAGE_BYTES));
}

static bool streaming_address(const void *buffer, size_t bytes, u64 mask, dma_addr_t *bus)
{
    struct lume_dma_domain domain;
    /* The allocator returns contiguous, identity-mapped kernel memory on PC. */
    return dma_domain(mask, &domain) && lume_dma_map(&domain, (uintptr_t)buffer, bytes, bus);
}

static bool streaming_sync(const void *buffer, size_t bytes, enum dma_data_direction direction,
                           bool for_cpu)
{
    static const enum lume_dma_direction directions[] = {
        [DMA_BIDIRECTIONAL] = LUME_DMA_BIDIRECTIONAL,
        [DMA_TO_DEVICE] = LUME_DMA_TO_DEVICE,
        [DMA_FROM_DEVICE] = LUME_DMA_FROM_DEVICE,
    };

    return for_cpu ? lume_dma_sync_for_cpu(buffer, bytes, directions[direction])
                   : lume_dma_sync_for_device(buffer, bytes, directions[direction]);
}
#endif

int lume_linux_ddk_start(struct lume_hardware *device)
{
    if (lume_worker_independent())
        return -EINVAL;
    uint64_t ticks = lume_counter_frequency();
    if (!device || !ticks || ticks > UINT64_C(10000000000))
        return -EINVAL;
    struct lume_linux_host_ops ops = {
        .allocate = lume_kernel_allocate,
        .free = lume_kernel_free,
        .microseconds = microseconds,
        .pause = pause_us,
        .relax = lume_cpu_relax,
        .irq_save = irq_save,
        .irq_restore = irq_restore,
        .map = map,
        .unmap = unmap,
        .read32 = read32,
        .write32 = write32,
        .log = log_message,
        .request_shared_irq = request_shared_irq,
        .release_shared_irq = release_shared_irq,
#if defined(LUME_ARCH_AMD64) || defined(LUME_ARCH_I686)
        .dma_mask_supported = dma_mask_supported,
        .coherent_allocate = allocate_coherent,
        .coherent_free = free_coherent,
        .streaming_address = streaming_address,
        .streaming_sync = streaming_sync,
#endif
    };
#if defined(LUME_ARCH_AMD64) || defined(LUME_ARCH_I686)
    struct lume_property property;
    bool pci = !lume_hardware_property(device, "pci-address", LUME_PROPERTY_U32, 0, &property) &&
               property.count == 1 && !(property.values[0] & ~UINT64_C(0x00ffff00));
    if (pci)
    {
        ops.pci_config_read = pci_config_read;
        ops.pci_config_write = pci_config_write;
        ops.pci_resource = pci_resource;
        ops.pci_irq = pci_interrupt(device);
    }
#endif
    int result = lume_linux_runtime_start(&ops);
    if (!result)
    {
        hardware = device;
        frequency = ticks;
#if defined(LUME_ARCH_AMD64) || defined(LUME_ARCH_I686)
        pci_address = pci ? (uint32_t)property.values[0] : 0;
#endif
    }
    return result;
}

int lume_linux_ddk_can_stop(void)
{
    return lume_worker_independent() || entropy_users ? -EBUSY : 0;
}

struct entropy_request
{
    void *out;
    size_t bytes;
    bool complete;
};

static void read_entropy(void *context)
{
    struct entropy_request *request = context;
    unsigned current = lume_worker_current();
    if (entropy_users == UINT32_MAX)
        return;
    ++entropy_users;
    uint64_t deadline = lume_counter_read() + frequency * 5;
    while (!__atomic_load_n(&entropy_idle, __ATOMIC_ACQUIRE))
    {
        if (current >= LUME_WORKER_LIMIT || current == entropy_owner ||
            (int64_t)(lume_counter_read() - deadline) >= 0 ||
            !lume_worker_wait_bits(&entropy_idle, 1, deadline))
            goto done;
    }
    entropy_owner = current;
    __atomic_store_n(&entropy_idle, 0, __ATOMIC_RELEASE);
    request->complete =
        lume_linux_hwrng_read(request->out, request->bytes, true) == (int)request->bytes;
    entropy_owner = LUME_WORKER_LIMIT;
    __atomic_store_n(&entropy_idle, 1, __ATOMIC_RELEASE);
done:
    --entropy_users;
}

bool lume_linux_ddk_entropy(void *out, size_t bytes)
{
    if (!out || !bytes || bytes > 4096)
        return false;
    struct entropy_request request = {out, bytes, false};
    lume_worker_scheduler(read_entropy, &request);
    if (request.complete)
        return true;
    volatile unsigned char *wipe = out;
    for (size_t i = 0; i < bytes; ++i)
        wipe[i] = 0;
    return false;
}
