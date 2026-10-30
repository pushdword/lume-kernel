#include "lume/module.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define LUME_MMIO_H
static uint32_t lume_mmio_read32(uintptr_t);
static void lume_mmio_write32_sync(uintptr_t, uint32_t);
#include "../../drivers/irq/brcm_l2/module.c"

static uintptr_t register_base = UINT64_C(0x107d510600);
static uint32_t cpu_mask, pending;
static unsigned writes, completions, delivered[L2_LINES], depth;
static int parent_error;
static bool parent_enabled, retrigger;

struct lume_hardware
{
    struct lume_resource resources[LUME_HARDWARE_RESOURCES];
    unsigned count;
    uint32_t id;
};

uint32_t lume_hardware_id(const struct lume_hardware *hardware)
{
    return hardware->id;
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

static uint32_t lume_mmio_read32(uintptr_t address)
{
    assert(address == register_base + L2_STATUS || address == register_base + L2_MASK_STATUS);
    return address == register_base + L2_STATUS ? pending : cpu_mask;
}

static void lume_mmio_write32_sync(uintptr_t address, uint32_t value)
{
    ++writes;
    if (address == register_base + L2_CLEAR)
        pending &= ~value;
    else if (address == register_base + L2_MASK_SET)
        cpu_mask |= value;
    else if (address == register_base + L2_MASK_CLEAR)
        cpu_mask &= ~value;
    else
        assert(false);
}

static int parent_configure(unsigned irq, unsigned trigger)
{
    assert(irq == 271 && trigger == LUME_IRQ_LEVEL);
    return parent_error;
}

static void parent_enable(unsigned irq, bool enabled)
{
    assert(irq == 271);
    parent_enabled = enabled;
}

static void parent_complete(unsigned token)
{
    assert(token == 271);
    ++completions;
}

static void consumer(unsigned irq, void *context)
{
    assert(context == delivered && irq < L2_LINES && !(pending & (UINT32_C(1) << irq)));
    ++delivered[irq];
    if (retrigger)
    {
        retrigger = false;
        pending |= UINT32_C(1) << irq;
    }
}

const struct lume_resource *lume_hardware_resource(const struct lume_hardware *hardware,
                                                   unsigned index)
{
    return index < hardware->count ? hardware->resources + index : NULL;
}

static struct lume_hardware fixture(uint64_t base, unsigned irq, bool reversed)
{
    struct lume_hardware hardware = {.count = 2, .id = 27};
    hardware.resources[reversed ? 1 : 0] =
        (struct lume_resource){.base = base, .size = 0x30, .kind = LUME_RESOURCE_MMIO};
    hardware.resources[reversed ? 0 : 1] =
        (struct lume_resource){.base = irq, .size = 1, .kind = LUME_RESOURCE_IRQ};
    return hardware;
}

static void rejected(struct lume_hardware *hardware, int expected)
{
    struct l2_context context, before;
    memset(&context, 0xa5, sizeof(context));
    memcpy(&before, &context, sizeof(before));
    assert(probe(hardware, &context) == expected);
    assert(!memcmp(&before, &context, sizeof(context)));
}

int main(void)
{
    for (unsigned reversed = 0; reversed < 2; ++reversed)
        for (unsigned instance = 0; instance < 2; ++instance)
        {
            uint64_t base = instance ? UINT64_C(0x2100000000) : UINT64_C(0x107d510600);
            unsigned irq = instance ? 300 : 271;
            struct lume_hardware hardware = fixture(base, irq, reversed);
            struct l2_context context = {0};
            assert(!probe(&hardware, &context));
            assert(context.registers.base == base && context.registers.size == 0x30);
            assert(context.parent_irq.base == irq && context.parent_irq.size == 1);
            assert(!stop(&hardware, &context));
        }

    struct lume_hardware valid = fixture(UINT64_C(0x107d510600), 271, false);
    struct lume_hardware bad = valid;
    bad.count = 0;
    rejected(&bad, -LUME_EINVAL);
    bad = valid;
    bad.count = 1;
    rejected(&bad, -LUME_EINVAL);
    bad = valid;
    bad.resources[0] = valid.resources[1];
    bad.count = 1;
    rejected(&bad, -LUME_EINVAL);

    for (unsigned index = 0; index < 2; ++index)
    {
        bad = valid;
        bad.resources[bad.count++] = valid.resources[index];
        rejected(&bad, -LUME_EINVAL);
        bad = valid;
        bad.resources[index].reserved = 1;
        rejected(&bad, -LUME_EINVAL);
    }

    bad = valid;
    bad.resources[0].size = 0x2f;
    rejected(&bad, -LUME_EINVAL);
    bad = valid;
    bad.resources[0].base++;
    rejected(&bad, -LUME_EINVAL);
    bad = valid;
    bad.resources[0].base = UINT64_MAX - 3;
    rejected(&bad, -LUME_EINVAL);
    bad = valid;
    bad.resources[0].domain = 1;
    rejected(&bad, -LUME_EINVAL);
    bad = valid;
    bad.resources[0].flags = 1;
    rejected(&bad, -LUME_EINVAL);
    bad = valid;
    bad.resources[1].flags = LUME_IRQ_EDGE;
    rejected(&bad, -LUME_EINVAL);
    bad = valid;
    bad.resources[1].size = 2;
    rejected(&bad, -LUME_EINVAL);
    bad = valid;
    bad.resources[1].domain = 27;
    rejected(&bad, -LUME_ENOSYS);
    bad = valid;
    bad.resources[1].base = 31;
    rejected(&bad, -LUME_ENOSYS);
    bad = valid;
    bad.resources[1].base = LUME_IRQ_LIMIT;
    rejected(&bad, -LUME_ENOSYS);
    bad = valid;
    bad.resources[0].kind = LUME_RESOURCE_IOPORT;
    rejected(&bad, -LUME_ENOSYS);

    struct l2_context context = {0};
    assert(probe(NULL, &context) == -LUME_EINVAL);
    assert(probe(&valid, NULL) == -LUME_EINVAL);
    assert(lume_module_descriptor.driver_count == 1);
    assert(drivers[0].context_bytes == sizeof(context));
    assert(drivers[0].resource_kinds == ((1u << LUME_RESOURCE_MMIO) | (1u << LUME_RESOURCE_IRQ)));
    assert(drivers[0].match_count == 2);
    assert(drivers[0].ops == &lifecycle);
    assert(!strcmp(lume_module_descriptor.name, "brcm-l2"));

    assert(!probe(&valid, &context));
    assert(start(&valid, &context) == -LUME_EAGAIN && !writes);
    const struct lume_irq_controller controller = {parent_configure, parent_enable, parent_complete,
                                                   lume_irq_save, lume_irq_restore};
    assert(!lume_irq_controller_start(&controller, LUME_IRQ_LIMIT));
    struct lume_hardware no_id = valid;
    no_id.id = 0;
    assert(start(&no_id, &context) == -LUME_EINVAL && !writes);
    assert(start(NULL, &context) == -LUME_EINVAL);
    assert(start(&valid, NULL) == -LUME_EINVAL);
    assert(stop(&valid, NULL) == -LUME_EINVAL);

    uint32_t original_mask = UINT32_C(0xa5000000);
    cpu_mask = original_mask;
    pending = UINT32_C(1) << 1;
    parent_error = -LUME_EBUSY;
    assert(start(&valid, &context) == -LUME_EBUSY);
    assert(cpu_mask == original_mask && !lume_irq_domain_available(27));
    assert(!context.mask_saved && !context.domain_registered && !context.parent_registered);
    assert(pending == (UINT32_C(1) << 1));
    parent_error = 0;
    struct l2_context other = {0};
    assert(!lume_irq_domain_register(27, L2_LINES, &domain_ops, &other));
    assert(start(&valid, &context) == -LUME_EEXIST && cpu_mask == original_mask);
    assert(!lume_irq_domain_unregister(27, &other));

    assert(!start(&valid, &context));
    assert(context.domain == 27 && parent_enabled && cpu_mask == UINT32_MAX);
    assert(pending == (UINT32_C(1) << 1));
    assert(start(&valid, &context) == -LUME_EBUSY);
    assert(lume_irq_domain_request(27, 1, LUME_IRQ_LEVEL, consumer, delivered) == -LUME_EINVAL);
    assert(!lume_irq_domain_request(27, 1, LUME_IRQ_EDGE, consumer, delivered));
    assert(!lume_irq_domain_request(27, 31, LUME_IRQ_EDGE, consumer, delivered));
    assert(!lume_irq_domain_enable(27, 1, true));
    assert(!lume_irq_domain_enable(27, 31, true));
    uint32_t enabled_mask = cpu_mask;
    assert(stop(&valid, &context) == -LUME_EBUSY);
    assert(cpu_mask == enabled_mask && parent_enabled && context.domain_registered);

    pending |= UINT32_C(1) << 31;
    assert(lume_irq_dispatch(271));
    assert(delivered[1] == 1 && delivered[31] == 1 && !pending && completions == 1);
    retrigger = true;
    pending = UINT32_C(1) << 1;
    assert(lume_irq_dispatch(271) && delivered[1] == 2 && pending == 2);
    assert(lume_irq_dispatch(271) && delivered[1] == 3 && !pending);

    pending = UINT32_C(1) << 6;
    cpu_mask &= ~pending;
    assert(lume_irq_dispatch(271) && !pending && (cpu_mask & (UINT32_C(1) << 6)));
    struct lume_irq_info info;
    assert(!lume_irq_domain_info(27, 6, &info) && info.stray == 1);
    assert(!lume_irq_domain_release(27, 1, delivered));
    assert(!lume_irq_domain_release(27, 31, delivered));
    assert(!stop(&valid, &context));
    assert(cpu_mask == original_mask && !parent_enabled && !lume_irq_domain_available(27));
    assert(!lume_irq_info(271, &info) && !info.registered && !info.enabled);
    assert(!context.mask_saved && !context.domain_registered && !context.parent_registered);
    assert(!stop(&valid, &context) && !depth);
    puts("BRCM_L2_MODULE_PASS resources, startup rollback, chained dispatch and busy stop");
    return 0;
}
