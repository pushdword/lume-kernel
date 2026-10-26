#include "lume/virtio.h"
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

void lume_q35_entropy_start(void)
{
    if (!lume_virtio_open(&device, 0x1005, 0))
        return;
    if (!lume_virtio_queue(&device, 0, &queue))
        goto fail;
    lume_virtio_ready(&device);
    static const struct lume_platform_entropy_ops operations = {entropy};
    if (lume_platform_driver_register(LUME_PLATFORM_ENTROPY, &operations, sizeof(operations)))
        goto fail;
    return;
fail:
    lume_virtio_fail(&device);
}
