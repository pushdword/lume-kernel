#include "gem.h"
#include <lume/module_build.h>
#include <lume/module.h>
#include <lume/mmio.h>
#include <lume/netif.h>
#include <lume/abi.h>
#include <string.h>

#define POLL_LIMIT 10000u
#define PAGE 4096u

struct gem
{
    uintptr_t mac, cfg;
    struct lume_dma_domain domain;
    struct gem_dma_descriptor *tx, *rx;
    unsigned char *transmit, *receive;
    uint64_t tx_dma, rx_dma, transmit_dma, receive_dma;
    struct lume_netif *interface;
    uint32_t index, phy, control;
    bool pending, running;
    char name[16];
    unsigned char storage[4 * PAGE + PAGE - 1];
};

static uint32_t read_reg(struct gem *g, unsigned offset)
{
    return lume_mmio_read32(g->mac + offset);
}

static void write_reg(struct gem *g, unsigned offset, uint32_t value)
{
    lume_mmio_write32_sync(g->mac + offset, value);
}

static bool sync_descriptor(struct gem_dma_descriptor *descriptor, bool cpu)
{
    return cpu ? lume_dma_sync_for_cpu(descriptor, PAGE, LUME_DMA_BIDIRECTIONAL)
               : lume_dma_sync_for_device(descriptor, PAGE, LUME_DMA_BIDIRECTIONAL);
}

static bool rearm(struct gem *g)
{
    if (!lume_dma_sync_for_device(g->receive, PAGE, LUME_DMA_FROM_DEVICE))
        return false;
    *g->rx = (struct gem_dma_descriptor){.address_low = (uint32_t)g->receive_dma | GEM_RX_WRAP,
                                         .address_high = (uint32_t)(g->receive_dma >> 32)};
    return sync_descriptor(g->rx, false);
}

static int idle(struct gem *g)
{
    for (unsigned i = 0; i < POLL_LIMIT; ++i)
        if (read_reg(g, GEM_NETWORK_STATUS) & GEM_STATUS_MDIO_IDLE)
            return 0;
    return -LUME_EIO;
}

static int phy_read(struct gem *g, unsigned reg, uint16_t *value)
{
    int result = idle(g);
    if (result)
        return result;
    write_reg(g, GEM_PHY_MAINTENANCE,
              GEM_PHY_START_OF_FRAME_CLAUSE_22 | GEM_PHY_OPERATION_READ | GEM_PHY_CODE_CLAUSE_22 |
                  (g->phy << GEM_PHY_ADDRESS_SHIFT) | (reg << GEM_PHY_REGISTER_SHIFT));
    result = idle(g);
    if (!result)
        *value = (uint16_t)read_reg(g, GEM_PHY_MAINTENANCE);
    return result;
}

static int halt(struct gem *g)
{
    if (!g->mac)
        return 0;
    write_reg(g, GEM_NETWORK_CONTROL, GEM_CONTROL_TX_HALT);
    write_reg(g, GEM_INTERRUPT_DISABLE, UINT32_MAX);
    lume_mmio_write32_sync(g->cfg + GEM_CFG_ALIAS_CLEAR + GEM_CFG_INTERRUPT_ENABLE,
                           GEM_CFG_INTERRUPT_ETHERNET);
    for (unsigned i = 0; i < POLL_LIMIT; ++i)
        if (!(read_reg(g, GEM_TX_STATUS) & GEM_TX_STATUS_ACTIVE))
        {
            write_reg(g, GEM_NETWORK_CONTROL, 0);
            g->running = g->pending = false;
            return 0;
        }
    return -LUME_EIO;
}

static void poll(void *context)
{
    struct gem *g = context;
    if (!g->pending || !sync_descriptor(g->tx, true) || !(g->tx->control & GEM_TX_USED))
        return;
    uint32_t status = read_reg(g, GEM_TX_STATUS);
    g->pending = false;
    write_reg(g, GEM_TX_STATUS, status);
    if (status & GEM_TX_STATUS_ERRORS)
        lume_netif_tx_error(g->interface);
}

static bool link(void *context)
{
    struct gem *g = context;
    return g->running &&
           (lume_mmio_read32(g->cfg + GEM_CFG_STATUS) & GEM_CFG_STATUS_RGMII_LINK_STATUS);
}

static int receive(void *context, void *data, size_t capacity)
{
    struct gem *g = context;
    if (!g->running)
        return 0;
    if (!sync_descriptor(g->rx, true))
        return -LUME_EIO;
    if (!(g->rx->address_low & GEM_RX_OWNED))
        return 0;
    uint32_t status = g->rx->control;
    size_t bytes = status & GEM_DESCRIPTOR_LENGTH;
    int result = -LUME_EIO;
    if ((status & (GEM_RX_START | GEM_RX_END)) == (GEM_RX_START | GEM_RX_END) &&
        bytes >= GEM_FRAME_HEADER_BYTES && bytes <= GEM_FRAME_MAX_BYTES)
    {
        if (bytes > capacity)
            result = -LUME_E2BIG;
        else if (lume_dma_sync_for_cpu(g->receive, PAGE, LUME_DMA_FROM_DEVICE))
        {
            memcpy(data, g->receive, bytes);
            result = (int)bytes;
        }
    }
    if (!rearm(g))
        return -LUME_EIO;
    write_reg(g, GEM_RX_STATUS, UINT32_MAX);
    write_reg(g, GEM_NETWORK_CONTROL, g->control);
    return result;
}

static bool transmit(void *context, const void *data, size_t bytes)
{
    struct gem *g = context;
    poll(g);
    if (!g->running || g->pending || bytes < GEM_FRAME_HEADER_BYTES || bytes > GEM_FRAME_MAX_BYTES)
        return false;
    size_t padded = bytes < GEM_FRAME_MIN_BYTES ? GEM_FRAME_MIN_BYTES : bytes;
    memcpy(g->transmit, data, bytes);
    memset(g->transmit + bytes, 0, padded - bytes);
    if (!lume_dma_sync_for_device(g->transmit, PAGE, LUME_DMA_TO_DEVICE))
        return false;
    g->tx->control = GEM_TX_WRAP | GEM_TX_LAST | (uint32_t)padded;
    if (!sync_descriptor(g->tx, false))
        return false;
    g->pending = true;
    write_reg(g, GEM_NETWORK_CONTROL, g->control | GEM_CONTROL_TX_START);
    return true;
}

static bool admin(void *context, bool up)
{
    struct gem *g = context;
    if (!up)
        return !halt(g);
    g->running = true;
    write_reg(g, GEM_NETWORK_CONTROL, g->control);
    return true;
}

static const struct lume_netif_ops net_ops = {poll, link, receive, transmit, admin};

static int probe(struct lume_hardware *hardware, void *context)
{
    struct gem *g = context;
    const struct lume_resource *mac = lume_hardware_resource(hardware, 0);
    const struct lume_resource *cfg = lume_hardware_resource(hardware, 1);
    if (!mac || !cfg || mac->kind != LUME_RESOURCE_MMIO || cfg->kind != LUME_RESOURCE_MMIO ||
        mac->size < GEM_MMIO_BYTES || cfg->size < GEM_MMIO_BYTES)
        return -LUME_EINVAL;
    g->mac = (uintptr_t)mac->base;
    g->cfg = (uintptr_t)cfg->base;
    if (read_reg(g, GEM_MODULE_ID_REGISTER) != GEM_MODULE_ID)
    {
        g->mac = g->cfg = 0;
        return -LUME_ENOENT;
    }
    if (!(read_reg(g, GEM_DESIGN_CONFIG_6) & GEM_DESIGN_DMA_64))
        return -LUME_ENOSYS;
    struct lume_property property;
    int result =
        lume_hardware_property(hardware, "phy-handle", LUME_PROPERTY_REFERENCE, 0, &property);
    if (result)
        return result;
    result = lume_hardware_property_id((uint32_t)property.values[0], "reg", LUME_PROPERTY_U32, 0,
                                       &property);
    if (result || property.values[0] > 31)
        return result ? result : -LUME_EINVAL;
    g->phy = (uint32_t)property.values[0];
    return lume_hardware_dma(hardware, &g->domain);
}

static int start(struct lume_hardware *hardware, void *context)
{
    (void)hardware;
    struct gem *g = context;
    uintptr_t memory = ((uintptr_t)g->storage + PAGE - 1) & ~(uintptr_t)(PAGE - 1);
    g->tx = (void *)memory;
    g->rx = (void *)(memory + PAGE);
    g->transmit = (void *)(memory + 2 * PAGE);
    g->receive = (void *)(memory + 3 * PAGE);
    if (!lume_dma_map(&g->domain, memory, PAGE, &g->tx_dma) ||
        !lume_dma_map(&g->domain, memory + PAGE, PAGE, &g->rx_dma) ||
        !lume_dma_map(&g->domain, memory + 2 * PAGE, PAGE, &g->transmit_dma) ||
        !lume_dma_map(&g->domain, memory + 3 * PAGE, PAGE, &g->receive_dma))
        return -LUME_EFAULT;
    int result = halt(g);
    if (result)
        return result;
    write_reg(g, GEM_NETWORK_CONTROL, GEM_CONTROL_MDIO_ENABLE);
    uint16_t id1, id2;
    if (phy_read(g, 2, &id1) || phy_read(g, 3, &id2) || !id1 || id1 == UINT16_MAX ||
        id2 == UINT16_MAX)
        return -LUME_EIO;
    *g->tx = (struct gem_dma_descriptor){.address_low = (uint32_t)g->transmit_dma,
                                         .control = GEM_TX_USED | GEM_TX_WRAP,
                                         .address_high = (uint32_t)(g->transmit_dma >> 32)};
    if (!sync_descriptor(g->tx, false) || !rearm(g))
        return -LUME_EIO;
    write_reg(g, GEM_NETWORK_CONFIG,
              GEM_CONFIG_FULL_DUPLEX | GEM_CONFIG_GIGABIT | GEM_CONFIG_REMOVE_FCS);
    write_reg(
        g, GEM_DMA_CONFIG,
        GEM_DMA_ADDRESS_64 | GEM_DMA_BURST_INCR4 |
            ((GEM_FRAME_BUFFER_BYTES / GEM_DMA_RX_BUFFER_UNIT_BYTES) << GEM_DMA_RX_BUFFER_SHIFT));
    write_reg(g, GEM_TX_QUEUE_BASE_HIGH, (uint32_t)(g->tx_dma >> 32));
    write_reg(g, GEM_RX_QUEUE_BASE_HIGH, (uint32_t)(g->rx_dma >> 32));
    write_reg(g, GEM_TX_QUEUE_BASE, (uint32_t)g->tx_dma);
    write_reg(g, GEM_RX_QUEUE_BASE, (uint32_t)g->rx_dma);
    write_reg(g, GEM_TX_STATUS, UINT32_MAX);
    write_reg(g, GEM_RX_STATUS, UINT32_MAX);
    g->control = GEM_CONTROL_MDIO_ENABLE | GEM_CONTROL_RX_ENABLE | GEM_CONTROL_TX_ENABLE;
    uint32_t low = read_reg(g, GEM_MAC_ADDRESS_LOW), high = read_reg(g, GEM_MAC_ADDRESS_HIGH);
    /* Programming the upper address word activates the specific-address filter. */
    write_reg(g, GEM_MAC_ADDRESS_LOW, low);
    write_reg(g, GEM_MAC_ADDRESS_HIGH, high);
    struct lume_netif_descriptor descriptor = {
        .name = g->name, .kind = LUME_IF_ETHERNET, .mtu = 1500, .ops = &net_ops, .context = g};
    for (unsigned i = 0; i < 6; ++i)
        descriptor.mac[i] = (unsigned char)((i < 4 ? low : high) >> (8 * (i % 4)));
    memcpy(g->name, "eth", 3);
    unsigned id = lume_hardware_id(hardware), count = 0;
    char digits[10];
    do
    {
        digits[count++] = (char)('0' + id % 10);
        id /= 10;
    } while (id);
    unsigned end = 3;
    while (count)
        g->name[end++] = digits[--count];
    g->name[end] = 0;
    result = lume_netif_register(&descriptor);
    if (result < 0)
        return result;
    g->index = (uint32_t)result;
    g->interface = lume_netif_find(g->index);
    return lume_netif_admin(g->interface, true);
}

static int stop(struct lume_hardware *hardware, void *context)
{
    (void)hardware;
    struct gem *g = context;
    if (g->index)
    {
        int result = lume_netif_unregister(g->index);
        if (result)
            return result;
        g->index = 0;
        g->interface = NULL;
    }
    return halt(g);
}

static int read_register(struct lume_hardware *hardware, void *context, unsigned resource,
                         uint64_t offset, unsigned width, uint64_t *value)
{
    (void)hardware;
    struct gem *g = context;
    if (width != 4)
        return -LUME_EINVAL;
    if (!resource &&
        (offset == GEM_MODULE_ID_REGISTER || offset == GEM_NETWORK_STATUS ||
         offset == GEM_DESIGN_CONFIG_6 || offset == GEM_NETWORK_CONFIG || offset == GEM_DMA_CONFIG))
        *value = read_reg(g, (unsigned)offset);
    else if (resource == 1 && offset == GEM_CFG_STATUS)
        *value = lume_mmio_read32(g->cfg + offset);
    else
        return -LUME_EACCES;
    return 0;
}

static const struct lume_hardware_identity matches[] = {{LUME_HARDWARE_ID_DT, 0, "qemu,rp1-gem"}};
static const struct lume_hardware_ops ops = {
    .probe = probe, .start = start, .stop = stop, .read_register = read_register};
static const struct lume_driver drivers[] = {{.version = LUME_DRIVER_VERSION,
                                              .name = "gem",
                                              .matches = matches,
                                              .match_count = 1,
                                              .context_bytes = sizeof(struct gem),
                                              .ops = &ops}};
const struct lume_module_descriptor lume_module_descriptor = {
    LUME_MODULE_HEADER("gem"), .drivers = drivers, .driver_count = 1};
