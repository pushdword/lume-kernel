#include "lume/virtio.h"
#include "lume/q35_module.h"
#include "lume/platform_driver.h"
#include "lume/sd.h"
#include "lume/worker.h"
#include "lume/counter.h"
#include "lume/cpu.h"
#include <string.h>
static struct lume_virtio device;
static struct lume_virtqueue queue;
static uint64_t capacity, allowed_first, allowed_count;

#define BLOCK_REQUESTS 4u
#define BLOCK_REQUEST_SECTORS 32u

_Static_assert(BLOCK_REQUESTS *BLOCK_REQUEST_SECTORS >= LUME_SD_MAX_SECTORS,
               "Block request slots must cover a complete transfer");

static struct block_request
{
    struct
    {
        uint32_t type, reserved;
        uint64_t sector;
    } header;

    _Alignas(16) unsigned char data[BLOCK_REQUEST_SECTORS * 512];
    volatile unsigned char status;
    unsigned expected;
    bool active, done, success;
} requests[BLOCK_REQUESTS];

static unsigned request_limit;
static bool transferring, dirty;

static void submit(unsigned slot, unsigned type, uint64_t sector, unsigned count,
                   const void *buffer)
{
    struct block_request *request = requests + slot;
    unsigned head = slot * 3u, bytes = count * 512u;
    request->header.type = type;
    request->header.sector = sector;
    request->status = 0xff;
    request->expected = type == 0 ? bytes + 1 : 1;
    request->active = true;
    request->done = request->success = false;
    if (type == 1)
        memcpy(request->data, buffer, bytes);
    queue.descriptors[head] = (struct lume_virtio_desc){(uintptr_t)&request->header, 16, 1,
                                                        (uint16_t)(head + (type == 4 ? 2 : 1))};
    queue.descriptors[head + 1] = (struct lume_virtio_desc){
        (uintptr_t)request->data, bytes, type == 0 ? 3 : 1, (uint16_t)(head + 2)};
    queue.descriptors[head + 2] = (struct lume_virtio_desc){(uintptr_t)&request->status, 1, 2, 0};
    lume_virtio_submit(&queue, head);
}

static bool complete(unsigned count)
{
    unsigned remaining = count;
    uint64_t start = lume_counter_read(), frequency = lume_counter_frequency();
    uint64_t pause = frequency / 1000u + (frequency % 1000u != 0);
    bool success = true;
    while (remaining && !device.failed)
    {
        unsigned id, written;
        while (lume_virtio_take(&queue, &id, &written))
        {
            if (id % 3u || id / 3u >= count || !requests[id / 3u].active || requests[id / 3u].done)
            {
                lume_virtio_fail(&device);
                return false;
            }
            struct block_request *request = requests + id / 3u;
            request->done = true;
            request->success = !request->status && written == request->expected;
            success &= request->success;
            --remaining;
        }
        if (!remaining)
            return success;
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
    if (device.failed || !capacity || !request_limit || transferring ||
        (type != 4 && (!buffer || !count || count > LUME_SD_MAX_SECTORS || sector >= capacity ||
                       count > capacity - sector)))
        return false;
    transferring = true;
    bool success = true;
    unsigned first = 0;
    do
    {
        unsigned pending = 0, submitted = first;
        do
        {
            unsigned part = count - submitted;
            if (part > BLOCK_REQUEST_SECTORS)
                part = BLOCK_REQUEST_SECTORS;
            submit(pending++, type, sector + submitted, part,
                   buffer ? (const unsigned char *)buffer + (size_t)submitted * 512u : NULL);
            submitted += part;
        } while (pending < request_limit && submitted < count);
        success = complete(pending);
        for (unsigned i = 0; i < pending; ++i)
        {
            unsigned part = count - first;
            if (part > BLOCK_REQUEST_SECTORS)
                part = BLOCK_REQUEST_SECTORS;
            if (success && type == 0)
                memcpy((unsigned char *)buffer + (size_t)first * 512u, requests[i].data,
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

static bool read_sector(uint64_t sector, void *buffer)
{
    return transfer(0, sector, 1, buffer);
}

static bool write_sector(uint64_t sector, const void *buffer)
{
    if (!allowed_count || sector < allowed_first || sector - allowed_first >= allowed_count ||
        !(device.features & (1u << 9)))
        return false;
    dirty = true;
    return transfer(1, sector, 1, (void *)buffer) && flush();
}

static bool read_blocks(uint64_t sector, unsigned count, void *buffer)
{
    if (!buffer || !count || count > LUME_SD_MAX_SECTORS || sector >= capacity ||
        count > capacity - sector)
        return false;
    if (!transfer(0, sector, count, buffer))
        return false;
    lume_worker_checkpoint(count * 512);
    return true;
}

static bool write_deferred(uint64_t sector, unsigned count, const void *buffer)
{
    if (!buffer || !count || count > LUME_SD_MAX_SECTORS || sector >= capacity ||
        count > capacity - sector || !allowed_count || sector < allowed_first ||
        sector - allowed_first >= allowed_count ||
        count > allowed_count - (sector - allowed_first) || !(device.features & (1u << 9)))
        return false;
    dirty = true;
    if (!transfer(1, sector, count, (void *)buffer))
        return false;
    lume_worker_checkpoint(count * 512);
    return true;
}

static bool write_blocks(uint64_t sector, unsigned count, const void *buffer)
{
    return write_deferred(sector, count, buffer) && flush();
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

static bool stop(void)
{
    return (!dirty || flush()) && lume_virtio_reset(&device);
}

static void stats(void)
{
    lume_value("VIRTIO_BLOCK_SECTORS", capacity);
}

static const struct lume_platform_sd_ops operations = {
    start,      configure, stop,        read_sector,  write_sector,   sectors, allow,
    allow_boot, stats,     read_blocks, write_blocks, write_deferred, flush};

static int module_start(struct lume_hardware *hardware, void *context)
{
    struct q35_binding *binding = context;
    uint32_t pci;
    int result = q35_module_pci(hardware, 0x1af4, 0x1001, &pci);
    if (result)
        return result;
    if (!lume_virtio_open(&device, pci, 0x1001, (1u << 5) | (1u << 9)) ||
        !lume_virtio_queue(&device, 0, &queue) || queue.count < 3)
        return -LUME_EIO;
    request_limit = queue.count / 3u;
    if (request_limit > BLOCK_REQUESTS)
        request_limit = BLOCK_REQUESTS;
    capacity = (uint64_t)lume_in32(device.port + 20) | (uint64_t)lume_in32(device.port + 24) << 32;
    if (!capacity)
        return -LUME_EIO;
    lume_virtio_ready(&device);
    result = lume_platform_driver_register(LUME_PLATFORM_SD, &operations, sizeof(operations));
    binding->registered = !result;
    return result;
}

static int module_stop(struct lume_hardware *hardware, void *context)
{
    (void)hardware;
    struct q35_binding *binding = context;
    if (binding->registered)
    {
        int result = lume_platform_driver_can_stop(LUME_PLATFORM_SD, &operations);
        if (result)
            return result;
    }
    if ((dirty && !flush()) || !lume_virtio_reset(&device))
        return -LUME_EIO;
    if (binding->registered)
    {
        int result = lume_platform_driver_unregister(LUME_PLATFORM_SD, &operations);
        if (result)
            return result;
        binding->registered = false;
    }
    lume_virtio_queue_release(&queue);
    memset(&device, 0, sizeof(device));
    capacity = allowed_first = allowed_count = 0;
    request_limit = 0;
    dirty = transferring = false;
    return 0;
}

Q35_MODULE("q35-virtio-blk", LUME_HARDWARE_ID_PCI, "pci,1af4:1001", 1u << LUME_RESOURCE_IOPORT);
