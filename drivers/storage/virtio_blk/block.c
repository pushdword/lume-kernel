#include "lume/virtio.h"
#if !defined(LUME_ARCH_I686)
#include "lume/q35_module.h"
#else
#include "lume/q35_binding.h"
#endif
#include "lume/platform_driver.h"
#include "lume/sd.h"
#include "lume/storage.h"
#include "lume/pc_disk.h"
#include "lume/worker.h"
#include "lume/counter.h"
#include "lume/cpu.h"
#include <string.h>
#if defined(LUME_LINUX_VIRTIO_BLOCK)
#include <lume/linux_compat.h>
#include <linux/err.h>
#endif
static struct lume_virtio device;
static struct lume_virtqueue queue;
static uint64_t capacity, allowed_first, allowed_count;
static struct lume_volume journal_volume;

#define BLOCK_REQUESTS 4u
#define BLOCK_REQUEST_SECTORS LUME_SD_MAX_SECTORS
#define READ_CACHE_SECTORS 128u

static unsigned char read_cache[READ_CACHE_SECTORS * 512u];
static _Alignas(16) unsigned char request_data[LUME_SD_MAX_SECTORS * 512u];
static uint64_t read_cache_first;
static unsigned read_cache_count;

_Static_assert(BLOCK_REQUESTS *BLOCK_REQUEST_SECTORS >= LUME_SD_MAX_SECTORS,
               "Block request slots must cover a complete transfer");
_Static_assert(READ_CACHE_SECTORS == LUME_SD_MAX_SECTORS &&
                   !(READ_CACHE_SECTORS & (READ_CACHE_SECTORS - 1u)),
               "Read cache must cover one power-of-two transfer window");

static struct block_request
{
    struct
    {
        uint32_t type, reserved;
        uint64_t sector;
    } header;

    unsigned char *data;
    volatile unsigned char status;
    unsigned expected;
    bool active, done, success;
} requests[BLOCK_REQUESTS];

static unsigned request_limit;
static unsigned request_sectors;
static bool transferring, dirty;
static uint64_t submitted_requests;

static void finish(struct block_request *request, unsigned written)
{
    request->done = true;
    request->success = !request->status && written == request->expected;
}

#if defined(LUME_LINUX_VIRTIO_BLOCK)
static struct virtio_device linux_device;

static int linux_queue_create(void *context)
{
    (void)context;
    return lume_virtio_queue(&device, 0, &queue) ? 0 : -EIO;
}

static int linux_queue_submit(void *context, unsigned slot, struct scatterlist *const *lists,
                              unsigned out, unsigned in, unsigned segments, gfp_t flags)
{
    (void)context;
    (void)flags;
    unsigned head = slot * 3u;
    if (head >= queue.count || segments > queue.count - head)
        return -ENOSPC;
    unsigned index = head;
    for (unsigned i = 0; i < out + in; ++i)
    {
        for (struct scatterlist *segment = lists[i]; segment; segment = sg_next(segment))
        {
            /* Request storage is preallocated and identity mapped for DMA. */
            uint16_t next = index + 1 < head + segments ? (uint16_t)(index + 1) : 0;
            uint16_t access = i >= out ? 2 : 0;
            queue.descriptors[index++] = (struct lume_virtio_desc){
                (uintptr_t)segment->buffer, segment->length, access | (next ? 1u : 0u), next};
        }
    }
    return 0;
}

static void linux_queue_notify(void *context, uint32_t slots)
{
    (void)context;
    unsigned heads[BLOCK_REQUESTS], count = 0;
    for (unsigned slot = 0; slot < BLOCK_REQUESTS; ++slot)
        if (slots & (UINT32_C(1) << slot))
            heads[count++] = slot * 3u;
    lume_virtio_submit_batch(&queue, heads, count);
}

static int linux_queue_take(void *context, unsigned *slot, unsigned *written)
{
    (void)context;
    if ((uint16_t)(queue.used[1] - queue.consumed) > linux_device.queue.pending)
        return -EIO;
    unsigned id;
    if (!lume_virtio_take(&queue, &id, written))
        return 0;
    if (id % 3u || id >= request_limit * 3u)
        return -EIO;
    *slot = id / 3u;
    return 1;
}

static void linux_queue_ready(void *context)
{
    (void)context;
    lume_virtio_ready(&device);
}

static int linux_queue_reset(void *context)
{
    (void)context;
    return lume_virtio_reset(&device) ? 0 : -EIO;
}

static void linux_queue_destroy(void *context)
{
    (void)context;
    lume_virtio_queue_release(&queue);
}

static const struct lume_linux_virtio_ops linux_queue_ops = {
    .create = linux_queue_create,
    .submit = linux_queue_submit,
    .notify = linux_queue_notify,
    .take = linux_queue_take,
    .ready = linux_queue_ready,
    .reset = linux_queue_reset,
    .destroy = linux_queue_destroy,
    .max_segments = 3,
    .max_pending = BLOCK_REQUESTS,
    .atomic_submit = true,
};

static void linux_complete(struct virtqueue *completed_queue)
{
    unsigned written;
    struct block_request *request = virtqueue_get_buf(completed_queue, &written);
    if (request)
    {
        finish(request, written);
        if (request->success && submitted_requests == 1)
            lume_puts("LUME PC VIRTIO BLOCK SCATTERLIST ACTIVE\n");
    }
}

static int linux_submit_request(struct block_request *request, unsigned bytes, unsigned type)
{
    struct scatterlist header, data, status;
    struct scatterlist *lists[3];
    sg_init_one(&header, &request->header, sizeof(request->header));
    sg_init_one(&status, (const void *)&request->status, 1);
    unsigned out = 1, in = 0;
    lists[0] = &header;
    if (bytes)
    {
        sg_init_one(&data, request->data, bytes);
        lists[1] = &data;
        if (type == 0)
            ++in;
        else
            ++out;
    }
    lists[out + in++] = &status;
    return virtqueue_add_sgs(&linux_device.queue, lists, out, in, request, GFP_ATOMIC);
}
#endif

static bool submit(unsigned slot, unsigned type, uint64_t sector, unsigned count,
                   const void *buffer)
{
    struct block_request *request = requests + slot;
    unsigned bytes = count * 512u;
    request->header.type = type;
    request->header.sector = sector;
    request->data = request_data + (size_t)slot * request_sectors * 512u;
    request->status = 0xff;
    request->expected = type == 0 ? bytes + 1 : 1;
    request->active = true;
    request->done = request->success = false;
    if (type == 1)
        lume_worker_copy(request->data, buffer, bytes);
#if defined(LUME_LINUX_VIRTIO_BLOCK)
    if (linux_submit_request(request, bytes, type))
    {
        request->active = false;
        return false;
    }
#else
    unsigned head = slot * 3u;
    queue.descriptors[head] = (struct lume_virtio_desc){(uintptr_t)&request->header, 16, 1,
                                                        (uint16_t)(head + (type == 4 ? 2 : 1))};
    queue.descriptors[head + 1] = (struct lume_virtio_desc){
        (uintptr_t)request->data, bytes, type == 0 ? 3 : 1, (uint16_t)(head + 2)};
    queue.descriptors[head + 2] = (struct lume_virtio_desc){(uintptr_t)&request->status, 1, 2, 0};
#endif
    ++submitted_requests;
    return true;
}

static bool wait_requests(unsigned count)
{
    unsigned remaining = count;
    uint64_t start = lume_counter_read(), frequency = lume_counter_frequency();
    uint64_t pause = frequency / 1000u + (frequency % 1000u != 0);
#if !defined(LUME_LINUX_VIRTIO_BLOCK)
    bool success = true;
#endif
    while (remaining && !device.failed)
    {
#if defined(LUME_LINUX_VIRTIO_BLOCK)
        int progress = lume_linux_virtio_poll(&linux_device);
        if (progress < 0)
            break;
        if (progress && !--remaining)
        {
            for (unsigned i = 0; i < count; ++i)
                if (!requests[i].success)
                    return false;
            return true;
        }
#else
        unsigned id, written;
        while (lume_virtio_take(&queue, &id, &written))
        {
            if (id % 3u || id / 3u >= count || !requests[id / 3u].active || requests[id / 3u].done)
            {
                lume_virtio_fail(&device);
                return false;
            }
            struct block_request *request = requests + id / 3u;
            finish(request, written);
            success &= request->success;
            --remaining;
        }
        if (!remaining)
            return success;
#endif
        uint64_t at = lume_counter_read();
        if (at - start >= frequency * 5u)
            break;
        if (at - start < frequency / 10000u)
            lume_cpu_relax();
        else if (!lume_worker_pause(at + pause))
            lume_cpu_relax();
    }
    lume_virtio_fail(&device);
    return false;
}

static bool transfer(unsigned type, uint64_t sector, unsigned count, void *buffer)
{
    if (device.failed || !capacity || !request_limit || !request_sectors || transferring ||
        (type != 4 && (!buffer || !count || count > LUME_SD_MAX_SECTORS || sector >= capacity ||
                       count > capacity - sector)))
        return false;
    transferring = true;
    if (type == 1)
        read_cache_count = 0;
    bool success = true;
    unsigned first = 0;
    do
    {
        unsigned pending = 0, submitted = first;
#if !defined(LUME_LINUX_VIRTIO_BLOCK)
        unsigned heads[BLOCK_REQUESTS];
#endif
        do
        {
            unsigned part = count - submitted;
            if (part > request_sectors)
                part = request_sectors;
#if !defined(LUME_LINUX_VIRTIO_BLOCK)
            heads[pending] = pending * 3u;
#endif
            success =
                submit(pending, type, sector + submitted, part,
                       buffer ? (const unsigned char *)buffer + (size_t)submitted * 512u : NULL);
            if (!success)
                break;
            ++pending;
            submitted += part;
        } while (pending < request_limit && submitted < count);
        if (!pending)
            break;
        bool all_submitted = success;
#if defined(LUME_LINUX_VIRTIO_BLOCK)
        if (virtqueue_kick(&linux_device.queue))
            success = wait_requests(pending) && all_submitted;
        else
        {
            lume_virtio_fail(&device);
            success = false;
        }
#else
        lume_virtio_submit_batch(&queue, heads, pending);
        success = wait_requests(pending) && all_submitted;
#endif
        for (unsigned i = 0; i < pending; ++i)
        {
            unsigned part = count - first;
            if (part > request_sectors)
                part = request_sectors;
            if (success && type == 0)
                lume_worker_copy((unsigned char *)buffer + (size_t)first * 512u, requests[i].data,
                                 part * 512u);
            if (requests[i].done)
                requests[i].active = false;
            first += part;
        }
    } while (success && first < count);
    transferring = false;
    return success;
}

static bool flush(void)
{
    if (device.failed || !capacity || transferring)
        return false;
    if (!dirty)
        return true;
    if (!(device.features & (1u << 9)) || !transfer(4, 0, 0, NULL))
        return false;
    dirty = false;
    return true;
}

static bool cached_read(uint64_t sector, unsigned count, void *buffer)
{
    if (device.failed || transferring || !buffer || !count || count > LUME_SD_MAX_SECTORS ||
        sector >= capacity || count > capacity - sector)
        return false;
    if (read_cache_count >= count && sector >= read_cache_first &&
        sector - read_cache_first <= read_cache_count - count)
    {
        lume_worker_copy(buffer, read_cache + (size_t)(sector - read_cache_first) * 512u,
                         count * 512u);
        return true;
    }
    if (count >= 32)
        return transfer(0, sector, count, buffer);
    /* Metadata and executable-page reads share a bounded window. Writes invalidate it
     * before submission, including writes that later fail. */
    read_cache_count = 0;
    uint64_t first = sector & ~(uint64_t)(READ_CACHE_SECTORS - 1u);
    if (sector - first > READ_CACHE_SECTORS - count)
        first = sector;
    unsigned sectors =
        capacity - first < READ_CACHE_SECTORS ? (unsigned)(capacity - first) : READ_CACHE_SECTORS;
    if (transfer(0, first, sectors, read_cache))
    {
        read_cache_first = first;
        read_cache_count = sectors;
        lume_worker_copy(buffer, read_cache + (size_t)(sector - first) * 512u, count * 512u);
        return true;
    }
    /* A failed speculative read must not hide a readable requested range. */
    return transfer(0, sector, count, buffer);
}

static bool read_sector(uint64_t sector, void *buffer)
{
    return cached_read(sector, 1, buffer);
}

static bool write_allowed(uint64_t sector, unsigned count)
{
    struct lume_volume data = {allowed_first, allowed_count};
    return (device.features & (1u << 9)) && (lume_volume_contains(&data, sector, count) ||
                                             lume_volume_contains(&journal_volume, sector, count));
}

static bool read_blocks(uint64_t sector, unsigned count, void *buffer)
{
    if (!buffer || !count || count > LUME_SD_MAX_SECTORS || sector >= capacity ||
        count > capacity - sector)
        return false;
    if (!cached_read(sector, count, buffer))
        return false;
    lume_worker_checkpoint(count * 512);
    return true;
}

static bool write_device(uint64_t sector, unsigned count, const void *buffer)
{
    if (!buffer || !count || count > LUME_SD_MAX_SECTORS || sector >= capacity ||
        count > capacity - sector)
        return false;
    dirty = true;
    if (!transfer(1, sector, count, (void *)buffer))
        return false;
    lume_worker_checkpoint(count * 512);
    return true;
}

static bool write_deferred(uint64_t sector, unsigned count, const void *buffer)
{
    return write_allowed(sector, count) && write_device(sector, count, buffer);
}

static bool write_blocks(uint64_t sector, unsigned count, const void *buffer)
{
    return write_deferred(sector, count, buffer) && flush();
}

static bool write_sector(uint64_t sector, const void *buffer)
{
    if (!write_allowed(sector, 1))
        return false;
    dirty = true;
    return transfer(1, sector, 1, (void *)buffer) && flush();
}

static uint64_t sectors(void)
{
    return capacity;
}

static bool allow(uint64_t first, uint64_t count)
{
    if ((device.features & (1u << 5)) || !first || first >= capacity || !count ||
        count > capacity - first || allowed_count || !(device.features & (1u << 9)))
        return false;
    allowed_first = first;
    allowed_count = count;
    return true;
}

static bool allow_boot(uint64_t first, uint64_t count)
{
    (void)first;
    (void)count;
    return false;
}

static bool allow_journal(uint64_t first, uint64_t count)
{
    unsigned char mbr[512];
    struct lume_volume allowed;
    if (!allowed_count || journal_volume.count || !read_sector(0, mbr) ||
        !lume_volume_journal_select(mbr, capacity, &allowed) || first != allowed.first ||
        count != allowed.count)
        return false;
    journal_volume = allowed;
    return true;
}

static bool start(uint64_t frequency)
{
    (void)frequency;
    return capacity != 0 && !device.failed;
}

static bool configure(uint64_t base, uint64_t size, uint32_t irq)
{
    (void)base;
    (void)size;
    (void)irq;
    return false;
}

static bool reset_transport(void)
{
#if defined(LUME_LINUX_VIRTIO_BLOCK)
    if (linux_device.queue.vdev)
    {
        virtio_reset_device(&linux_device);
        return linux_device.reset;
    }
#endif
    return lume_virtio_reset(&device);
}

static bool stop(void)
{
    return (!dirty || flush()) && reset_transport();
}

static void stats(void)
{
    lume_value("VIRTIO_BLOCK_SECTORS", capacity);
    lume_value("VIRTIO_BLOCK_REQUEST_SECTORS", request_sectors);
    lume_value("VIRTIO_BLOCK_REQUESTS", submitted_requests);
}

static const struct lume_platform_sd_ops operations = {
    start,      configure, stop,        read_sector,  write_sector,   sectors, allow,
    allow_boot, stats,     read_blocks, write_blocks, write_deferred, flush,   allow_journal};

static struct lume_pc_disk raw_disk;

static int raw_read(struct lume_block *block, uint64_t sector, unsigned count, void *buffer)
{
    (void)block;
    return read_blocks(sector, count, buffer) ? 0 : -LUME_EIO;
}

static int raw_write(struct lume_block *block, uint64_t sector, unsigned count, const void *buffer)
{
    if (!lume_storage_is_installer() || block->read_only)
        return -LUME_EROFS;
    return write_device(sector, count, buffer) ? 0 : -LUME_EIO;
}

static int raw_sync(struct lume_block *block)
{
    (void)block;
    return flush() ? 0 : -LUME_EIO;
}

static const struct lume_block_ops raw_operations = {
    .read = raw_read, .write = raw_write, .sync = raw_sync};

static int module_start(struct lume_hardware *hardware, void *context)
{
    read_cache_count = 0;
    struct q35_binding *binding = context;
    uint32_t pci;
    int result = q35_module_pci(hardware, 0x1af4, 0x1001, &pci);
    if (result)
        return result;
    if (!lume_virtio_open(&device, pci, 0x1001, (1u << 1) | (1u << 2) | (1u << 5) | (1u << 9)))
        return -LUME_EIO;
#if defined(LUME_LINUX_VIRTIO_BLOCK)
    result = lume_linux_virtio_attach(&linux_device, &linux_queue_ops, NULL);
    if (result)
        return result;
    struct virtqueue *linux_queue = virtio_find_single_vq(&linux_device, linux_complete, "request");
    if (IS_ERR(linux_queue))
        return (int)PTR_ERR(linux_queue);
#else
    if (!lume_virtio_queue(&device, 0, &queue))
        return -LUME_EIO;
#endif
    if (queue.count < 3)
        return -LUME_EIO;
    request_limit = queue.count / 3u;
    if (request_limit > BLOCK_REQUESTS)
        request_limit = BLOCK_REQUESTS;
    request_sectors = BLOCK_REQUEST_SECTORS;
    if (device.features & (1u << 1))
    {
        unsigned maximum = lume_in32(device.port + 28);
        if (maximum && maximum < 512)
            return -LUME_EIO;
        if (maximum && maximum / 512 < request_sectors)
            request_sectors = maximum / 512;
    }
    if ((device.features & (1u << 2)) && !lume_in32(device.port + 32))
        return -LUME_EIO;
    submitted_requests = 0;
    capacity = (uint64_t)lume_in32(device.port + 20) | (uint64_t)lume_in32(device.port + 24) << 32;
    if (!capacity)
        return -LUME_EIO;
#if defined(LUME_LINUX_VIRTIO_BLOCK)
    virtio_device_ready(&linux_device);
#else
    lume_virtio_ready(&device);
#endif
    if (lume_storage_is_installer())
    {
        bool read_only = (device.features & (1u << 5)) || !(device.features & (1u << 9));
        return lume_pc_disk_register(&raw_disk, "vda", capacity, LUME_SD_MAX_SECTORS,
                                     &raw_operations, NULL, read_only);
    }
    result = lume_platform_driver_register(LUME_PLATFORM_SD, &operations, sizeof(operations));
    binding->registered = !result;
    return result;
}

static int module_stop(struct lume_hardware *hardware, void *context)
{
    read_cache_count = 0;
    (void)hardware;
    struct q35_binding *binding = context;
    if (binding->registered)
    {
        int result = lume_platform_driver_can_stop(LUME_PLATFORM_SD, &operations);
        if (result)
            return result;
    }
    int raw_result = lume_pc_disk_unregister(&raw_disk);
    if (raw_result)
        return raw_result;
    if ((dirty && !flush()) || !reset_transport())
        return -LUME_EIO;
    if (binding->registered)
    {
        int result = lume_platform_driver_unregister(LUME_PLATFORM_SD, &operations);
        if (result)
            return result;
        binding->registered = false;
    }
#if defined(LUME_LINUX_VIRTIO_BLOCK)
    if (linux_device.transport)
    {
        int result = lume_linux_virtio_detach(&linux_device);
        if (result)
            return result;
    }
#else
    lume_virtio_queue_release(&queue);
#endif
    memset(&device, 0, sizeof(device));
    capacity = allowed_first = allowed_count = 0;
    journal_volume = (struct lume_volume){0};
    request_limit = 0;
    request_sectors = 0;
    dirty = transferring = false;
    return 0;
}

#if !defined(LUME_ARCH_I686)
Q35_MODULE("q35-virtio-blk", LUME_HARDWARE_ID_PCI, "pci,1af4:1001", 1u << LUME_RESOURCE_IOPORT);
#else
static bool claimed;

static int probe(struct lume_hardware *hardware, void *context)
{
    (void)hardware;
    if (claimed)
        return -LUME_EBUSY;
    claimed = true;
    ((struct q35_binding *)context)->claimed = true;
    return 0;
}

static int release(struct lume_hardware *hardware, void *context)
{
    struct q35_binding *binding = context;
    if (!binding->claimed)
        return 0;
    int result = module_stop(hardware, context);
    if (!result)
    {
        binding->claimed = false;
        claimed = false;
    }
    return result;
}

int lume_pc_virtio_block_start(bool *present)
{
    *present = false;
    static const struct lume_hardware_identity matches[] = {
        {LUME_HARDWARE_ID_PCI, 0, "pci,1af4:1001"}};
    static const struct lume_hardware_ops lifecycle = {
        .probe = probe, .start = module_start, .stop = release};
    static const struct lume_driver driver = {.version = LUME_DRIVER_VERSION,
                                              .name = "pc-virtio-blk",
                                              .matches = matches,
                                              .match_count = 1,
                                              .context_bytes = sizeof(struct q35_binding),
                                              .resource_kinds = 1u << LUME_RESOURCE_IOPORT,
                                              .ops = &lifecycle};
    int result = lume_driver_register(&driver);
    if (result)
        return result;
    for (unsigned i = 0; i < lume_hardware_count(); ++i)
    {
        struct lume_hardware_info info;
        result = lume_hardware_info(i, &info);
        if (result)
            return result;
        if (lume_driver_matches(&driver, info.id))
        {
            *present = true;
            return lume_hardware_bind(info.id, &driver);
        }
    }
    return 0;
}
#endif
