/* Connect the single-queue GEM driver to the Lume network interface registry.
 * Link this adapter after gem.o so the controller is attached before registration.
 * Administrative down detaches the controller. The interface and its index remain registered.
 */
#include "gem.h"
#include <lume/device.h>
#include <lume/netif.h>

struct gem_netif_context
{
    struct lume_netif *interface;
    unsigned char mac[6];
    bool attached, tx_pending;
};

static struct gem_netif_context gem_network;

/* Collect each accepted TX completion once, including failures discovered after submission. */
static void gem_netif_poll(void *opaque)
{
    struct gem_netif_context *context = opaque;
    if (!context->tx_pending)
        return;
    int result = lume_example_gem_tx_complete();
    if (result == -LUME_EAGAIN)
        return;
    context->tx_pending = false;
    if (result < 0)
        lume_netif_tx_error(context->interface);
}

static bool gem_netif_link(void *opaque)
{
    (void)opaque;
    return lume_example_gem_link();
}

static int gem_netif_receive(void *opaque, void *data, size_t capacity)
{
    (void)opaque;
    return lume_example_gem_receive(data, capacity);
}

static bool gem_netif_transmit(void *opaque, const void *data, size_t bytes)
{
    struct gem_netif_context *context = opaque;
    gem_netif_poll(context);
    if (context->tx_pending || lume_example_gem_transmit(data, bytes))
        return false;
    context->tx_pending = true;
    return true;
}

/* Keep the registered station address stable across controller reattachments. */
static bool gem_netif_same_mac(const struct gem_netif_context *context,
                               const struct lume_example_gem_layout *layout)
{
    for (unsigned i = 0; i < sizeof(context->mac); ++i)
        if (context->mac[i] != layout->mac[i])
            return false;
    return true;
}

static bool gem_netif_admin(void *opaque, bool up)
{
    struct gem_netif_context *context = opaque;
    if (up)
    {
        if (!context->attached)
            return false;
        struct lume_example_gem_layout layout;
        if (!lume_example_gem_layout(&layout))
        {
            if (lume_example_gem_eth_start())
                return false;
            if (!lume_example_gem_layout(&layout))
            {
                (void)lume_example_gem_eth_stop();
                return false;
            }
        }
        if (gem_netif_same_mac(context, &layout))
            return true;
        (void)lume_example_gem_eth_stop();
        return false;
    }
    gem_netif_poll(context);
    if (lume_example_gem_eth_stop())
        return false;
    if (context->tx_pending)
    {
        context->tx_pending = false;
        lume_netif_tx_error(context->interface);
    }
    return true;
}

static const struct lume_netif_ops gem_netif_operations = {
    .poll = gem_netif_poll,
    .link = gem_netif_link,
    .receive = gem_netif_receive,
    .transmit = gem_netif_transmit,
    .admin = gem_netif_admin,
};

static int gem_netif_start(struct lume_device *device)
{
    struct gem_netif_context *context = device->context;
    struct lume_example_gem_layout layout;
    if (!lume_example_gem_layout(&layout))
        return -LUME_EIO;
    if (context->interface)
    {
        if (!gem_netif_same_mac(context, &layout))
            return -LUME_EINVAL;
    }
    else
    {
        struct lume_netif_descriptor descriptor = {
            .name = "eth0",
            .kind = LUME_IF_ETHERNET,
            .mtu = GEM_FRAME_MAX_BYTES - GEM_FRAME_HEADER_BYTES,
            .ops = &gem_netif_operations,
            .context = context,
        };
        for (unsigned i = 0; i < sizeof(context->mac); ++i)
            descriptor.mac[i] = context->mac[i] = layout.mac[i];
        int index = lume_netif_register(&descriptor);
        if (index < 0)
            return index;
        context->interface = lume_netif_find((uint32_t)index);
    }
    context->attached = true;
    return lume_netif_admin(context->interface, true);
}

static int gem_netif_stop(struct lume_device *device)
{
    struct gem_netif_context *context = device->context;
    int result = context->interface ? lume_netif_admin(context->interface, false) : 0;
    if (!result)
        context->attached = false;
    return result;
}

static const struct lume_driver_ops gem_netif_driver_operations = {
    .start = gem_netif_start,
    .stop = gem_netif_stop,
};

static struct lume_device gem_netif_device = {
    .version = LUME_DDK_VERSION,
    .class_id = LUME_DEVICE_NETWORK,
    .name = "gem-netif",
    .ops = &gem_netif_driver_operations,
    .context = &gem_network,
};

LUME_DRIVER_ENTRY(gem_netif_driver_entry, gem_netif_device);
