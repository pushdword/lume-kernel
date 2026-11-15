#include "lume/irq_async.h"
#include "lume/abi.h"
#include <assert.h>
#include <stdio.h>

static unsigned calls, eois, last_token, masks;
static bool withdraw;
static int context;

static int configure(unsigned irq, unsigned trigger)
{
    assert(irq >= 32 && irq < 320 && trigger <= LUME_IRQ_EDGE);
    return 0;
}

static void enable(unsigned irq, bool on)
{
    assert(irq >= 32 && irq < 320);
    masks += !on;
}

static void eoi(unsigned token)
{
    ++eois;
    last_token = token;
}

static uint64_t save(void)
{
    return 0x80;
}

static void restore(uint64_t flags)
{
    assert(flags == 0x80);
}

static void callback(unsigned irq, void *opaque)
{
    assert(irq == 306 && opaque == &context);
    ++calls;
    if (withdraw)
    {
        assert(lume_irq_unregister(irq, NULL) == -LUME_EPERM);
        assert(lume_irq_unregister(irq, opaque) == -LUME_EBUSY);
        struct lume_irq_info info;
        assert(!lume_irq_info(irq, &info) && info.registered && !info.enabled);
        assert(!lume_irq_dispatch_async(irq));
    }
}

int main(void)
{
    static const struct lume_irq_controller controller = {configure, enable, eoi, save, restore};
    assert(!lume_irq_dispatch_async(306));
    assert(!lume_irq_controller_start(&controller, 320));
    assert(!lume_irq_dispatch_async(306) && !eois);
    assert(!lume_irq_register_async(306, LUME_IRQ_EDGE, callback, &context));
    assert(!lume_irq_dispatch_async(306) && !calls && !eois);
    assert(!lume_irq_enable(306, true));
    assert(lume_irq_dispatch_async(1024 + 306));
    assert(calls == 1 && eois == 1 && last_token == 1024 + 306);
    struct lume_irq_info info;
    assert(!lume_irq_info(306, &info) && info.handled == 1 && info.enabled);
    withdraw = true;
    assert(lume_irq_dispatch_async(306));
    assert(calls == 2 && eois == 2 && masks == 1);
    assert(!lume_irq_unregister(306, &context));
    assert(!lume_irq_dispatch_async(306) && eois == 2);
    assert(!lume_irq_info(306, &info) && info.handled == 2 && !info.registered);
    assert(!lume_irq_register(305, LUME_IRQ_EDGE, callback, &context));
    assert(!lume_irq_enable(305, true));
    assert(!lume_irq_dispatch_async(305) && eois == 2 && calls == 2);
    assert(!lume_irq_unregister(305, &context));
    puts("PASS_IRQ_ASYNC acknowledgement, fallback, masked sources, EOI and retained context");
    return 0;
}
