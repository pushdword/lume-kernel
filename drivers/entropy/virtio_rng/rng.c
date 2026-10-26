#include "lume/virtio.h"
#include "lume/q35_module.h"
#include "lume/platform_driver.h"
#include <string.h>
static struct lume_virtio device;
static struct lume_virtqueue queue;
static _Alignas(16) unsigned char buffer[256];

static bool entropy(void *out, size_t bytes)
{
    if (!out || !bytes || bytes > sizeof(buffer) || device.failed)
        return false;
    size_t offset = 0;
    while (offset < bytes)
    {
        queue.descriptors[0] =
            (struct lume_virtio_desc){(uintptr_t)buffer, (uint32_t)(bytes - offset), 2, 0};
        lume_virtio_submit(&queue, 0);
        unsigned count;
        if (!lume_virtio_wait(&device, &queue, &count) || !count || count > bytes - offset)
        {
            memset(buffer, 0, sizeof(buffer));
            return false;
        }
        memcpy((unsigned char *)out + offset, buffer, count);
        offset += count;
    }
    memset(buffer, 0, sizeof(buffer));
    return true;
}

static const struct lume_platform_entropy_ops operations = {entropy};

static int module_start(struct lume_hardware *hardware, void *context)
{
    struct q35_binding *binding = context;
    uint32_t pci;
    int result = q35_module_pci(hardware, 0x1af4, 0x1005, &pci);
    if (result)
        return result;
    if (!lume_virtio_open(&device, pci, 0x1005, 0) || !lume_virtio_queue(&device, 0, &queue))
        return -LUME_EIO;
    lume_virtio_ready(&device);
    result = lume_platform_driver_register(LUME_PLATFORM_ENTROPY, &operations, sizeof(operations));
    binding->registered = !result;
    return result;
}

static int module_stop(struct lume_hardware *hardware, void *context)
{
    (void)hardware;
    struct q35_binding *binding = context;
    if (!lume_virtio_reset(&device))
        return -LUME_EIO;
    if (binding->registered)
    {
        int result = lume_platform_driver_unregister(LUME_PLATFORM_ENTROPY, &operations);
        if (result)
            return result;
        binding->registered = false;
    }
    lume_virtio_queue_release(&queue);
    memset(&device, 0, sizeof(device));
    memset(buffer, 0, sizeof(buffer));
    return 0;
}

Q35_MODULE("q35-virtio-rng", LUME_HARDWARE_ID_PCI, "pci,1af4:1005", 1u << LUME_RESOURCE_IOPORT);
