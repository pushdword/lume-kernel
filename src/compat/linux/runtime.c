/* SPDX-License-Identifier: GPL-2.0-only */
#include <lume/linux_compat.h>
#include <stdarg.h>

static struct lume_linux_host_ops host;
static bool started, reading;
static unsigned allocations;
static struct hwrng *active_rng;
static struct virtio_device *poll_device;

struct allocation
{
    void *base;
    size_t bytes;
};

int lume_linux_runtime_start(const struct lume_linux_host_ops *ops)
{
    if (started)
        return -EBUSY;
    if (!ops || !ops->allocate || !ops->free || !ops->microseconds || !ops->pause || !ops->relax ||
        !ops->irq_save || !ops->irq_restore || !ops->map || !ops->unmap || !ops->read32 ||
        !ops->write32)
        return -EINVAL;
    host = *ops;
    started = true;
    return 0;
}

int lume_linux_runtime_stop(void)
{
    if (allocations || active_rng || poll_device || reading)
        return -EBUSY;
    memset(&host, 0, sizeof(host));
    started = false;
    return 0;
}

unsigned lume_linux_allocations(void)
{
    return allocations;
}

void *kmalloc(size_t bytes, gfp_t flags)
{
    if (!started || flags != GFP_KERNEL || !bytes ||
        bytes > SIZE_MAX - sizeof(struct allocation) - 63)
        return NULL;
    void *base = host.allocate(bytes + sizeof(struct allocation) + 63);
    if (!base)
        return NULL;
    uintptr_t address = ((uintptr_t)base + sizeof(struct allocation) + 63) & ~(uintptr_t)63;
    struct allocation *record = (struct allocation *)address - 1;
    *record = (struct allocation){base, bytes};
    ++allocations;
    return (void *)address;
}

void *kzalloc(size_t bytes, gfp_t flags)
{
    void *memory = kmalloc(bytes, flags);
    if (memory)
        memset(memory, 0, bytes);
    return memory;
}

void kfree(const void *memory)
{
    if (!memory)
        return;
    const struct allocation *record = (const struct allocation *)memory - 1;
    void *base = record->base;
    volatile unsigned char *bytes = (volatile unsigned char *)memory;
    for (size_t i = 0; i < record->bytes; ++i)
        bytes[i] = 0;
    --allocations;
    host.free(base);
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

void usleep_range(unsigned long minimum, unsigned long maximum)
{
    if (minimum && maximum >= minimum && minimum <= UINT32_MAX)
        host.pause((unsigned)minimum);
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

int wait_for_completion_killable(struct completion *completion)
{
    if (!started || !completion)
        return -EINVAL;
    uint64_t start = host.microseconds();
    for (;;)
    {
        unsigned done = __atomic_load_n(&completion->done, __ATOMIC_ACQUIRE);
        if (done && __atomic_compare_exchange_n(&completion->done, &done, done - 1, false,
                                                __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
            return 0;
        if (poll_device)
        {
            int result = lume_linux_virtio_poll(poll_device);
            if (result < 0)
                return result;
        }
        if (host.microseconds() - start >= UINT64_C(5000000))
        {
            if (poll_device)
                poll_device->error = -ETIMEDOUT;
            return -ETIMEDOUT;
        }
        host.pause(50);
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
    if (!device || reading)
        return;
    for (unsigned i = 0; i < device->resource_count; ++i)
        if (device->resources[i].release == unregister_managed && device->resources[i].data == rng)
        {
            hwrng_unregister(rng);
            for (unsigned j = i + 1; j < device->resource_count; ++j)
                device->resources[j - 1] = device->resources[j];
            --device->resource_count;
            return;
        }
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
    volatile unsigned char *wipe = buffer;
    for (unsigned i = 0; i < sizeof(buffer); ++i)
        wipe[i] = 0;
    reading = false;
    if (result < 0)
    {
        memset(out, 0, bytes);
        return result;
    }
    return (int)copied;
}

static void delete_queues(struct virtio_device *device)
{
    if (device->reset)
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

int virtqueue_add_inbuf(struct virtqueue *queue, struct scatterlist *sg, unsigned count,
                        void *token, gfp_t flags)
{
    if (!queue || !queue->vdev || !sg || count != 1 || !sg->buffer || !sg->length || !token ||
        flags != GFP_KERNEL)
        return -EINVAL;
    struct virtio_device *device = queue->vdev;
    if (device->error || queue->pending || queue->ready || device->reset)
        return device->error ? device->error : -EBUSY;
    int result = device->transport->submit(device->transport_context, sg->buffer, sg->length);
    if (result)
    {
        device->error = result;
        return result;
    }
    queue->token = token;
    queue->capacity = sg->length;
    queue->pending = true;
    return 0;
}

bool virtqueue_kick(struct virtqueue *queue)
{
    if (!queue || !queue->vdev || !queue->pending || queue->vdev->error)
        return false;
    queue->vdev->transport->notify(queue->vdev->transport_context);
    return true;
}

void *virtqueue_get_buf(struct virtqueue *queue, unsigned *length)
{
    if (!queue || !length || !queue->ready)
        return NULL;
    void *token = queue->token;
    *length = queue->completed;
    queue->token = NULL;
    queue->ready = false;
    return token;
}

void virtio_device_ready(struct virtio_device *device)
{
    device->transport->ready(device->transport_context);
}

void virtio_reset_device(struct virtio_device *device)
{
    if (!device->reset)
    {
        int result = device->transport->reset(device->transport_context);
        if (result)
            device->error = result;
        else
        {
            device->reset = true;
            device->queue.pending = false;
        }
    }
}

int lume_linux_virtio_poll(struct virtio_device *device)
{
    if (!device || device->error)
        return device ? device->error : -EINVAL;
    struct virtqueue *queue = &device->queue;
    if (!queue->pending || device->reset)
        return 0;
    unsigned bytes = 0;
    int result = device->transport->take(device->transport_context, &bytes);
    if (result < 0 || bytes > queue->capacity)
    {
        device->error = result < 0 ? result : -EIO;
        return device->error;
    }
    if (!result)
        return 0;
    queue->completed = bytes;
    queue->pending = false;
    queue->ready = true;
    queue->callback(queue);
    return 1;
}

int lume_linux_virtio_remove(struct virtio_driver *driver, struct virtio_device *device)
{
    if (!driver || !device || reading)
        return -EBUSY;
    if (!device->priv)
        return 0;
    /* Reset must succeed before the Linux remove callback can free DMA-owned state. */
    if (!device->reset)
    {
        int result = device->transport->reset(device->transport_context);
        if (result)
            return result;
        device->reset = true;
        device->queue.pending = false;
    }
    driver->remove(device);
    device->priv = NULL;
    device->transport = NULL;
    device->transport_context = NULL;
    poll_device = NULL;
    return 0;
}

int lume_linux_virtio_probe(struct virtio_driver *driver, struct virtio_device *device,
                            const struct lume_linux_virtio_ops *transport, void *context)
{
    if (!started || poll_device || active_rng || !driver || !driver->probe || !driver->remove ||
        !driver->scan || !device || device->priv || !transport || !transport->create ||
        !transport->submit || !transport->notify || !transport->take || !transport->ready ||
        !transport->reset || !transport->destroy)
        return -EINVAL;
    device->config = &config;
    device->transport = transport;
    device->transport_context = context;
    device->error = 0;
    device->reset = true;
    poll_device = device;
    int result = driver->probe(device);
    if (result)
    {
        device->priv = NULL;
        poll_device = NULL;
        return result;
    }
    driver->scan(device);
    if (!active_rng || device->error)
    {
        result = device->error ? device->error : -ENODEV;
        int removed = lume_linux_virtio_remove(driver, device);
        return removed ? removed : result;
    }
    return 0;
}
