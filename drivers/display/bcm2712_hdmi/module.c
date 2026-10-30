#include <string.h>
#include <stdatomic.h>

#include "lume/module.h"
#include "lume/abi.h"
#include "lume/module_build.h"
#include "lume/irq.h"
#include "lume/work.h"
#include "lume/console.h"
#include "lume/mmio.h"
#include "lume/pi5_firmware.h"
#include "firmware.h"

/* BCM2712 uses the VC6 HDMI_HOTPLUG offset from the Raspberry Pi Linux driver. */
#define HDMI_HOTPLUG 0x1c8u
#define HDMI_HOTPLUG_CONNECTED 1u

extern const struct lume_hardware_ops hdmi_firmware_lifecycle;

static const struct lume_hardware_identity matches[] = {
    {LUME_HARDWARE_ID_DT, 0, "brcm,bcm2712-hdmi0"},
    {LUME_HARDWARE_ID_DT, 0, "brcm,bcm2712-hdmi1"},
};

enum hdmi_irq
{
    HDMI_IRQ_CEC_TX,
    HDMI_IRQ_CEC_RX,
    HDMI_IRQ_CEC_LOW,
    HDMI_IRQ_HPD_CONNECTED,
    HDMI_IRQ_HPD_REMOVED,
    HDMI_IRQ_COUNT
};

static const char *const irq_names[HDMI_IRQ_COUNT] = {
    [HDMI_IRQ_CEC_TX] = "cec-tx",           [HDMI_IRQ_CEC_RX] = "cec-rx",
    [HDMI_IRQ_CEC_LOW] = "cec-low",         [HDMI_IRQ_HPD_CONNECTED] = "hpd-connected",
    [HDMI_IRQ_HPD_REMOVED] = "hpd-removed",
};

struct hdmi_context
{
    struct lume_resource hdmi;
    struct lume_resource irqs[HDMI_IRQ_COUNT];
    unsigned id, hdmi_index, registered_irqs;
    atomic_uint hpd_events;
    struct lume_work hpd_work;
    atomic_int hpd_work_error;
    atomic_bool stopping;
    unsigned hpd_batches[HDMI_IRQ_COUNT];
    bool initialized;
    bool mmio_claimed, hsm_held;
};

static int probe(struct lume_hardware *hardware, void *context)
{
    if (!hardware || !context)
        return -LUME_EINVAL;
    struct lume_resource registers = {0}, irqs[HDMI_IRQ_COUNT] = {0};
    unsigned irq_index = 0, reg_index = 0, irq_mask = 0, hdmi_index = 0;
    bool have_hdmi = false;
    for (unsigned i = 0; i < LUME_HARDWARE_RESOURCES; ++i)
    {
        const struct lume_resource *resource = lume_hardware_resource(hardware, i);
        if (!resource)
            break;
        if (resource->reserved)
            return -LUME_EINVAL;
        if (resource->kind == LUME_RESOURCE_IRQ)
        {
            struct lume_property name = {0};
            int result = lume_hardware_property(hardware, "interrupt-names", LUME_PROPERTY_STRING,
                                                irq_index++, &name);
            if (result)
                return result;
            if (resource->size != 1 || !resource->domain ||
                resource->base >= LUME_IRQ_DOMAIN_LINES || resource->flags != LUME_IRQ_EDGE)
                return -LUME_EINVAL;
            for (unsigned j = 0; j < HDMI_IRQ_COUNT; ++j)
            {
                if (strcmp(name.string, irq_names[j]))
                    continue;
                unsigned bit = 1u << j;
                if (irq_mask & bit)
                    return -LUME_EINVAL;
                for (unsigned k = 0; k < HDMI_IRQ_COUNT; ++k)
                    if ((irq_mask & (1u << k)) && irqs[k].domain == resource->domain &&
                        irqs[k].base == resource->base)
                        return -LUME_EINVAL;
                irqs[j] = *resource;
                irq_mask |= bit;
                break;
            }
            continue;
        }
        if (resource->kind != LUME_RESOURCE_MMIO)
            continue;
        struct lume_property name = {0};
        int result =
            lume_hardware_property(hardware, "reg-names", LUME_PROPERTY_STRING, reg_index++, &name);
        if (result)
            return result;
        if (strcmp(name.string, "hdmi"))
            continue;
        if (have_hdmi || resource->flags || resource->domain || resource->size < HDMI_HOTPLUG + 4 ||
            (resource->base & 3u) || resource->base > UINTPTR_MAX - (HDMI_HOTPLUG + 3) ||
            resource->base > UINT64_MAX - resource->size)
            return -LUME_EINVAL;
        registers = *resource;
        hdmi_index = i;
        have_hdmi = true;
    }
    if (!have_hdmi || irq_mask != (1u << HDMI_IRQ_COUNT) - 1u)
        return -LUME_ENOENT;
    struct hdmi_context *hdmi = context;
    hdmi->hdmi = registers;
    memcpy(hdmi->irqs, irqs, sizeof(irqs));
    hdmi->id = lume_hardware_id(hardware);
    hdmi->hdmi_index = hdmi_index;
    return 0;
}

static unsigned hsm_users;
static uint32_t hsm_state, hsm_rate, hsm_configured_rate;
static bool hsm_state_written, hsm_rate_written;

static int hsm_query(uint32_t *state, uint32_t *rate)
{
    /* Clock 13 supplies the shared BCM2712 HSM. An unclocked register read can stall. */
    uint32_t message[] = {52, 0, 0x00030001, 8, 4, 13, 0, 0x00030002, 8, 4, 13, 0, 0};
    int result = lume_pi5_firmware_property(message, sizeof(message));
    if (result)
        return result;
    if (message[4] != UINT32_C(0x80000008) || message[9] != UINT32_C(0x80000008) ||
        message[5] != 13 || message[10] != 13)
        return -LUME_EIO;
    *state = message[6];
    *rate = message[11];
    if (!*rate)
    {
        uint32_t measured[] = {32, 0, 0x00030047, 8, 4, 13, 0, 0};
        result = lume_pi5_firmware_property(measured, sizeof(measured));
        if (!result && measured[4] == UINT32_C(0x80000008) && measured[5] == 13)
            *rate = measured[6];
    }
    return 0;
}

static int hsm_set(uint32_t tag, uint32_t value)
{
    uint32_t message[] = {32, 0, tag, 8, 8, 13, value, 0};
    if (tag == 0x00038002)
    {
        uint32_t rate[] = {36, 0, tag, 12, 12, 13, value, 0, 0};
        int result = lume_pi5_firmware_property(rate, sizeof(rate));
        if (!result && (rate[4] & UINT32_C(0x80000008)) != UINT32_C(0x80000008))
            result = -LUME_EIO;
        return result;
    }
    int result = lume_pi5_firmware_property(message, sizeof(message));
    if (!result && message[4] != UINT32_C(0x80000008))
        result = -LUME_EIO;
    return result;
}

int hdmi_hsm_acquire(bool *held)
{
    if (hsm_users)
    {
        ++hsm_users;
        *held = true;
        return 0;
    }
    int result = hsm_query(&hsm_state, &hsm_rate);
    if (result)
        return result;
    lume_value("HDMI_HSM_STATE", hsm_state);
    lume_value("HDMI_HSM_HZ", hsm_rate);
    /* A live firmware scanout keeps its inherited clock configuration. */
    if (hdmi_firmware_active() && (!(hsm_state & 1u) || !hsm_rate))
        return -LUME_EAGAIN;
    hsm_users = 1;
    *held = true;
    hsm_state_written = hsm_rate_written = false;
    if (!hsm_rate)
    {
        hsm_rate_written = true;
        result = hsm_set(0x00038002, 300000000);
        if (result)
            return result;
    }
    if (!(hsm_state & 1u))
    {
        hsm_state_written = true;
        result = hsm_set(0x00038001, 1);
        if (result)
            return result;
    }
    uint32_t state;
    result = hsm_query(&state, &hsm_configured_rate);
    if (!result && (!(state & 1u) || !hsm_configured_rate))
        result = -LUME_EAGAIN;
    return result;
}

int hdmi_hsm_release(void)
{
    if (!hsm_users)
        return 0;
    if (hsm_users > 1)
    {
        --hsm_users;
        return 0;
    }
    if (hsm_state_written)
    {
        int result = hsm_set(0x00038001, hsm_state & 1u);
        if (result)
            return result;
        hsm_state_written = false;
    }
    /* A previously unconfigured clock retains its selected rate. */
    if (hsm_rate_written && hsm_rate)
    {
        int result = hsm_set(0x00038002, hsm_rate);
        if (result)
            return result;
    }
    hsm_rate_written = false;
    hsm_users = 0;
    return 0;
}

static int read_hpd(struct hdmi_context *hdmi, uint32_t *value)
{
    uint32_t state, rate;
    int result = hsm_query(&state, &rate);
    if (result)
        return result;
    if (!(state & 1u) || !rate)
        return -LUME_EAGAIN;
    *value = lume_mmio_read32((uintptr_t)hdmi->hdmi.base + HDMI_HOTPLUG);
    return 0;
}

static void report_hpd(struct hdmi_context *hdmi)
{
    unsigned events = atomic_exchange_explicit(&hdmi->hpd_events, 0, memory_order_relaxed);
    int error = atomic_exchange_explicit(&hdmi->hpd_work_error, 0, memory_order_relaxed);
    if (!events && !error)
        return;
    for (unsigned i = 0; i < HDMI_IRQ_COUNT; ++i)
        if (events & (1u << i))
            ++hdmi->hpd_batches[i];
    lume_value("HDMI_DEVICE", hdmi->id);
    lume_value("HDMI_HPD_EVENTS", events);
    lume_value("HDMI_HPD_CONNECTED_BATCHES", hdmi->hpd_batches[HDMI_IRQ_HPD_CONNECTED]);
    lume_value("HDMI_HPD_REMOVED_BATCHES", hdmi->hpd_batches[HDMI_IRQ_HPD_REMOVED]);
    if (!atomic_load_explicit(&hdmi->stopping, memory_order_relaxed))
    {
        uint32_t value;
        int result = read_hpd(hdmi, &value);
        if (result)
            lume_value("HDMI_HPD_ERROR", (uint64_t)(int64_t)result);
        else
            lume_value("HDMI_CONNECTED", !!(value & HDMI_HOTPLUG_CONNECTED));
    }
    if (error)
        lume_value("HDMI_WORK_ERROR", (uint64_t)(int64_t)error);
}

static void process_hpd(void *context)
{
    struct hdmi_context *hdmi = context;
    if (!atomic_load_explicit(&hdmi->stopping, memory_order_relaxed))
        report_hpd(hdmi);
}

static void hpd_interrupt(unsigned irq, void *context)
{
    struct hdmi_context *hdmi = context;
    if (atomic_load_explicit(&hdmi->stopping, memory_order_relaxed))
        return;
    unsigned events = 0;
    if (irq == hdmi->irqs[HDMI_IRQ_HPD_CONNECTED].base)
        events |= 1u << HDMI_IRQ_HPD_CONNECTED;
    if (irq == hdmi->irqs[HDMI_IRQ_HPD_REMOVED].base)
        events |= 1u << HDMI_IRQ_HPD_REMOVED;
    if (!events)
        return;
    atomic_fetch_or_explicit(&hdmi->hpd_events, events, memory_order_relaxed);
    int result = lume_work_submit(&hdmi->hpd_work);
    if (result)
        atomic_store_explicit(&hdmi->hpd_work_error, result, memory_order_relaxed);
}

static int start(struct lume_hardware *hardware, void *context)
{
    (void)hardware;
    struct hdmi_context *hdmi = context;
    if (!hdmi || !hdmi->hdmi.size)
        return -LUME_EINVAL;
    if (hdmi->registered_irqs || hdmi->mmio_claimed)
        return -LUME_EBUSY;
    atomic_init(&hdmi->hpd_events, 0);
    atomic_init(&hdmi->hpd_work_error, 0);
    atomic_init(&hdmi->stopping, false);
    hdmi->hpd_work = (struct lume_work){.run = process_hpd, .context = hdmi};
    memset(hdmi->hpd_batches, 0, sizeof(hdmi->hpd_batches));
    hdmi->initialized = true;
    int result = lume_resource_claim(hdmi, &hdmi->hdmi, 1);
    if (result)
        return result;
    hdmi->mmio_claimed = true;
    result = hdmi_hsm_acquire(&hdmi->hsm_held);
    if (result)
        return result;
    for (unsigned i = HDMI_IRQ_HPD_CONNECTED; i <= HDMI_IRQ_HPD_REMOVED; ++i)
    {
        const struct lume_resource *irq = &hdmi->irqs[i];
        int result = lume_irq_domain_request(irq->domain, (unsigned)irq->base, irq->flags,
                                             hpd_interrupt, hdmi);
        if (result)
            return result;
        hdmi->registered_irqs |= 1u << i;
    }
    for (unsigned i = HDMI_IRQ_HPD_CONNECTED; i <= HDMI_IRQ_HPD_REMOVED; ++i)
    {
        const struct lume_resource *irq = &hdmi->irqs[i];
        int result = lume_irq_domain_enable(irq->domain, (unsigned)irq->base, true);
        if (result)
            return result;
    }
    return 0;
}

static int stop(struct lume_hardware *hardware, void *context)
{
    (void)hardware;
    struct hdmi_context *hdmi = context;
    if (!hdmi || !hdmi->initialized)
        return 0;
    atomic_store_explicit(&hdmi->stopping, true, memory_order_relaxed);
    for (unsigned i = 0; i < HDMI_IRQ_COUNT; ++i)
    {
        unsigned bit = 1u << i;
        if (!(hdmi->registered_irqs & bit))
            continue;
        const struct lume_resource *irq = &hdmi->irqs[i];
        int result = lume_irq_domain_release(irq->domain, (unsigned)irq->base, hdmi);
        if (result)
            return result;
        hdmi->registered_irqs &= ~bit;
    }
    (void)lume_work_cancel(&hdmi->hpd_work);
    report_hpd(hdmi);
    if (hdmi->mmio_claimed)
    {
        int result = lume_resource_release(hdmi);
        if (result)
            return result;
        hdmi->mmio_claimed = false;
    }
    if (hdmi->hsm_held)
    {
        int result = hdmi_hsm_release();
        if (result)
            return result;
        hdmi->hsm_held = false;
    }
    return 0;
}

static int read_register(struct lume_hardware *hardware, void *context, unsigned resource,
                         uint64_t offset, unsigned width, uint64_t *value)
{
    (void)hardware;
    struct hdmi_context *hdmi = context;
    if (!hdmi || !value || resource != hdmi->hdmi_index || width != 4)
        return -LUME_EINVAL;
    if (offset != HDMI_HOTPLUG && offset != 0xe8 && offset != 0xec && offset != 0xf0 &&
        offset != 0xf4 && offset != 0xf8 && offset != 0x100 && offset != 0x104 && offset != 0x114)
        return -LUME_EINVAL;
    if (!hdmi->mmio_claimed || atomic_load_explicit(&hdmi->stopping, memory_order_relaxed))
        return -LUME_ENOENT;
    uint32_t raw;
    int result = read_hpd(hdmi, &raw);
    if (!result)
        *value = offset == HDMI_HOTPLUG
                     ? raw
                     : lume_mmio_read32((uintptr_t)hdmi->hdmi.base + (uintptr_t)offset);
    return result;
}

static const struct lume_hardware_ops lifecycle = {
    .probe = probe,
    .start = start,
    .stop = stop,
    .read_register = read_register,
};

static const struct lume_hardware_identity firmware_matches[] = {
    {LUME_HARDWARE_ID_DT, 0, "brcm,bcm2708-fb"},
};

static const struct lume_driver drivers[] = {
    {
        .version = LUME_DRIVER_VERSION,
        .name = "bcm2712-hdmi",
        .matches = matches,
        .match_count = sizeof(matches) / sizeof(matches[0]),
        .context_bytes = sizeof(struct hdmi_context),
        .resource_kinds = 1u << LUME_RESOURCE_IRQ,
        .ops = &lifecycle,
    },
    {
        .version = LUME_DRIVER_VERSION,
        .name = "pi5-hdmi-framebuffer",
        .matches = firmware_matches,
        .match_count = sizeof(firmware_matches) / sizeof(firmware_matches[0]),
        .context_bytes = sizeof(struct hdmi_framebuffer_context),
        .ops = &hdmi_firmware_lifecycle,
    },
};

const struct lume_module_descriptor lume_module_descriptor = {
    LUME_MODULE_HEADER("bcm2712-hdmi"),
    .drivers = drivers,
    .driver_count = sizeof(drivers) / sizeof(drivers[0]),
};
