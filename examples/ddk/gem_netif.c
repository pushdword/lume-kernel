/* Connect the single-queue GEM driver to the Lume network interface registry.
 * Link this adapter after gem.o so the controller is attached before registration.
 * Administrative down detaches the controller. The interface and its index remain registered.
 */
#include "gem.h"
#include <lume/device.h>
#include <lume/netif.h>
#include <string.h>

extern uint32_t lume_clock_millis(void);
#define GEM_LINK_POLL_MS 250u
#define GEM_TX_QUEUE 16u

struct gem_netif_context
{
    struct lume_netif *interface;
    unsigned char mac[6];
    bool attached, tx_pending;
    bool link_valid, link_up;
    uint32_t link_sampled;
    uint32_t tx_started;
    uint32_t tx_restarts;
    unsigned tx_first, tx_count;

    struct
    {
        size_t bytes;
        unsigned char data[GEM_FRAME_MAX_BYTES];
    } tx_queue[GEM_TX_QUEUE];
};

static struct gem_netif_context gem_network;

static void gem_netif_depth(struct gem_netif_context *context)
{
    lume_netif_account(context->interface, LUME_NET_TX_DEPTH,
                       context->tx_count + (unsigned)context->tx_pending);
}

/* Collect at most one completion and submit at most one queued frame per poll. */
static void gem_netif_poll(void *opaque)
{
    struct gem_netif_context *context = opaque;
    if (context->tx_pending)
    {
        lume_netif_account(context->interface, LUME_NET_TX_POLL, 0);
        int result = lume_example_gem_tx_complete();
        uint32_t restarts = lume_example_gem_tx_restarts();
        lume_netif_account(context->interface, LUME_NET_TX_RESTART,
                           (uint32_t)(restarts - context->tx_restarts));
        context->tx_restarts = restarts;
        if (result == -LUME_EAGAIN)
            return;
        context->tx_pending = false;
        if (result < 0)
            lume_netif_account(context->interface,
                               lume_example_gem_tx_failure() == GEM_TX_FAILURE_TIMEOUT
                                   ? LUME_NET_TX_TIMEOUT
                                   : LUME_NET_TX_DMA_ERROR,
                               0);
        else
            lume_netif_account(context->interface, LUME_NET_TX_COMPLETE,
                               (uint32_t)(lume_clock_millis() - context->tx_started));
    }
    if (context->tx_count)
    {
        unsigned slot = context->tx_first;
        int result =
            lume_example_gem_transmit(context->tx_queue[slot].data, context->tx_queue[slot].bytes);
        if (result == -LUME_EAGAIN)
        {
            lume_netif_account(context->interface, LUME_NET_TX_BUSY, 0);
            return;
        }
        context->tx_first = (slot + 1) % GEM_TX_QUEUE;
        --context->tx_count;
        if (result < 0)
            lume_netif_account(context->interface, LUME_NET_TX_SUBMIT_ERROR, 0);
        else
        {
            context->tx_pending = true;
            context->tx_started = lume_clock_millis();
        }
    }
    gem_netif_depth(context);
}

static bool gem_netif_link(void *opaque)
{
    struct gem_netif_context *context = opaque;
    uint32_t now = lume_clock_millis();
    /* Sample PHY management independently of the packet polling rate. */
    if (!context->link_valid || (uint32_t)(now - context->link_sampled) >= GEM_LINK_POLL_MS)
    {
        context->link_up = lume_example_gem_link();
        context->link_sampled = now;
        context->link_valid = true;
    }
    return context->link_up;
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
    if (context->tx_count == GEM_TX_QUEUE)
    {
        lume_netif_account(context->interface, LUME_NET_TX_BUSY, 0);
        return false;
    }
    unsigned slot = (context->tx_first + context->tx_count) % GEM_TX_QUEUE;
    memcpy(context->tx_queue[slot].data, data, bytes);
    context->tx_queue[slot].bytes = bytes;
    ++context->tx_count;
    gem_netif_depth(context);
    if (!context->tx_pending)
        gem_netif_poll(context);
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
    context->link_valid = false;
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
    if (context->tx_pending || context->tx_count)
    {
        lume_netif_account(context->interface, LUME_NET_TX_CANCELLED,
                           context->tx_count + (unsigned)context->tx_pending);
        context->tx_pending = false;
        context->tx_first = context->tx_count = 0;
        gem_netif_depth(context);
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
    context->tx_restarts = lume_example_gem_tx_restarts();
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

int lume_example_gem_netif_attach(void)
{
    return lume_device_attach(&gem_netif_device);
}

int lume_example_gem_netif_detach(void)
{
    int result =
        gem_netif_device.state == LUME_DEVICE_OFF ? 0 : lume_device_detach(&gem_netif_device);
    if (result || !gem_network.interface)
        return result;
    struct lume_netif_info info = {0};
    lume_netif_info(gem_network.interface, &info);
    result = lume_netif_unregister(info.ifindex);
    if (!result)
        gem_network = (struct gem_netif_context){0};
    return result;
}
