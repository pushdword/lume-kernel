/* SPDX-License-Identifier: BSD-3-Clause */
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#define lume_virtio_pci_start host_pci_start
#define lume_virtio_pci_queue_start host_queue_start
#define lume_virtio_pci_ready host_pci_ready
#define lume_virtio_pci_reset host_pci_reset
#include "../../drivers/gpu/virtio_gpu/module.c"
#undef lume_virtio_pci_start
#undef lume_virtio_pci_queue_start
#undef lume_virtio_pci_ready
#undef lume_virtio_pci_reset

static const struct lume_driver *registered_driver;
static struct q35_binding binding;
static unsigned devices, ready_calls, reset_calls, live_pages, render_nodes;
static int ready_error;
static bool invalid_queue;
static uint32_t device_config[4] = {0, 0, 1, 2};
static void *queue_allocation;

uint64_t lume_pages_alloc_span_uninitialized(unsigned pages)
{
    assert(!queue_allocation && pages == 2);
    queue_allocation = calloc(pages, 4096);
    assert(queue_allocation);
    live_pages = pages;
    return (uintptr_t)queue_allocation;
}

bool lume_page_free(uint64_t address)
{
    assert(live_pages && address >= (uintptr_t)queue_allocation &&
           address < (uintptr_t)queue_allocation + 8192);
    if (!--live_pages)
    {
        free(queue_allocation);
        queue_allocation = NULL;
    }
    return true;
}

uint32_t lume_q35_pci_read(uint32_t address, unsigned offset)
{
    assert(address == 0x1800);
    return offset == 0 ? UINT32_C(0x10501af4) : 0;
}

void lume_q35_pci_write(uint32_t address, unsigned offset, uint32_t value)
{
    (void)value;
    assert(address == 0x1800 && offset == 4);
}

unsigned lume_q35_pci_bars(uint32_t address, struct lume_pci_bar output[6])
{
    (void)output;
    assert(address == 0x1800);
    return 0;
}

int lume_hardware_property(const struct lume_hardware *hardware, const char *name, unsigned kind,
                           unsigned index, struct lume_property *output)
{
    assert(!hardware && !strcmp(name, "pci-address") && kind == LUME_PROPERTY_U32 && !index);
    *output = (struct lume_property){.kind = kind, .count = 1, .values = {0x1800}};
    return 0;
}

uint64_t lume_q35_counter_frequency(void)
{
    return 1000;
}

uint64_t lume_q35_counter_value(uint64_t tsc)
{
    (void)tsc;
    return 1;
}

void lume_worker_checkpoint(unsigned bytes)
{
    assert(!bytes);
}

void lume_value(const char *name, uint64_t value)
{
    (void)name;
    (void)value;
}

_Noreturn void lume_fail(const char *message)
{
    (void)message;
    abort();
}

int host_pci_start(struct lume_virtio_pci *output, const struct lume_virtio_pci_io *io,
                   uint16_t product, uint64_t wanted, uint64_t required)
{
    (void)wanted;
    assert(product == 0x1050 && required == (UINT64_C(1) << VIRTIO_GPU_F_VIRGL));
    *output = (struct lume_virtio_pci){.io = *io,
                                       .epoch = 1,
                                       .common.bytes = 56,
                                       .config = {(uintptr_t)device_config, sizeof(device_config)},
                                       .features = required | LUME_VIRTIO_PCI_VERSION_1};
    return 0;
}

int host_queue_start(struct lume_virtio_pci *device, struct lume_virtio_pci_queue *output,
                     unsigned number, unsigned count, void *memory, uint64_t physical, size_t bytes)
{
    assert(device == &transport && !device->ready && !number && count == 128 &&
           physical == (uintptr_t)memory && bytes == 8192);
    *output = (struct lume_virtio_pci_queue){.device = device,
                                             .available = memory,
                                             .enabled = true,
                                             .epoch = device->epoch,
                                             .count = invalid_queue ? 0 : count};
    return 0;
}

int host_pci_ready(struct lume_virtio_pci *device)
{
    assert(device == &transport && queue.enabled);
    ++ready_calls;
    if (ready_error)
        return ready_error;
    device->ready = true;
    return 0;
}

int host_pci_reset(struct lume_virtio_pci *device)
{
    ++reset_calls;
    device->ready = false;
    device->reset_complete = true;
    ++device->epoch;
    return 0;
}

int lume_render_device_register(void)
{
    assert(!render_nodes && ready_calls && transport.ready && gpu.slots == 64);
    ++render_nodes;
    return 0;
}

int lume_render_device_unregister(void)
{
    assert(render_nodes == 1);
    --render_nodes;
    return 0;
}

int lume_driver_register(const struct lume_driver *driver)
{
    assert(!registered_driver && !strcmp(driver->name, "pc-virtio-gpu"));
    registered_driver = driver;
    return 0;
}

unsigned lume_hardware_count(void)
{
    return devices;
}

int lume_hardware_info(unsigned index, struct lume_hardware_info *info)
{
    assert(index < devices);
    *info = (struct lume_hardware_info){.id = 10 + index};
    return 0;
}

bool lume_driver_matches(const struct lume_driver *driver, uint32_t id)
{
    assert(driver == registered_driver);
    return id == 11;
}

int lume_hardware_bind(uint32_t id, const struct lume_driver *driver)
{
    assert(id == 11 && driver == registered_driver);
    int result = driver->ops->probe(NULL, &binding);
    if (!result)
        result = driver->ops->start(NULL, &binding);
    if (result)
        assert(!driver->ops->stop(NULL, &binding));
    return result;
}

static void reset_fixture(void)
{
    assert(!queue_allocation && !live_pages && !render_nodes && !claimed && !initialized);
    registered_driver = NULL;
    binding = (struct q35_binding){0};
    ready_calls = reset_calls = 0;
    ready_error = 0;
    invalid_queue = false;
}

static void check_ranges(void)
{
    struct lume_virtio_pci_range output;
    bars[0] = (struct lume_pci_bar){
        .offset = 16,
        .resource = {.kind = LUME_RESOURCE_MMIO, .base = UINTPTR_MAX - 4095, .size = 4096}};
    bar_count = 1;
    assert(range(NULL, 0, 4092, 4, &output));
    assert(output.address == UINTPTR_MAX - 3 && output.bytes == 4);
    assert(!range(NULL, 0, 4093, 4, &output));
    assert(!range(NULL, 0, 4096, 1, &output));
    assert(!range(NULL, 0, 0, 0, &output));
    bars[0].resource.size = 4097;
    assert(!range(NULL, 0, 0, 4, &output));
    bars[0].resource.size = 0;
    assert(!range(NULL, 0, 0, 4, &output));
    bar_count = 0;
}

int main(void)
{
    check_ranges();
    reset_fixture();
    assert(!lume_pc_virtio_gpu_start() && registered_driver && !claimed && !ready_calls);
    reset_fixture();
    devices = 2;
    assert(!lume_pc_virtio_gpu_start() && claimed && render_nodes == 1 && ready_calls == 1);
    struct q35_binding rejected = {0};
    assert(probe(NULL, &rejected) == -LUME_EBUSY && !rejected.claimed);
    assert(!stop(NULL, &rejected) && !reset_calls && render_nodes == 1 && claimed);
    assert(!stop(NULL, &binding) && reset_calls == 1 && !render_nodes && !claimed);
    reset_fixture();
    ready_error = -LUME_EIO;
    assert(lume_pc_virtio_gpu_start() == -LUME_EIO && ready_calls == 1 && reset_calls == 1);
    reset_fixture();
    invalid_queue = true;
    assert(lume_pc_virtio_gpu_start() == -LUME_EINVAL && ready_calls == 1 && reset_calls == 1);
    reset_fixture();
    return 0;
}
