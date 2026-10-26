#include "lume/virtio.h"
#include "lume/q35_module.h"
#include "lume/platform_driver.h"
#include <string.h>
static struct lume_virtio device;
static struct lume_virtqueue queue;
static uint64_t capacity, allowed_first, allowed_count;

static struct
{
    uint32_t type, reserved;
    uint64_t sector;
} header;

static _Alignas(16) unsigned char data[512];
static volatile unsigned char status;

static bool transfer(unsigned type, uint64_t sector, void *buffer)
{
    if (device.failed || !capacity || (type != 4 && sector >= capacity))
        return false;
    header.type = type;
    header.sector = sector;
    status = 0xff;
    if (type == 1)
        memcpy(data, buffer, 512);
    queue.descriptors[0] = (struct lume_virtio_desc){(uintptr_t)&header, 16, 1, type == 4 ? 2 : 1};
    queue.descriptors[1] = (struct lume_virtio_desc){(uintptr_t)data, 512, type == 0 ? 3 : 1, 2};
    queue.descriptors[2] = (struct lume_virtio_desc){(uintptr_t)&status, 1, 2, 0};
    lume_virtio_submit(&queue, 0);
    unsigned written;
    if (!lume_virtio_wait(&device, &queue, &written) || status ||
        written != (type == 0 ? 513u : 1u))
        return false;
    if (type == 0)
        memcpy(buffer, data, 512);
    return true;
}

static bool read_sector(uint64_t sector, void *buffer)
{
    return transfer(0, sector, buffer);
}

static bool write_sector(uint64_t sector, const void *buffer)
{
    if (!allowed_count || sector < allowed_first || sector - allowed_first >= allowed_count ||
        !(device.features & (1u << 9)))
        return false;
    return transfer(1, sector, (void *)buffer) && transfer(4, 0, NULL);
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
    return lume_virtio_reset(&device);
}

static void stats(void)
{
    lume_value("VIRTIO_BLOCK_SECTORS", capacity);
}

static const struct lume_platform_sd_ops operations = {
    start, configure, stop, read_sector, write_sector, sectors, allow, allow_boot, stats};

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
    if (!lume_virtio_reset(&device))
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
    return 0;
}

Q35_MODULE("q35-virtio-blk", LUME_HARDWARE_ID_PCI, "pci,1af4:1001", 1u << LUME_RESOURCE_IOPORT);
