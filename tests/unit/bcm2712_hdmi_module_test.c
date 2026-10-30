#include "lume/module.h"
#include "lume/irq.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define LUME_MMIO_H
static uint32_t lume_mmio_read32(uintptr_t);
#include "../../drivers/display/bcm2712_hdmi/module.c"
const struct lume_hardware_ops hdmi_firmware_lifecycle = {0};
static bool firmware_active;

bool hdmi_firmware_active(void)
{
    return firmware_active;
}

struct lume_hardware
{
    struct lume_resource resources[14];
    const char *reg_names[9], *irq_names[5];
    unsigned count, id;
};

static unsigned logs, depth, acknowledgements;
static int configure_error_line = -1;
static bool enabled[32], in_irq;
static unsigned mmio_reads;
static bool clock_enabled = true;
static uint32_t clock_rate = 120000000;
static bool fail_clock_write;
static uint32_t hpd_value = 1;

static uint32_t lume_mmio_read32(uintptr_t address)
{
    assert(!in_irq && clock_enabled);
    assert(address == UINT64_C(0x107c7015c8) || address == UINT64_C(0x107c7065c8));
    ++mmio_reads;
    return hpd_value;
}

int lume_pi5_firmware_property(uint32_t *message, size_t bytes)
{
    if (message[2] == 0x00030047)
    {
        assert(!in_irq && bytes == 32 && message[5] == 13);
        message[4] = UINT32_C(0x80000008);
        message[6] = clock_rate;
        return 0;
    }
    if (message[2] == 0x00038001 || message[2] == 0x00038002)
    {
        assert(!in_irq && message[5] == 13);
        assert(bytes == (message[2] == 0x00038001 ? 32 : 36));
        if (fail_clock_write)
            return -LUME_EIO;
        if (message[2] == 0x00038001)
            clock_enabled = message[6] & 1u;
        else
            clock_rate = message[6];
        message[4] = UINT32_C(0x80000008);
        return 0;
    }
    assert(!in_irq && bytes == 52 && message[5] == 13 && message[10] == 13);
    message[4] = message[9] = UINT32_C(0x80000008);
    message[6] = clock_enabled;
    message[11] = clock_rate;
    return 0;
}

uint32_t lume_hardware_id(const struct lume_hardware *hardware)
{
    return hardware->id;
}

const struct lume_resource *lume_hardware_resource(const struct lume_hardware *hardware,
                                                   unsigned index)
{
    return index < hardware->count ? hardware->resources + index : NULL;
}

int lume_hardware_property(const struct lume_hardware *hardware, const char *name, unsigned kind,
                           unsigned index, struct lume_property *out)
{
    assert(kind == LUME_PROPERTY_STRING);
    const char *value = NULL;
    if (!strcmp(name, "reg-names") && index < 9)
        value = hardware->reg_names[index];
    if (!strcmp(name, "interrupt-names") && index < 5)
        value = hardware->irq_names[index];
    if (!value)
        return -LUME_ENOENT;
    strcpy(out->string, value);
    return 0;
}

void lume_puts(const char *text)
{
    assert(!in_irq && text);
}

void lume_value(const char *name, uint64_t value)
{
    assert(!in_irq && name);
    (void)value;
    ++logs;
}

uint64_t lume_irq_save(void)
{
    return depth++;
}

void lume_irq_restore(uint64_t flags)
{
    assert(depth == flags + 1);
    depth = (unsigned)flags;
}

static int configure(unsigned irq, unsigned trigger)
{
    (void)irq;
    (void)trigger;
    return 0;
}

static void enable(unsigned irq, bool on)
{
    (void)irq;
    (void)on;
}

static void eoi(unsigned irq)
{
    (void)irq;
}

static int domain_configure(void *context, unsigned irq, unsigned trigger)
{
    assert(context == enabled && irq < 32 && trigger == LUME_IRQ_EDGE);
    return (int)irq == configure_error_line ? -LUME_EIO : 0;
}

static void domain_enable(void *context, unsigned irq, bool on)
{
    assert(context == enabled && irq < 32);
    enabled[irq] = on;
}

static void acknowledge(void *context, unsigned irq)
{
    assert(context == enabled && irq < 32);
    ++acknowledgements;
}

static struct lume_hardware fixture(unsigned port)
{
    struct lume_hardware h = {.count = 14, .id = 38 + port};
    const char *names[] = {"hdmi", "dvp", "phy", "rm", "packet", "metadata", "csc", "cec", "hd"};
    for (unsigned i = 0; i < 9; ++i)
    {
        h.reg_names[i] = names[i];
        h.resources[i] =
            (struct lume_resource){.kind = LUME_RESOURCE_MMIO,
                                   .base = UINT64_C(0x107c701400) + port * 0x5000 + i * 0x1000,
                                   .size = 0x300};
    }
    h.resources[8].base = UINT64_C(0x107c720000);
    unsigned lines[2][5] = {{1, 2, 3, 7, 8}, {11, 12, 13, 14, 15}};
    for (unsigned i = 0; i < 5; ++i)
    {
        h.irq_names[i] = irq_names[i];
        h.resources[9 + i] = (struct lume_resource){.kind = LUME_RESOURCE_IRQ,
                                                    .base = lines[port][i],
                                                    .size = 1,
                                                    .domain = 27,
                                                    .flags = LUME_IRQ_EDGE};
    }
    return h;
}

static void dispatch(unsigned line)
{
    unsigned before_logs = logs;
    in_irq = true;
    assert(lume_irq_domain_dispatch(27, line));
    in_irq = false;
    assert(logs == before_logs);
}

static void dummy(void *context)
{
    (void)context;
}

int main(void)
{
    const struct lume_irq_controller root = {configure, enable, eoi, lume_irq_save,
                                             lume_irq_restore};
    assert(!lume_irq_controller_start(&root, 1020));
    const struct lume_irq_domain_ops ops = {domain_configure, domain_enable, acknowledge};
    assert(!lume_irq_domain_register(27, 32, &ops, enabled));
    struct lume_hardware h = fixture(0), h1 = fixture(1);
    struct hdmi_context a = {0}, b = {0};
    assert(!stop(&h, &a));
    assert(!probe(&h, &a) && !probe(&h1, &b));
    assert(a.hdmi_index == 0 && a.irqs[HDMI_IRQ_HPD_REMOVED].base == 8);
    struct lume_hardware bad = h;
    bad.resources[0].size = 3;
    struct hdmi_context rejected = {0};
    assert(probe(&bad, &rejected) == -LUME_EINVAL);
    bad = h;
    bad.resources[9].size = 2;
    assert(probe(&bad, &rejected) == -LUME_EINVAL);
    bad = h;
    bad.irq_names[4] = bad.irq_names[3];
    assert(probe(&bad, &rejected) == -LUME_EINVAL);
    bad = h;
    bad.resources[13].base = bad.resources[12].base;
    assert(probe(&bad, &rejected) == -LUME_EINVAL);
    bad = h;
    bad.count = 13;
    assert(probe(&bad, &rejected) == -LUME_ENOENT);
    bad = h;
    const char *name = bad.irq_names[0];
    bad.irq_names[0] = bad.irq_names[4];
    bad.irq_names[4] = name;
    assert(!probe(&bad, &rejected) && rejected.irqs[HDMI_IRQ_CEC_TX].base == 8);

    configure_error_line = 8;
    assert(start(&h, &a) == -LUME_EIO);
    assert(a.registered_irqs == (1u << HDMI_IRQ_HPD_CONNECTED));
    assert(!stop(&h, &a));
    configure_error_line = -1;
    assert(!lume_irq_domain_unregister(27, enabled));
    assert(start(&h, &a) == -LUME_EAGAIN);
    assert(!stop(&h, &a));
    assert(!lume_irq_domain_register(27, 32, &ops, enabled));
    assert(!start(&h, &a) && enabled[7] && enabled[8]);
    assert(start(&h, &a) == -LUME_EBUSY);
    assert(!start(&h1, &b) && enabled[14] && enabled[15]);
    uint64_t raw = 123;
    assert(!read_register(&h, &a, 0, HDMI_HOTPLUG, 4, &raw) && raw == 1);
    clock_enabled = false;
    unsigned reads = mmio_reads;
    assert(read_register(&h, &a, 0, HDMI_HOTPLUG, 4, &raw) == -LUME_EAGAIN);
    assert(mmio_reads == reads);
    clock_enabled = true;
    hpd_value = 0;
    assert(!read_register(&h, &a, 0, HDMI_HOTPLUG, 4, &raw) && raw == 0);
    assert(read_register(&h, &a, 0, HDMI_HOTPLUG + 4, 4, &raw) == -LUME_EINVAL);
    assert(read_register(&h, &a, 0, HDMI_HOTPLUG, 8, &raw) == -LUME_EINVAL);
    assert(!stop(&h1, &b));
    dispatch(8);
    assert(lume_work_pending() && !a.hpd_batches[HDMI_IRQ_HPD_REMOVED]);
    assert(lume_work_drain(1) == 1 && a.hpd_batches[HDMI_IRQ_HPD_REMOVED] == 1);
    dispatch(7);
    dispatch(8);
    assert(lume_work_drain(2) == 1);
    assert(a.hpd_batches[HDMI_IRQ_HPD_CONNECTED] == 1);
    assert(a.hpd_batches[HDMI_IRQ_HPD_REMOVED] == 2);
    struct lume_work full[LUME_WORK_LIMIT];
    for (unsigned i = 0; i < LUME_WORK_LIMIT; ++i)
    {
        full[i] = (struct lume_work){.run = dummy};
        assert(!lume_work_submit(full + i));
    }
    dispatch(7);
    assert(atomic_load(&a.hpd_work_error) == -LUME_ENOSPC);
    assert(atomic_load(&a.hpd_events) == (1u << HDMI_IRQ_HPD_CONNECTED));
    assert(lume_work_drain(LUME_WORK_LIMIT) == LUME_WORK_LIMIT);
    dispatch(8);
    assert(lume_work_drain(1) == 1 && !atomic_load(&a.hpd_events));
    assert(!atomic_load(&a.hpd_work_error));
    assert(a.hpd_batches[HDMI_IRQ_HPD_CONNECTED] == 2);
    assert(a.hpd_batches[HDMI_IRQ_HPD_REMOVED] == 3);
    dispatch(8);
    assert(lume_work_pending());
    assert(!stop(&h, &a) && !lume_work_pending() && !enabled[7] && !enabled[8]);
    assert(a.hpd_batches[HDMI_IRQ_HPD_REMOVED] == 4);
    assert(!stop(&h, &a));
    clock_enabled = false;
    clock_rate = 0;
    firmware_active = true;
    assert(start(&h, &a) == -LUME_EAGAIN);
    assert(!stop(&h, &a) && !hsm_users && !clock_rate && !clock_enabled);
    firmware_active = false;
    fail_clock_write = true;
    assert(start(&h, &a) == -LUME_EIO);
    assert(!stop(&h, &a) && !hsm_users && !clock_enabled);
    fail_clock_write = false;
    assert(!start(&h, &a) && clock_enabled && clock_rate == 300000000);
    assert(!start(&h1, &b) && hsm_users == 2);
    assert(!stop(&h1, &b) && clock_enabled && hsm_users == 1);
    assert(!stop(&h, &a) && !clock_enabled && !hsm_users);
    assert(!lume_irq_domain_unregister(27, enabled));
    assert(acknowledgements == 6 && !depth);
    puts("PASS HDMI HPD clock guard, register access, deferred events, both ports and cleanup");
    return 0;
}
