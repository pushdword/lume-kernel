/* SPDX-License-Identifier: GPL-2.0-only */
#include <lume/linux_compat.h>
#include <lume/linux_memory.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/string.h>
#include <linux/workqueue.h>
#include <stdarg.h>

static struct lume_linux_host_ops host;
static bool started, reading;
static struct hwrng *active_rng;
static struct virtio_device *poll_device;
static struct pci_dev *pci_device;
static struct lume_linux_runtime_stats statistics;
static bool in_irq;
static int managed(struct device *, void (*)(void *), void *);
static bool detach_managed(struct device *, void (*)(void *), void *);

struct interrupt_request
{
    struct interrupt_request *next;
    irq_handler_t handler;
    irq_handler_t thread_handler;
    struct work_struct thread_work;
    const char *name;
    void *device, *handle;
    unsigned irq;
    bool in_thread;
};

static struct interrupt_request *interrupts;

void lume_linux_runtime_snapshot(struct lume_linux_runtime_stats *out)
{
    if (out)
        *out = statistics;
}

static void pause_us(unsigned us)
{
    ++statistics.pause_calls;
    statistics.pause_requested_microseconds += us;
    host.pause(us);
}

int lume_linux_runtime_start(const struct lume_linux_host_ops *ops)
{
    if (started)
        return -EBUSY;
    if (!ops || !ops->allocate || !ops->free || !ops->microseconds || !ops->pause || !ops->relax ||
        !ops->irq_save || !ops->irq_restore || !ops->map || !ops->unmap || !ops->read32 ||
        !ops->write32)
        return -EINVAL;
    if ((ops->coherent_allocate != NULL) != (ops->coherent_free != NULL) ||
        (ops->streaming_address != NULL) != (ops->streaming_sync != NULL) ||
        ((ops->coherent_allocate || ops->streaming_address) && !ops->dma_mask_supported) ||
        (ops->request_shared_irq != NULL) != (ops->release_shared_irq != NULL) ||
        (ops->pci_config_read != NULL) != (ops->pci_config_write != NULL))
        return -EINVAL;
    int result = lume_linux_memory_start(ops->allocate, ops->free);
    if (result)
        return result;
    result = lume_linux_workqueue_start();
    if (result)
    {
        (void)lume_linux_memory_stop();
        return result;
    }
    result = lume_linux_kthread_start();
    if (result)
    {
        lume_linux_workqueue_stop();
        (void)lume_linux_memory_stop();
        return result;
    }
    host = *ops;
    statistics = (struct lume_linux_runtime_stats){0};
    started = true;
    return 0;
}

int lume_linux_runtime_stop(void)
{
    if (lume_linux_allocations() || active_rng || poll_device || reading ||
        (pci_device && pci_device->dev.resource_count) ||
        lume_linux_workqueue_busy() || lume_linux_kthread_busy() || lume_linux_tasklet_busy())
        return -EBUSY;
    int result = lume_linux_memory_stop();
    if (result)
        return result;
    memset(&host, 0, sizeof(host));
    pci_device = NULL;
    started = false;
    lume_linux_kthread_stop();
    lume_linux_workqueue_stop();
    return 0;
}

unsigned lume_linux_allocations(void)
{
    return lume_linux_memory_allocations();
}

int lume_linux_pci_attach(struct pci_dev *device)
{
    if (!device || in_irq)
        return -EINVAL;
    if (!started || !host.pci_config_read)
        return -ENODEV;
    if (pci_device)
        return -EBUSY;
    u32 identity;
    int result = host.pci_config_read(PCI_VENDOR_ID, 4, &identity);
    if (result)
        return result;
    if ((u16)identity == UINT16_MAX)
        return -ENODEV;
    device->vendor = (u16)identity;
    device->device = (u16)(identity >> 16);
    device->irq = host.pci_irq;
    for (unsigned bar = 0; bar < PCI_STD_NUM_BARS; ++bar)
    {
        device->bars[bar] = (struct lume_linux_pci_bar){0};
        if (host.pci_resource)
            host.pci_resource(bar, &device->bars[bar]);
    }
    pci_device = device;
    return 0;
}

static int check_pci_config(const struct pci_dev *device, int where, unsigned width)
{
    if (!device || device != pci_device)
        return PCIBIOS_DEVICE_NOT_FOUND;
    if ((width != 1 && width != 2 && width != 4) || where < 0 || (unsigned)where > 4096 - width ||
        ((unsigned)where & (width - 1)))
        return PCIBIOS_BAD_REGISTER_NUMBER;
    return PCIBIOS_SUCCESSFUL;
}

static int pci_config_status(int result)
{
    if (result == -EINVAL || result == -EOPNOTSUPP)
        return PCIBIOS_BAD_REGISTER_NUMBER;
    if (result == -ENODEV)
        return PCIBIOS_DEVICE_NOT_FOUND;
    return result ? PCIBIOS_SET_FAILED : PCIBIOS_SUCCESSFUL;
}

int lume_linux_pci_read_config(const struct pci_dev *device, int where, unsigned width, u32 *value)
{
    if (!value)
        return PCIBIOS_BAD_REGISTER_NUMBER;
    *value = UINT32_MAX;
    int status = check_pci_config(device, where, width);
    if (status)
        return status;
    u32 raw;
    status = pci_config_status(host.pci_config_read((unsigned)where, width, &raw));
    if (!status)
        *value = raw;
    return status;
}

int lume_linux_pci_write_config(const struct pci_dev *device, int where, unsigned width, u32 value)
{
    int status = check_pci_config(device, where, width);
    return status ? status
                  : pci_config_status(host.pci_config_write((unsigned)where, width, value));
}

static int pci_set_command(struct pci_dev *device, u16 set, u16 clear)
{
    u16 command;
    if (pci_read_config_word(device, PCI_COMMAND, &command))
        return -EIO;
    command = (u16)((command | set) & ~clear);
    return pci_write_config_word(device, PCI_COMMAND, command) ? -EIO : 0;
}

static int enable_pci_device(struct pci_dev *device, bool memory_only)
{
    if (!device || device != pci_device || in_irq)
        return -EINVAL;
    u16 command = 0;
    for (unsigned bar = 0; bar < PCI_STD_NUM_BARS; ++bar)
    {
        unsigned long flags = pci_resource_flags(device, bar);
        if (flags & IORESOURCE_MEM)
            command |= PCI_COMMAND_MEMORY;
        else if (!memory_only && (flags & IORESOURCE_IO))
            command |= PCI_COMMAND_IO;
    }
    int result = pci_set_command(device, command, 0);
    if (!result)
        device->enabled = true;
    return result;
}

int pci_enable_device(struct pci_dev *device)
{
    return enable_pci_device(device, false);
}

int pci_enable_device_mem(struct pci_dev *device)
{
    return enable_pci_device(device, true);
}

static void disable_managed_pci(void *data)
{
    struct pci_dev *device = data;
    pci_disable_device(device);
    device->managed_enabled = false;
}

int pcim_enable_device(struct pci_dev *device)
{
    if (!device || device != pci_device)
        return -EINVAL;
    if (device->managed_enabled)
        return 0;
    int result = pci_enable_device(device);
    if (!result)
        result = managed(&device->dev, disable_managed_pci, device);
    if (result)
    {
        pci_disable_device(device);
        return result;
    }
    device->managed_enabled = true;
    return 0;
}

void pci_disable_device(struct pci_dev *device)
{
    if (device && device == pci_device && !in_irq &&
        !pci_set_command(device, 0, PCI_COMMAND_IO | PCI_COMMAND_MEMORY))
        device->enabled = false;
}

void pci_set_master(struct pci_dev *device)
{
    if (device && device == pci_device && !in_irq &&
        !pci_set_command(device, PCI_COMMAND_MASTER, 0))
        device->bus_master = true;
}

void pci_clear_master(struct pci_dev *device)
{
    if (device && device == pci_device && !in_irq &&
        !pci_set_command(device, 0, PCI_COMMAND_MASTER))
        device->bus_master = false;
}

int pci_request_region(struct pci_dev *device, unsigned bar, const char *name)
{
    if (!device || device != pci_device || bar >= PCI_STD_NUM_BARS || !name || !*name ||
        !pci_resource_len(device, bar) ||
        !(pci_resource_flags(device, bar) & (IORESOURCE_IO | IORESOURCE_MEM)))
        return -EINVAL;
    unsigned long mask = 1ul << bar;
    if (device->requested_bars & mask)
        return -EBUSY;
    device->requested_bars |= mask;
    return 0;
}

int pci_request_regions(struct pci_dev *device, const char *name)
{
    if (!device || device != pci_device || !name || !*name)
        return -EINVAL;
    unsigned long acquired = 0;
    for (unsigned bar = 0; bar < PCI_STD_NUM_BARS; ++bar)
    {
        if (!pci_resource_len(device, bar))
            continue;
        int result = pci_request_region(device, bar, name);
        if (result)
        {
            device->requested_bars &= ~acquired;
            return result;
        }
        acquired |= 1ul << bar;
    }
    return acquired ? 0 : -ENODEV;
}

unsigned long pci_select_bars(const struct pci_dev *device, unsigned long flags)
{
    unsigned long bars = 0;
    if (!device || device != pci_device)
        return 0;
    for (unsigned bar = 0; bar < PCI_STD_NUM_BARS; ++bar)
        if (pci_resource_len(device, bar) && (pci_resource_flags(device, bar) & flags))
            bars |= 1ul << bar;
    return bars;
}

int pci_request_selected_regions(struct pci_dev *device, unsigned long bars, const char *name)
{
    if (!device || device != pci_device || !name || !*name ||
        (bars & ~((1ul << PCI_STD_NUM_BARS) - 1)))
        return -EINVAL;
    unsigned long acquired = 0;
    for (unsigned bar = 0; bar < PCI_STD_NUM_BARS; ++bar)
        if (bars & (1ul << bar))
        {
            int result = pci_request_region(device, bar, name);
            if (result)
            {
                device->requested_bars &= ~acquired;
                return result;
            }
            acquired |= 1ul << bar;
        }
    return acquired ? 0 : -ENODEV;
}

int pci_request_mem_regions(struct pci_dev *device, const char *name)
{
    return pci_request_selected_regions(device, pci_select_bars(device, IORESOURCE_MEM), name);
}

void pci_release_region(struct pci_dev *device, unsigned bar)
{
    if (device && device == pci_device && bar < PCI_STD_NUM_BARS)
        device->requested_bars &= ~(1ul << bar);
}

void pci_release_regions(struct pci_dev *device)
{
    if (device && device == pci_device)
        device->requested_bars = 0;
}

void pci_release_selected_regions(struct pci_dev *device, unsigned long bars)
{
    if (device && device == pci_device)
        device->requested_bars &= ~bars;
}

struct pci_mapping
{
    struct pci_mapping *next;
    struct pci_dev *device;
    void *address;
    size_t bytes;
    bool automatic;
};

static struct pci_mapping *pci_mappings;

static void release_pci_mapping(void *data)
{
    struct pci_mapping *mapping = data;
    for (struct pci_mapping **entry = &pci_mappings; *entry; entry = &(*entry)->next)
        if (*entry == mapping)
        {
            *entry = mapping->next;
            host.unmap(mapping->address, mapping->bytes);
            kfree(mapping);
            return;
        }
}

static void *map_pci_bar(struct pci_dev *device, unsigned bar, unsigned long offset,
                         unsigned long maxlen, bool automatic)
{
    if (!device || device != pci_device || in_irq || bar >= PCI_STD_NUM_BARS ||
        !(pci_resource_flags(device, bar) & IORESOURCE_MEM))
        return NULL;
    u64 resource_length = pci_resource_len(device, bar);
    if (offset >= resource_length)
        return NULL;
    u64 length = resource_length - offset;
    if (maxlen && maxlen < length)
        length = maxlen;
    u64 start = pci_resource_start(device, bar);
    if (start > UINT64_MAX - offset)
        return NULL;
    start += offset;
    if (!length || length > SIZE_MAX || start > UINT64_MAX - length)
        return NULL;
    struct pci_mapping *mapping = kmalloc(sizeof(*mapping), GFP_KERNEL);
    if (!mapping)
        return NULL;
    void *address = host.map(start, (size_t)length);
    if (!address || IS_ERR(address))
    {
        kfree(mapping);
        return NULL;
    }
    *mapping = (struct pci_mapping){
        .next = pci_mappings,
        .device = device,
        .address = address,
        .bytes = (size_t)length,
        .automatic = automatic,
    };
    pci_mappings = mapping;
    if (automatic && managed(&device->dev, release_pci_mapping, mapping))
    {
        release_pci_mapping(mapping);
        return NULL;
    }
    return address;
}

void *pci_iomap(struct pci_dev *device, unsigned bar, unsigned long maxlen)
{
    return map_pci_bar(device, bar, 0, maxlen, false);
}

void *pci_iomap_range(struct pci_dev *device, unsigned bar, unsigned long offset,
                      unsigned long maxlen)
{
    return map_pci_bar(device, bar, offset, maxlen, false);
}

void *pcim_iomap(struct pci_dev *device, unsigned bar, unsigned long maxlen)
{
    return map_pci_bar(device, bar, 0, maxlen, true);
}

void pci_iounmap(struct pci_dev *device, void *address)
{
    if (in_irq)
        return;
    for (struct pci_mapping **entry = &pci_mappings; *entry; entry = &(*entry)->next)
    {
        struct pci_mapping *mapping = *entry;
        if (mapping->device != device || mapping->address != address)
            continue;
        if (mapping->automatic &&
            !detach_managed(&device->dev, release_pci_mapping, mapping))
            return;
        release_pci_mapping(mapping);
        return;
    }
}

void pcim_iounmap(struct pci_dev *device, void *address)
{
    pci_iounmap(device, address);
}

static bool invoke_irq(unsigned irq, void *context)
{
    struct interrupt_request *request = context;
    bool previous = in_irq;
    in_irq = true;
    irqreturn_t result = request->handler((int)irq, request->device);
    in_irq = previous;
    if (result == IRQ_WAKE_THREAD && request->thread_handler)
    {
        (void)schedule_work(&request->thread_work);
        return true;
    }
    return result == IRQ_HANDLED;
}

static irqreturn_t wake_irq_thread(int irq, void *device)
{
    (void)irq;
    (void)device;
    return IRQ_WAKE_THREAD;
}

static void invoke_irq_thread(struct work_struct *work)
{
    struct interrupt_request *request = container_of(work, struct interrupt_request, thread_work);
    __atomic_store_n(&request->in_thread, true, __ATOMIC_RELEASE);
    (void)request->thread_handler((int)request->irq, request->device);
    __atomic_store_n(&request->in_thread, false, __ATOMIC_RELEASE);
}

int request_threaded_irq(unsigned irq, irq_handler_t handler, irq_handler_t thread_handler,
                         unsigned long flags, const char *name, void *device)
{
    if (!started || in_irq || irq > INT32_MAX || (!handler && !thread_handler) || !name || !device)
        return -EINVAL;
    if (flags != IRQF_SHARED || !host.request_shared_irq)
        return -EOPNOTSUPP;
    for (struct interrupt_request *item = interrupts; item; item = item->next)
        if (item->irq == irq && item->device == device)
            return -EBUSY;
    struct interrupt_request *request = kmalloc(sizeof(*request), GFP_KERNEL);
    if (!request)
        return -ENOMEM;
    *request = (struct interrupt_request){.handler = handler ? handler : wake_irq_thread,
                                          .thread_handler = thread_handler,
                                          .name = name,
                                          .device = device,
                                          .irq = irq};
    INIT_WORK(&request->thread_work, invoke_irq_thread);
    int result = host.request_shared_irq(irq, invoke_irq, request, &request->handle);
    if (result)
    {
        kfree(request);
        return result;
    }
    request->next = interrupts;
    interrupts = request;
    return 0;
}

int request_irq(unsigned irq, irq_handler_t handler, unsigned long flags, const char *name,
                void *device)
{
    return request_threaded_irq(irq, handler, NULL, flags, name, device);
}

const void *free_irq(unsigned irq, void *device)
{
    if (in_irq)
        return NULL;
    for (struct interrupt_request **entry = &interrupts; *entry; entry = &(*entry)->next)
    {
        struct interrupt_request *request = *entry;
        if (request->irq != irq || request->device != device)
            continue;
        if (__atomic_load_n(&request->in_thread, __ATOMIC_ACQUIRE))
            return NULL;
        if (host.release_shared_irq(irq, request->handle))
            return NULL;
        if (request->thread_handler)
            (void)cancel_work_sync(&request->thread_work);
        const char *name = request->name;
        *entry = request->next;
        kfree(request);
        return name;
    }
    return NULL;
}

static int managed(struct device *device, void (*release)(void *), void *data)
{
    if (!device || device->resource_count >= LUME_LINUX_DEVRES_LIMIT)
        return -ENOSPC;
    unsigned index = device->resource_count++;
    device->resources[index].release = release;
    device->resources[index].data = data;
    return 0;
}

static bool detach_managed(struct device *device, void (*release)(void *), void *data)
{
    for (unsigned i = 0; i < device->resource_count; ++i)
        if (device->resources[i].release == release && device->resources[i].data == data)
        {
            for (unsigned j = i + 1; j < device->resource_count; ++j)
                device->resources[j - 1] = device->resources[j];
            --device->resource_count;
            memset(device->resources + device->resource_count, 0, sizeof(device->resources[0]));
            return true;
        }
    return false;
}

struct managed_interrupt
{
    unsigned irq;
    void *device;
};

static void release_managed_interrupt(void *data)
{
    struct managed_interrupt *request = data;
    (void)free_irq(request->irq, request->device);
    kfree(request);
}

int devm_request_threaded_irq(struct device *owner, unsigned irq, irq_handler_t handler,
                              irq_handler_t thread_handler, unsigned long flags, const char *name,
                              void *device)
{
    if (!owner)
        return -EINVAL;
    if (owner->resource_count >= LUME_LINUX_DEVRES_LIMIT)
        return -ENOSPC;
    struct managed_interrupt *request = kmalloc(sizeof(*request), GFP_KERNEL);
    if (!request)
        return -ENOMEM;
    *request = (struct managed_interrupt){.irq = irq, .device = device};
    int result = managed(owner, release_managed_interrupt, request);
    if (result)
    {
        kfree(request);
        return result;
    }
    result = request_threaded_irq(irq, handler, thread_handler, flags, name, device);
    if (result && detach_managed(owner, release_managed_interrupt, request))
        kfree(request);
    return result;
}

void devm_free_irq(struct device *owner, unsigned irq, void *device)
{
    if (!owner)
        return;
    for (unsigned i = 0; i < owner->resource_count; ++i)
    {
        struct managed_interrupt *request = owner->resources[i].data;
        if (owner->resources[i].release != release_managed_interrupt || request->irq != irq ||
            request->device != device)
            continue;
        if (detach_managed(owner, release_managed_interrupt, request))
            release_managed_interrupt(request);
        return;
    }
}

struct streaming_mapping
{
    struct streaming_mapping *next;
    struct device *device;
    void *buffer, *allocation;
    dma_addr_t bus;
    size_t bytes;
    enum dma_data_direction direction;
};

static struct streaming_mapping *streaming_mappings;

static bool valid_dma_mask(u64 mask)
{
    return mask && !(mask & (mask + 1));
}

int dma_set_mask(struct device *device, u64 mask)
{
    if (!device || !valid_dma_mask(mask))
        return -EINVAL;
    if (!started || !host.streaming_address || !host.dma_mask_supported(mask))
        return -EIO;
    device->dma_mask = mask;
    return 0;
}

dma_addr_t dma_map_single(struct device *device, void *buffer, size_t bytes,
                          enum dma_data_direction direction)
{
    if (!started || !host.streaming_address || in_irq || !device || !buffer || !bytes ||
        bytes > UINTPTR_MAX - (uintptr_t)buffer || (unsigned)direction >= DMA_NONE)
        return DMA_MAPPING_ERROR;
    uintptr_t start = (uintptr_t)buffer, end = start + bytes;
    for (const struct streaming_mapping *item = streaming_mappings; item; item = item->next)
        if (start < (uintptr_t)item->buffer + item->bytes && (uintptr_t)item->buffer < end)
            return DMA_MAPPING_ERROR;
    void *owner = lume_linux_memory_dma_hold(buffer, bytes);
    if (!owner)
        return DMA_MAPPING_ERROR;
    u64 mask = device->dma_mask ? device->dma_mask : DMA_BIT_MASK(32);
    dma_addr_t bus;
    struct streaming_mapping *item = kmalloc(sizeof(*item), GFP_KERNEL);
    if (!item || !host.streaming_address(buffer, bytes, mask, &bus) || bus == DMA_MAPPING_ERROR ||
        bus > mask || bytes - 1 > mask - bus ||
        !host.streaming_sync(buffer, bytes, direction, false))
    {
        kfree(item);
        lume_linux_memory_dma_release(owner);
        return DMA_MAPPING_ERROR;
    }
    *item = (struct streaming_mapping){.next = streaming_mappings,
                                       .device = device,
                                       .buffer = buffer,
                                       .allocation = owner,
                                       .bus = bus,
                                       .bytes = bytes,
                                       .direction = direction};
    streaming_mappings = item;
    return bus;
}

void dma_unmap_single(struct device *device, dma_addr_t bus, size_t bytes,
                      enum dma_data_direction direction)
{
    if (in_irq)
        return;
    for (struct streaming_mapping **entry = &streaming_mappings; *entry; entry = &(*entry)->next)
    {
        struct streaming_mapping *item = *entry;
        if (item->device != device || item->bus != bus || item->bytes != bytes ||
            item->direction != direction)
            continue;
        if (host.streaming_sync(item->buffer, bytes, direction, true))
        {
            *entry = item->next;
            lume_linux_memory_dma_release(item->allocation);
            kfree(item);
        }
        return;
    }
}

static void sync_streaming(struct device *device, dma_addr_t bus, size_t bytes,
                           enum dma_data_direction direction, bool for_cpu)
{
    if (!bytes)
        return;
    for (const struct streaming_mapping *item = streaming_mappings; item; item = item->next)
        if (item->device == device && item->direction == direction && bus >= item->bus &&
            bus - item->bus < item->bytes && bytes <= item->bytes - (bus - item->bus))
        {
            (void)host.streaming_sync((const unsigned char *)item->buffer + (bus - item->bus),
                                      bytes, direction, for_cpu);
            return;
        }
}

void dma_sync_single_for_cpu(struct device *device, dma_addr_t bus, size_t bytes,
                             enum dma_data_direction direction)
{
    sync_streaming(device, bus, bytes, direction, true);
}

void dma_sync_single_for_device(struct device *device, dma_addr_t bus, size_t bytes,
                                enum dma_data_direction direction)
{
    sync_streaming(device, bus, bytes, direction, false);
}

struct coherent_allocation
{
    struct coherent_allocation *next;
    struct device *device;
    void *cpu;
    dma_addr_t bus;
    size_t bytes, span;
    bool managed;
};

static struct coherent_allocation *coherent_allocations;

int dma_set_coherent_mask(struct device *device, u64 mask)
{
    if (!device || !valid_dma_mask(mask))
        return -EINVAL;
    if (!started || !host.coherent_allocate || !host.dma_mask_supported(mask))
        return -EIO;
    device->coherent_dma_mask = mask;
    return 0;
}

static void coherent_release(void *data)
{
    struct coherent_allocation *allocation = data;
    struct coherent_allocation **link = &coherent_allocations;
    while (*link != allocation)
        link = &(*link)->next;
    *link = allocation->next;
    host.coherent_free(allocation->cpu, allocation->span);
    kfree(allocation);
}

static void *coherent_allocate(struct device *device, size_t bytes, dma_addr_t *handle, gfp_t flags,
                               bool automatic)
{
    if (!started || !host.coherent_allocate || !device || !bytes || !handle ||
        flags != GFP_KERNEL || (automatic && device->resource_count >= LUME_LINUX_DEVRES_LIMIT))
        return NULL;
    size_t span = 4096;
    while (span < bytes)
    {
        if (span > SIZE_MAX / 2)
            return NULL;
        span *= 2;
    }
    u64 mask = device->coherent_dma_mask ? device->coherent_dma_mask : DMA_BIT_MASK(32);
    if (!host.dma_mask_supported(mask))
        return NULL;
    struct coherent_allocation *allocation = kzalloc(sizeof(*allocation), GFP_KERNEL);
    if (!allocation)
        return NULL;
    dma_addr_t bus = 0;
    void *cpu = host.coherent_allocate(span, mask, &bus);
    if (!cpu || ((uintptr_t)cpu & (span - 1)) || (bus & (span - 1)) || bus > mask ||
        span - 1 > mask - bus)
    {
        if (cpu)
            host.coherent_free(cpu, span);
        kfree(allocation);
        return NULL;
    }
    memset(cpu, 0, span);
    *allocation = (struct coherent_allocation){.next = coherent_allocations,
                                               .device = device,
                                               .cpu = cpu,
                                               .bus = bus,
                                               .bytes = bytes,
                                               .span = span,
                                               .managed = automatic};
    coherent_allocations = allocation;
    if (automatic && managed(device, coherent_release, allocation))
    {
        coherent_release(allocation);
        return NULL;
    }
    *handle = bus;
    return cpu;
}

static void coherent_free(struct device *device, size_t bytes, void *cpu, dma_addr_t bus,
                          bool automatic)
{
    for (struct coherent_allocation *item = coherent_allocations; item; item = item->next)
        if (item->device == device && item->bytes == bytes && item->cpu == cpu &&
            item->bus == bus && item->managed == automatic)
        {
            if (!automatic || detach_managed(device, coherent_release, item))
                coherent_release(item);
            return;
        }
}

void *dma_alloc_coherent(struct device *device, size_t bytes, dma_addr_t *handle, gfp_t flags)
{
    return coherent_allocate(device, bytes, handle, flags, false);
}

void dma_free_coherent(struct device *device, size_t bytes, void *cpu, dma_addr_t handle)
{
    coherent_free(device, bytes, cpu, handle, false);
}

void *dmam_alloc_coherent(struct device *device, size_t bytes, dma_addr_t *handle, gfp_t flags)
{
    return coherent_allocate(device, bytes, handle, flags, true);
}

void dmam_free_coherent(struct device *device, size_t bytes, void *cpu, dma_addr_t handle)
{
    coherent_free(device, bytes, cpu, handle, true);
}

static void free_managed(void *memory)
{
    kfree(memory);
}

void *devm_kzalloc(struct device *device, size_t bytes, gfp_t flags)
{
    if (!device)
        return NULL;
    void *memory = kzalloc(bytes, flags);
    if (memory && managed(device, free_managed, memory))
    {
        kfree(memory);
        return NULL;
    }
    return memory;
}

struct mapping
{
    void *address;
    size_t bytes;
};

static void unmap_managed(void *data)
{
    struct mapping *mapping = data;
    host.unmap(mapping->address, mapping->bytes);
    kfree(mapping);
}

void *devm_platform_ioremap_resource(struct platform_device *device, unsigned index)
{
    if (!started || !device || index >= device->num_resources)
        return ERR_PTR(-EINVAL);
    const struct resource *resource = device->resource + index;
    if (!resource->size || resource->size > SIZE_MAX ||
        resource->start > UINT64_MAX - resource->size)
        return ERR_PTR(-EINVAL);
    struct mapping *mapping = kzalloc(sizeof(*mapping), GFP_KERNEL);
    if (!mapping)
        return ERR_PTR(-ENOMEM);
    mapping->bytes = (size_t)resource->size;
    mapping->address = host.map(resource->start, mapping->bytes);
    if (!mapping->address || IS_ERR(mapping->address))
    {
        int error = mapping->address ? (int)PTR_ERR(mapping->address) : -ENOMEM;
        kfree(mapping);
        return ERR_PTR(error);
    }
    int result = managed(&device->dev, unmap_managed, mapping);
    if (result)
    {
        unmap_managed(mapping);
        return ERR_PTR(result);
    }
    return mapping->address;
}

int lume_linux_device_release(struct device *device)
{
    if (!device)
        return -EINVAL;
    if (reading)
        return -EBUSY;
    while (device->resource_count)
    {
        unsigned index = --device->resource_count;
        void (*release)(void *) = device->resources[index].release;
        void *data = device->resources[index].data;
        memset(device->resources + index, 0, sizeof(device->resources[index]));
        release(data);
    }
    device->driver_data = NULL;
    return 0;
}

int lume_linux_platform_probe(struct platform_driver *driver, struct platform_device *device)
{
    if (!started || !driver || !driver->probe || !device || device->dev.resource_count)
        return -EINVAL;
    int result = driver->probe(device);
    if (result)
        (void)lume_linux_device_release(&device->dev);
    return result;
}

u32 lume_linux_ioread32(const volatile void *address)
{
    return host.read32(address);
}

void lume_linux_iowrite32(u32 value, volatile void *address)
{
    host.write32(value, address);
}

unsigned long lume_linux_jiffies(void)
{
    return (unsigned long)(host.microseconds() / 1000);
}

uint64_t lume_linux_microseconds(void)
{
    return host.microseconds();
}

void usleep_range(unsigned long minimum, unsigned long maximum)
{
    if (minimum && maximum >= minimum && minimum <= UINT32_MAX)
        pause_us((unsigned)minimum);
}

unsigned long lume_linux_irq_save(void)
{
    return host.irq_save();
}

void lume_linux_irq_restore(unsigned long flags)
{
    host.irq_restore(flags);
}

void lume_linux_relax(void)
{
    host.relax();
}

void lume_linux_dev_log(struct device *device, const char *message)
{
    if (host.log)
        host.log(device && device->name ? device->name : "linux", message);
}

void init_completion(struct completion *completion)
{
    __atomic_store_n(&completion->done, 0u, __ATOMIC_RELAXED);
}

void reinit_completion(struct completion *completion)
{
    __atomic_store_n(&completion->done, 0u, __ATOMIC_RELEASE);
}

void complete(struct completion *completion)
{
    __atomic_fetch_add(&completion->done, 1u, __ATOMIC_RELEASE);
}

static int completion_result(uint64_t start, int result)
{
    statistics.completion_microseconds += host.microseconds() - start;
    statistics.completion_timeouts += result == -ETIMEDOUT;
    return result;
}

int wait_for_completion_killable(struct completion *completion)
{
    if (!started || !completion)
        return -EINVAL;
    const unsigned timeout = 5000000;
    uint64_t start = host.microseconds();
    ++statistics.completion_waits;
    for (;;)
    {
        ++statistics.completion_polls;
        unsigned done = __atomic_load_n(&completion->done, __ATOMIC_ACQUIRE);
        if (done && __atomic_compare_exchange_n(&completion->done, &done, done - 1, false,
                                                __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
            return completion_result(start, 0);
        if (poll_device)
        {
            int result = lume_linux_virtio_poll(poll_device);
            if (result < 0)
                return completion_result(start, result);
            if (result > 0)
                continue;
        }
        uint64_t elapsed = host.microseconds() - start;
        if (elapsed >= timeout)
        {
            if (poll_device)
                poll_device->error = -ETIMEDOUT;
            return completion_result(start, -ETIMEDOUT);
        }
        unsigned remaining = timeout - (unsigned)elapsed;
        if (poll_device && poll_device->queue.pending && poll_device->transport->wait &&
            poll_device->transport->wait(poll_device->transport_context, min(1000u, remaining)))
            continue;
        pause_us(min(50u, remaining));
    }
}

int ida_alloc(struct ida *ida, gfp_t flags)
{
    if (!ida || flags != GFP_KERNEL)
        return -EINVAL;
    for (unsigned i = 0; i < 32; ++i)
    {
        uint32_t mask = UINT32_C(1) << i;
        if (!(ida->allocated & mask))
        {
            ida->allocated |= mask;
            return (int)i;
        }
    }
    return -ENOSPC;
}

void ida_free(struct ida *ida, unsigned index)
{
    if (ida && index < 32)
        ida->allocated &= ~(UINT32_C(1) << index);
}

int lume_linux_sprintf(char *out, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    unsigned written = 0;
    while (*format)
    {
        if (*format != '%')
        {
            out[written++] = *format++;
            continue;
        }
        ++format;
        if (*format == '%')
            out[written++] = '%';
        else if (*format == 'd')
        {
            int value = va_arg(args, int);
            unsigned magnitude = value < 0 ? 0u - (unsigned)value : (unsigned)value;
            char digits[10];
            unsigned count = 0;
            if (value < 0)
                out[written++] = '-';
            do
            {
                digits[count++] = (char)('0' + magnitude % 10);
                magnitude /= 10;
            } while (magnitude);
            while (count)
                out[written++] = digits[--count];
        }
        else
        {
            out[written] = 0;
            va_end(args);
            return -EINVAL;
        }
        ++format;
    }
    out[written] = 0;
    va_end(args);
    return (int)written;
}

int hwrng_register(struct hwrng *rng)
{
    if (!started || !rng || !rng->name || !rng->read)
        return -EINVAL;
    if (active_rng)
        return -EBUSY;
    int result = rng->init ? rng->init(rng) : 0;
    if (!result)
        active_rng = rng;
    return result;
}

void hwrng_unregister(struct hwrng *rng)
{
    if (active_rng != rng || reading)
        return;
    active_rng = NULL;
    if (rng->cleanup)
        rng->cleanup(rng);
}

static void unregister_managed(void *rng)
{
    hwrng_unregister(rng);
}

int devm_hwrng_register(struct device *device, struct hwrng *rng)
{
    int result = hwrng_register(rng);
    if (!result)
    {
        result = managed(device, unregister_managed, rng);
        if (result)
            hwrng_unregister(rng);
    }
    return result;
}

void devm_hwrng_unregister(struct device *device, struct hwrng *rng)
{
    if (device && !reading && detach_managed(device, unregister_managed, rng))
        hwrng_unregister(rng);
}

bool lume_linux_hwrng_available(void)
{
    return active_rng != NULL;
}

int lume_linux_hwrng_read(void *out, size_t bytes, bool wait)
{
    if (!active_rng || !out || !bytes || bytes > 4096)
        return -EINVAL;
    if (reading)
        return -EBUSY;
    uint64_t start = host.microseconds();
    ++statistics.entropy_reads;
    reading = true;
    size_t copied = 0;
    int result = 0;
    _Alignas(64) unsigned char buffer[64];
    while (copied < bytes)
    {
        if (poll_device && (result = lume_linux_virtio_poll(poll_device)) < 0)
            break;
        unsigned request = (unsigned)min_t(size_t, bytes - copied, sizeof(buffer));
        request = (request + 3u) & ~3u;
        if (request < 32)
            request = 32;
        memset(buffer, 0, sizeof(buffer));
        result = active_rng->read(active_rng, buffer, request, wait);
        if (result <= 0)
            break;
        if ((unsigned)result > request)
        {
            result = -EIO;
            break;
        }
        size_t take = min_t(size_t, (unsigned)result, bytes - copied);
        memcpy((unsigned char *)out + copied, buffer, take);
        copied += take;
        if (!wait)
            break;
    }
    memzero_explicit(buffer, sizeof(buffer));
    reading = false;
    statistics.entropy_microseconds += host.microseconds() - start;
    if (result < 0)
    {
        memset(out, 0, bytes);
        return result;
    }
    statistics.entropy_bytes += copied;
    return (int)copied;
}

static void delete_queues(struct virtio_device *device)
{
    if (device->reset && device->queue.vdev)
    {
        device->transport->destroy(device->transport_context);
        memset(&device->queue, 0, sizeof(device->queue));
    }
}

static const struct virtio_config_ops config = {.del_vqs = delete_queues};

struct virtqueue *virtio_find_single_vq(struct virtio_device *device,
                                        void (*callback)(struct virtqueue *), const char *name)
{
    (void)name;
    if (!device || !callback || device->queue.vdev)
        return ERR_PTR(-EINVAL);
    int result = device->transport->create(device->transport_context);
    if (result)
        return ERR_PTR(result);
    device->queue = (struct virtqueue){.vdev = device, .callback = callback};
    device->reset = false;
    return &device->queue;
}

static int submit_scatterlists(struct virtqueue *queue, struct scatterlist *const *lists,
                               unsigned out, unsigned in, unsigned expected, void *token,
                               gfp_t flags)
{
    if (!queue || !queue->vdev || !lists || (!out && !in) || !token || in_irq ||
        (flags != GFP_KERNEL && flags != GFP_ATOMIC))
        return -EINVAL;
    struct virtio_device *device = queue->vdev;
    if (device->error)
        return device->error;
    if (device->reset)
        return -EBUSY;
    const struct lume_linux_virtio_ops *transport = device->transport;
    if (queue->pending + queue->ready == transport->max_pending)
        return -ENOSPC;
    if (flags == GFP_ATOMIC && !transport->atomic_submit)
        return -EOPNOTSUPP;
    unsigned limit = transport->max_segments;
    if (out > limit || in > limit - out || expected > limit)
        return -ENOSPC;
    unsigned segments = 0, writable = 0;
    for (unsigned list = 0; list < out + in; ++list)
    {
        if (!lists[list])
            return -EINVAL;
        for (struct scatterlist *sg = lists[list]; sg; sg = sg_next(sg))
        {
            if (segments == limit)
                return -ENOSPC;
            ++segments;
            if (!sg->buffer || !sg->length || sg->length > UINTPTR_MAX - (uintptr_t)sg->buffer)
                return -EINVAL;
            if (list >= out)
            {
                if (sg->length > UINT32_MAX - writable)
                    return -EINVAL;
                writable += sg->length;
            }
            if (expected && segments == expected && !sg->last)
                return -EINVAL;
        }
    }
    if (expected && segments != expected)
        return -EINVAL;
    unsigned slot = 0;
    while (queue->slots[slot].token)
        ++slot;
    int result =
        transport->submit(device->transport_context, slot, lists, out, in, segments, flags);
    if (result)
    {
        if (result == -EIO)
            device->error = result;
        return result;
    }
    queue->slots[slot] = (struct lume_linux_virtqueue_slot){.token = token, .writable = writable};
    queue->staged |= UINT32_C(1) << slot;
    ++queue->pending;
    return 0;
}

int virtqueue_add_inbuf(struct virtqueue *queue, struct scatterlist *sg, unsigned count,
                        void *token, gfp_t flags)
{
    return count ? submit_scatterlists(queue, &sg, 0, 1, count, token, flags) : -EINVAL;
}

int virtqueue_add_outbuf(struct virtqueue *queue, struct scatterlist *sg, unsigned count,
                         void *token, gfp_t flags)
{
    return count ? submit_scatterlists(queue, &sg, 1, 0, count, token, flags) : -EINVAL;
}

int virtqueue_add_sgs(struct virtqueue *queue, struct scatterlist *lists[], unsigned out,
                      unsigned in, void *token, gfp_t flags)
{
    return submit_scatterlists(queue, lists, out, in, 0, token, flags);
}

bool virtqueue_kick(struct virtqueue *queue)
{
    if (!queue || !queue->vdev || !queue->pending || queue->vdev->error)
        return false;
    uint32_t staged = queue->staged;
    queue->staged = 0;
    if (staged)
        queue->vdev->transport->notify(queue->vdev->transport_context, staged);
    return !queue->vdev->error;
}

void *virtqueue_get_buf(struct virtqueue *queue, unsigned *length)
{
    if (!queue || !length || !queue->ready)
        return NULL;
    struct lume_linux_virtqueue_slot *slot = &queue->slots[queue->completed[queue->next]];
    void *token = slot->token;
    *length = slot->written;
    slot->token = NULL;
    queue->next = (queue->next + 1) % queue->vdev->transport->max_pending;
    --queue->ready;
    return token;
}

void virtio_device_ready(struct virtio_device *device)
{
    device->transport->ready(device->transport_context);
}

static int reset_binding(struct virtio_device *device)
{
    if (!device->reset)
    {
        int result = device->transport->reset(device->transport_context);
        if (result)
            return result;
        device->reset = true;
        struct virtqueue *queue = &device->queue;
        *queue = (struct virtqueue){.vdev = queue->vdev, .callback = queue->callback};
    }
    return 0;
}

void virtio_reset_device(struct virtio_device *device)
{
    int result = reset_binding(device);
    if (result)
        device->error = result;
}

int lume_linux_virtio_poll(struct virtio_device *device)
{
    if (!device || device->error)
        return device ? device->error : -EINVAL;
    struct virtqueue *queue = &device->queue;
    if (!queue->pending || device->reset)
        return 0;
    unsigned id, bytes;
    int result = device->transport->take(device->transport_context, &id, &bytes);
    if (result <= 0)
    {
        if (result < 0)
            device->error = result;
        return result;
    }
    unsigned count = device->transport->max_pending;
    if (id >= count || (queue->staged & (UINT32_C(1) << id)) || !queue->slots[id].token ||
        queue->slots[id].completed || bytes > queue->slots[id].writable)
    {
        device->error = -EIO;
        return device->error;
    }
    queue->slots[id].written = bytes;
    queue->slots[id].completed = true;
    queue->completed[(queue->next + queue->ready) % count] = id;
    --queue->pending;
    ++queue->ready;
    queue->callback(queue);
    return 1;
}

static void release_binding(struct virtio_device *device)
{
    delete_queues(device);
    device->probed = false;
    device->priv = NULL;
    device->transport = NULL;
    device->transport_context = NULL;
}

int lume_linux_virtio_attach(struct virtio_device *device,
                             const struct lume_linux_virtio_ops *transport, void *context)
{
    if (!device || device->priv || device->transport || !transport || !transport->create ||
        !transport->max_segments || !transport->max_pending ||
        transport->max_pending > LUME_LINUX_VIRTQUEUE_SLOTS || !transport->submit ||
        !transport->notify || !transport->take || !transport->ready || !transport->reset ||
        !transport->destroy)
        return -EINVAL;
    device->config = &config;
    device->transport = transport;
    device->transport_context = context;
    device->error = 0;
    device->reset = true;
    return 0;
}

int lume_linux_virtio_detach(struct virtio_device *device)
{
    if (!device || !device->transport || device == poll_device)
        return -EINVAL;
    int result = reset_binding(device);
    if (!result)
        release_binding(device);
    return result;
}

int lume_linux_virtio_remove(struct virtio_driver *driver, struct virtio_device *device)
{
    if (!driver || !device || reading)
        return -EBUSY;
    if (!device->transport)
        return 0;
    if (device != poll_device)
        return -EINVAL;
    /* Reset must succeed before the Linux remove callback can free DMA-owned state. */
    int result = reset_binding(device);
    if (result)
        return result;
    if (device->probed)
        driver->remove(device);
    release_binding(device);
    poll_device = NULL;
    return 0;
}

int lume_linux_virtio_probe(struct virtio_driver *driver, struct virtio_device *device,
                            const struct lume_linux_virtio_ops *transport, void *context)
{
    if (!started || poll_device || active_rng || !driver || !driver->probe || !driver->remove)
        return -EINVAL;
    int result = lume_linux_virtio_attach(device, transport, context);
    if (result)
        return result;
    poll_device = device;
    result = driver->probe(device);
    if (!result)
    {
        device->probed = true;
        if (driver->scan)
            driver->scan(device);
        result = device->error;
    }
    if (result)
    {
        int removed = lume_linux_virtio_remove(driver, device);
        return removed ? removed : result;
    }
    return 0;
}
