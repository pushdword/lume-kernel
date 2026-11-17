/* SPDX-License-Identifier: GPL-2.0-only */
#include <lume/linux_ddk.h>
#include <lume/kernel_memory.h>
#include <lume/mmio.h>
#include <lume/irq.h>
#include <lume/counter.h>
#include <lume/cpu.h>
#include <lume/worker.h>
#include <lume/early.h>

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

int lume_linux_ddk_start(struct lume_hardware *device)
{
    if (lume_worker_independent())
        return -EINVAL;
    uint64_t ticks = lume_counter_frequency();
    if (!device || !ticks || ticks > UINT64_C(10000000000))
        return -EINVAL;
    const struct lume_linux_host_ops ops = {
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
    };
    int result = lume_linux_runtime_start(&ops);
    if (!result)
    {
        hardware = device;
        frequency = ticks;
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
