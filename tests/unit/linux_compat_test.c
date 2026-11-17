/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <lume/linux_compat.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/spinlock.h>

static _Alignas(64) unsigned char arena[32][2048];
static bool allocated[32];
static int fail_after = -1;
static uint64_t now;
static unsigned live, maps, unmaps, logs, writes, resets;
static uint32_t registers[16], next_word;
static bool fifo, persistent_fault, map_failure;
static struct completion *complete_on_pause;

static void *allocate(size_t bytes)
{
    assert(bytes <= sizeof(arena[0]));
    if (!fail_after)
        return NULL;
    if (fail_after > 0)
        --fail_after;
    for (unsigned i = 0; i < 32; ++i)
        if (!allocated[i])
        {
            assert((uintptr_t)arena[i] <= ULONG_MAX);
            allocated[i] = true;
            ++live;
            return arena[i];
        }
    return NULL;
}

static void deallocate(void *memory)
{
    for (unsigned i = 0; i < 32; ++i)
        if (memory == arena[i])
        {
            assert(allocated[i]);
            allocated[i] = false;
            --live;
            return;
        }
    abort();
}

static uint64_t microseconds(void)
{
    return now;
}

static void pause_us(unsigned us)
{
    now += us;
    if (complete_on_pause)
    {
        complete(complete_on_pause);
        complete_on_pause = NULL;
    }
}

static void relax(void)
{
    ++now;
}

static unsigned long irq_save(void)
{
    return 7;
}

static void irq_restore(unsigned long flags)
{
    assert(flags == 7);
}

static void *map(uint64_t physical, size_t bytes)
{
    assert(physical == UINT64_C(0x107d208000));
    assert(bytes == sizeof(registers));
    if (map_failure)
        return ERR_PTR(-EIO);
    ++maps;
    return registers;
}

static void unmap(void *address, size_t bytes)
{
    assert(address == registers && bytes == sizeof(registers));
    ++unmaps;
}

static u32 read32(const volatile void *address)
{
    ptrdiff_t offset = (const volatile uint32_t *)address - registers;
    assert(offset >= 0 && offset < 16);
    if (offset == 0x24 / 4)
        return fifo ? 1 : 0;
    if (offset == 0x20 / 4)
        return next_word++;
    return registers[offset];
}

static void write32(u32 value, volatile void *address)
{
    ptrdiff_t offset = (volatile uint32_t *)address - registers;
    assert(offset >= 0 && offset < 16);
    ++writes;
    if (offset == 0x18 / 4)
    {
        if (!persistent_fault)
            registers[offset] &= ~value;
        ++resets;
    }
    else
        registers[offset] = value;
}

static void log_message(const char *name, const char *message)
{
    assert(name && message);
    ++logs;
}

static const struct lume_linux_host_ops host = {
    .allocate = allocate,
    .free = deallocate,
    .microseconds = microseconds,
    .pause = pause_us,
    .relax = relax,
    .irq_save = irq_save,
    .irq_restore = irq_restore,
    .map = map,
    .unmap = unmap,
    .read32 = read32,
    .write32 = write32,
    .log = log_message,
};

static void memory_and_waits(void)
{
    assert(lume_linux_runtime_start(NULL) == -EINVAL);
    assert(lume_linux_runtime_start(&host) == 0);
    assert(lume_linux_runtime_start(&host) == -EBUSY);
    assert(!kmalloc(SIZE_MAX, GFP_KERNEL));
    assert(!kmalloc(64, GFP_ATOMIC));
    unsigned first = 3, second = 7;
    assert(min_t(unsigned, first++, second++) == 3 && first == 4 && second == 8);
    void *memory = kzalloc(111, GFP_KERNEL);
    assert(memory && !((uintptr_t)memory & 63));
    for (unsigned i = 0; i < 111; ++i)
        assert(!((unsigned char *)memory)[i]);
    assert(lume_linux_runtime_stop() == -EBUSY);
    kfree(memory);
    struct ida ids = {0};
    for (unsigned i = 0; i < 32; ++i)
        assert(ida_alloc(&ids, GFP_KERNEL) == (int)i);
    assert(ida_alloc(&ids, GFP_KERNEL) == -ENOSPC);
    ida_free(&ids, 17);
    assert(ida_alloc(&ids, GFP_KERNEL) == 17);
    struct completion completion;
    init_completion(&completion);
    complete(&completion);
    complete(&completion);
    assert(wait_for_completion_killable(&completion) == 0);
    assert(wait_for_completion_killable(&completion) == 0);
    complete_on_pause = &completion;
    assert(wait_for_completion_killable(&completion) == 0);
    uint64_t start = now;
    assert(wait_for_completion_killable(&completion) == -ETIMEDOUT);
    assert(now - start >= 5000000);
    spinlock_t lock = {0};
    unsigned long flags;
    spin_lock_irqsave(&lock, flags);
    assert(lock.locked);
    spin_unlock_irqrestore(&lock, flags);
    assert(!lock.locked);
    assert(lume_linux_runtime_stop() == 0);
    assert(!live);
}

static struct platform_device platform_device(struct resource *resource)
{
    return (struct platform_device){
        .dev = {.name = "rng-test"},
        .resource = resource,
        .num_resources = 1,
    };
}

static void rng200(void)
{
    struct resource resource = {UINT64_C(0x107d208000), sizeof(registers)};
    struct platform_driver *driver = lume_linux_platform_descriptor();
    for (int fail = 0; fail < 2; ++fail)
    {
        assert(lume_linux_runtime_start(&host) == 0);
        struct platform_device device = platform_device(&resource);
        fail_after = fail;
        assert(lume_linux_platform_probe(driver, &device) == -ENOMEM);
        assert(!device.dev.resource_count && !live && !lume_linux_hwrng_available());
        assert(lume_linux_runtime_stop() == 0);
    }
    fail_after = -1;
    assert(lume_linux_runtime_start(&host) == 0);
    struct platform_device failed = platform_device(&resource);
    map_failure = true;
    assert(lume_linux_platform_probe(driver, &failed) == -EIO);
    assert(!live);
    map_failure = false;
    struct platform_device device = platform_device(&resource);
    assert(lume_linux_platform_probe(driver, &device) == 0);
    assert(registers[0] & 1);
    assert(lume_linux_hwrng_available());
    assert(lume_linux_runtime_stop() == -EBUSY);
    unsigned char output[82];
    memset(output, 0xa5, sizeof(output));
    fifo = true;
    next_word = UINT32_C(0x44332211);
    assert(lume_linux_hwrng_read(output + 1, 73, true) == 73);
    assert(output[0] == 0xa5 && output[74] == 0xa5);
    assert(output[1] == 0x11 && output[2] == 0x22 && output[3] == 0x33 && output[4] == 0x44);
    fifo = false;
    assert(lume_linux_hwrng_read(output, 32, false) == 0);
    uint64_t before = now;
    assert(lume_linux_hwrng_read(output, 32, true) == 0);
    assert(now - before >= 1000000);
    registers[0x18 / 4] = UINT32_C(0x80000000);
    persistent_fault = true;
    unsigned previous = resets;
    assert(lume_linux_hwrng_read(output, 32, true) == 0);
    assert(resets == previous + 1);
    persistent_fault = false;
    registers[0x18 / 4] = 0;
    assert(lume_linux_device_release(&device.dev) == 0);
    assert(!(registers[0] & 1));
    assert(!live && maps == unmaps && !lume_linux_hwrng_available());
    assert(lume_linux_runtime_stop() == 0);
}

struct mock_transport
{
    void *buffer;
    unsigned capacity, submitted, taken, destroyed, resets, notifications;
    int delay, error;
    unsigned produced, next;
    bool reset_fail, stuck, created, pending;
};

static int queue_create(void *context)
{
    struct mock_transport *transport = context;
    transport->created = true;
    return transport->error;
}

static int queue_submit(void *context, void *buffer, unsigned bytes)
{
    struct mock_transport *transport = context;
    assert(transport->created && !transport->pending && bytes >= 32);
    transport->buffer = buffer;
    transport->capacity = bytes;
    transport->pending = true;
    ++transport->submitted;
    return 0;
}

static void queue_notify(void *context)
{
    ++((struct mock_transport *)context)->notifications;
}

static int queue_take(void *context, unsigned *bytes)
{
    struct mock_transport *transport = context;
    if (transport->error)
        return transport->error;
    if (transport->stuck || !transport->pending || transport->delay-- > 0)
        return 0;
    *bytes = transport->produced;
    if (*bytes <= transport->capacity)
        for (unsigned i = 0; i < *bytes; ++i)
            ((unsigned char *)transport->buffer)[i] = (unsigned char)transport->next++;
    transport->pending = false;
    ++transport->taken;
    return 1;
}

static void queue_ready(void *context)
{
    assert(((struct mock_transport *)context)->created);
}

static int queue_reset(void *context)
{
    struct mock_transport *transport = context;
    ++transport->resets;
    if (transport->reset_fail)
        return -EIO;
    transport->pending = false;
    return 0;
}

static void queue_destroy(void *context)
{
    struct mock_transport *transport = context;
    assert(!transport->pending);
    transport->created = false;
    ++transport->destroyed;
}

static const struct lume_linux_virtio_ops transport_ops = {
    .create = queue_create,
    .submit = queue_submit,
    .notify = queue_notify,
    .take = queue_take,
    .ready = queue_ready,
    .reset = queue_reset,
    .destroy = queue_destroy,
};

static void virtio_rng(void)
{
    struct virtio_driver *driver = lume_linux_virtio_descriptor();
    assert(lume_linux_runtime_start(&host) == 0);
    struct virtio_device device = {0};
    struct mock_transport transport = {.delay = 3, .produced = 17};
    assert(lume_linux_virtio_probe(driver, &device, &transport_ops, &transport) == 0);
    assert(lume_linux_hwrng_available() && transport.pending);
    unsigned char output[200];
    memset(output, 0xcc, sizeof(output));
    assert(lume_linux_hwrng_read(output + 1, 150, true) == 150);
    for (unsigned i = 0; i < 150; ++i)
        assert(output[i + 1] == (unsigned char)i);
    assert(output[0] == 0xcc && output[151] == 0xcc);
    assert(transport.submitted > 1 && transport.notifications == transport.submitted);
    transport.reset_fail = true;
    unsigned outstanding = live;
    assert(lume_linux_virtio_remove(driver, &device) == -EIO);
    assert(live == outstanding && device.priv && !transport.destroyed);
    transport.reset_fail = false;
    assert(lume_linux_virtio_remove(driver, &device) == 0);
    assert(!live && !device.priv && transport.destroyed == 1);
    assert(!lume_linux_hwrng_available());
    assert(lume_linux_runtime_stop() == 0);

    assert(lume_linux_runtime_start(&host) == 0);
    memset(&device, 0, sizeof(device));
    transport = (struct mock_transport){.produced = 65};
    assert(lume_linux_virtio_probe(driver, &device, &transport_ops, &transport) == 0);
    memset(output, 0xcc, sizeof(output));
    assert(lume_linux_hwrng_read(output, 32, true) == -EIO);
    for (unsigned i = 0; i < 32; ++i)
        assert(!output[i]);
    assert(lume_linux_virtio_remove(driver, &device) == 0);
    assert(lume_linux_runtime_stop() == 0);

    assert(lume_linux_runtime_start(&host) == 0);
    memset(&device, 0, sizeof(device));
    transport = (struct mock_transport){.stuck = true};
    assert(lume_linux_virtio_probe(driver, &device, &transport_ops, &transport) == 0);
    uint64_t start = now;
    assert(lume_linux_hwrng_read(output, 32, true) == -ETIMEDOUT);
    assert(now - start >= 5000000 && live);
    assert(lume_linux_virtio_remove(driver, &device) == 0);
    assert(!live && lume_linux_runtime_stop() == 0);

    assert(lume_linux_runtime_start(&host) == 0);
    memset(&device, 0, sizeof(device));
    transport = (struct mock_transport){.error = -EIO};
    assert(lume_linux_virtio_probe(driver, &device, &transport_ops, &transport) == -EIO);
    assert(!live && lume_linux_runtime_stop() == 0);
}

int main(void)
{
    memory_and_waits();
    rng200();
    virtio_rng();
    printf("Linux driver compatibility PASS: allocation failures, managed rollback, RNG200, "
           "partial VirtIO completions, DMA reset refusal, oversized replies and deadlines.\n");
    return 0;
}
