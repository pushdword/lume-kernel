#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* Replace only the architecture MMIO primitives. The driver and DDK lifecycle
 * below remain the production implementations. */
#define LUME_MMIO_H
static uint32_t lume_mmio_read32(uintptr_t);
static void lume_mmio_write32_sync(uintptr_t, uint32_t);
#include "gem.c"
#include "gem_netif.c"

static uint32_t registers[GEM_MMIO_BYTES / 4], cfg_status;
static unsigned checks, sync_calls, fail_sync, map_calls, fail_map;
static bool dma_available = true, dma_submitted;
const struct lume_dma_domain lume_rp1_dma_domain = {0, UINT64_C(0x1000000000),
                                                    UINT64_C(0x200000000), UINT64_MAX};
#define T(condition)                                                                               \
    do                                                                                             \
    {                                                                                              \
        assert(condition);                                                                         \
        ++checks;                                                                                  \
    } while (0)

static uint32_t lume_mmio_read32(uintptr_t address)
{
    if (address == GEM_CFG_BASE + GEM_CFG_STATUS)
        return cfg_status;
    assert(address >= GEM_MAC_BASE && address < GEM_MAC_BASE + GEM_MMIO_BYTES);
    return registers[(address - GEM_MAC_BASE) / 4];
}

static void lume_mmio_write32_sync(uintptr_t address, uint32_t value)
{
    if (address >= GEM_CFG_BASE)
    {
        T(address == GEM_CFG_BASE + GEM_CFG_ALIAS_CLEAR + GEM_CFG_INTERRUPT_ENABLE);
        T(value == GEM_CFG_INTERRUPT_ETHERNET);
        return;
    }
    unsigned offset = (unsigned)(address - GEM_MAC_BASE);
    assert(offset < GEM_MMIO_BYTES && !(offset & 3));
    if (offset == GEM_TX_STATUS || offset == GEM_RX_STATUS)
        registers[offset / 4] &= ~value;
    else
        registers[offset / 4] = value;
    if (offset == GEM_PHY_MAINTENANCE)
    {
        unsigned reg = (value >> 18) & 31;
        registers[offset / 4] = (value & 0xffff0000u) | (reg == 2 ? 0x141 : 0xcc2);
    }
    if (offset == GEM_NETWORK_CONTROL && (value & GEM_CONTROL_TX_START))
        dma_submitted = true;
}

bool lume_pi5_dma_start(void)
{
    return dma_available;
}

bool lume_dma_map(const struct lume_dma_domain *domain, uint64_t physical, size_t bytes,
                  uint64_t *bus)
{
    T(domain == &lume_rp1_dma_domain && bytes == GEM_DMA_PAGE_BYTES);
    T(!(physical & (GEM_DMA_PAGE_BYTES - 1)));
    ++map_calls;
    if (map_calls == fail_map)
        return false;
    *bus = UINT64_C(0x1000400000) + ((map_calls - 1) % 4) * GEM_DMA_PAGE_BYTES;
    return true;
}

bool lume_dma_sync_for_cpu(const void *p, size_t bytes, enum lume_dma_direction direction)
{
    T(p && bytes == GEM_DMA_PAGE_BYTES);
    T(direction == LUME_DMA_FROM_DEVICE || direction == LUME_DMA_BIDIRECTIONAL);
    return ++sync_calls != fail_sync;
}

bool lume_dma_sync_for_device(const void *p, size_t bytes, enum lume_dma_direction direction)
{
    T(p && bytes == GEM_DMA_PAGE_BYTES);
    T(direction >= LUME_DMA_TO_DEVICE && direction <= LUME_DMA_BIDIRECTIONAL);
    return ++sync_calls != fail_sync;
}

static void reset_model(void)
{
    memset(registers, 0, sizeof(registers));
    registers[GEM_MODULE_ID_REGISTER / 4] = GEM_MODULE_ID;
    registers[GEM_DESIGN_CONFIG_6 / 4] = GEM_DESIGN_DMA_64;
    registers[GEM_NETWORK_STATUS / 4] = GEM_STATUS_MDIO_IDLE;
    registers[GEM_MAC_ADDRESS_LOW / 4] = 0x12005452;
    registers[GEM_MAC_ADDRESS_HIGH / 4] = 0x5734;
    cfg_status = GEM_CFG_LINK_UP;
    sync_calls = map_calls = fail_sync = fail_map = 0;
    dma_available = true;
    dma_submitted = false;
}

static void receive_frame(const void *data, unsigned bytes, uint32_t flags)
{
    memcpy(rx_frame, data, bytes);
    rx.descriptor.control = bytes | flags;
    rx.descriptor.address_low |= GEM_RX_OWNED;
}

/* Exercise the real adapter and registry with the same MMIO and DMA model as the frame tests. */
static void exercise_netif(void)
{
    unsigned char frame[GEM_FRAME_MAX_BYTES], out[GEM_FRAME_MAX_BYTES];
    for (unsigned i = 0; i < sizeof(frame); ++i)
        frame[i] = (unsigned char)(i * 13u + 7u);
    T(lume_netif_count() == 0);
    T(lume_device_attach(&gem_netif_device) == -LUME_EIO);
    T(lume_netif_count() == 0 && lume_device_count() == 0);
    reset_model();
    T(lume_example_gem_configure(0, false) == 0);
    T(lume_example_gem_eth_start() == 0);
    T(lume_device_attach(&gem_netif_device) == 0);
    T(lume_device_count() == 2 && lume_netif_count() == 1);
    T(lume_device_attach(&gem_netif_device) == -LUME_EINVAL);
    struct lume_netif *interface = lume_netif_find(1);
    struct lume_netif_info info = {0};
    lume_netif_info(interface, &info);
    T(interface && info.ifindex == 1 && !strcmp(info.name, "eth0"));
    T(info.kind == LUME_IF_ETHERNET && info.mtu == 1500);
    T(info.flags == (LUME_IF_UP | LUME_IF_LINK));
    const unsigned char mac[6] = {0x52, 0x54, 0, 0x12, 0x34, 0x57};
    T(!memcmp(info.mac, mac, sizeof(mac)));

    T(lume_netif_transmit(interface, frame, 60) && dma_submitted);
    frame[0] ^= 0xff;
    T(tx_frame[0] != frame[0]);
    T(!lume_netif_transmit(interface, frame, 60));
    lume_netif_info(interface, &info);
    T(info.tx_packets == 1 && info.tx_errors == 1);
    tx.descriptor.control |= GEM_TX_USED;
    registers[GEM_TX_STATUS / 4] = GEM_TX_STATUS_COMPLETE;
    lume_netif_poll(interface);
    T(!gem_network.tx_pending);
    T(lume_netif_transmit(interface, frame, 14));
    T((tx.descriptor.control & GEM_DESCRIPTOR_LENGTH) == 60);
    tx.descriptor.control |= GEM_TX_USED;
    registers[GEM_TX_STATUS / 4] = GEM_TX_STATUS_UNDERRUN;
    lume_netif_poll(interface);
    lume_netif_info(interface, &info);
    T(info.tx_packets == 2 && info.tx_errors == 2);
    lume_netif_poll(interface);
    lume_netif_tx_error(0);
    lume_netif_info(interface, &info);
    T(info.tx_errors == 2);

    receive_frame(frame, 64, GEM_RX_START | GEM_RX_END);
    T(lume_netif_receive(interface, out, sizeof(out)) == 64);
    T(!memcmp(frame, out, 64));
    receive_frame(frame, 60, GEM_RX_START);
    T(lume_netif_receive(interface, out, sizeof(out)) == -LUME_EIO);
    lume_netif_info(interface, &info);
    T(info.rx_packets == 1 && info.rx_errors == 1);
    cfg_status = 0;
    lume_netif_poll(interface);
    lume_netif_info(interface, &info);
    T(info.flags == LUME_IF_UP);
    T(!lume_netif_transmit(interface, frame, 60));
    cfg_status = GEM_CFG_STATUS_RGMII_LINK_STATUS;
    lume_netif_poll(interface);

    T(lume_netif_admin(interface, false) == 0);
    T(lume_device_count() == 1 && lume_netif_count() == 1);
    lume_netif_info(interface, &info);
    T(!info.flags && info.ifindex == 1);
    T(lume_netif_receive(interface, out, sizeof(out)) == 0);
    T(!lume_netif_transmit(interface, frame, 60));
    dma_available = false;
    T(lume_netif_admin(interface, true) == -LUME_EIO);
    T(lume_device_count() == 1);
    lume_netif_info(interface, &info);
    T(!info.flags);
    dma_available = true;
    T(lume_netif_admin(interface, true) == 0);
    T(lume_device_count() == 2 && lume_netif_count() == 1);

    T(lume_netif_transmit(interface, frame, 60));
    lume_netif_info(interface, &info);
    uint64_t errors = info.tx_errors;
    T(lume_netif_admin(interface, false) == 0);
    lume_netif_info(interface, &info);
    T(info.tx_errors == errors + 1 && !gem_network.tx_pending);
    const unsigned char changed_mac[6] = {0x52, 0x54, 0, 0x12, 0x34, 0x58};
    T(lume_example_gem_configure(changed_mac, false) == 0);
    T(lume_netif_admin(interface, true) == -LUME_EIO);
    T(lume_device_count() == 1);
    lume_netif_info(interface, &info);
    T(!info.flags && !memcmp(info.mac, mac, sizeof(mac)));
    T(lume_example_gem_configure(mac, false) == 0);
    T(lume_netif_admin(interface, true) == 0);

    T(lume_device_detach(&gem_netif_device) == 0);
    T(lume_device_count() == 0 && lume_netif_find(1) == interface);
    T(lume_netif_admin(interface, true) == -LUME_EIO);
    T(lume_device_count() == 0);
    T(lume_example_gem_eth_start() == 0);
    T(lume_device_attach(&gem_netif_device) == 0);
    T(lume_netif_count() == 1 && lume_netif_find(1) == interface);
    registers[GEM_TX_STATUS / 4] |= GEM_TX_STATUS_ACTIVE;
    T(lume_netif_admin(interface, false) == -LUME_EIO);
    T(gem_device.state == LUME_DEVICE_FAILED && lume_device_count() == 2);
    registers[GEM_TX_STATUS / 4] &= ~GEM_TX_STATUS_ACTIVE;
    T(lume_netif_admin(interface, false) == 0);
    T(lume_device_count() == 1);
    T(lume_device_detach(&gem_netif_device) == 0);
    T(lume_device_count() == 0 && lume_netif_count() == 1);
    lume_netif_info(interface, &info);
    T(!info.flags);
}

int main(void)
{
    unsigned char frame[GEM_FRAME_BUFFER_BYTES], out[GEM_FRAME_BUFFER_BYTES];
    struct lume_example_gem_layout info;
    reset_model();
    T(lume_example_gem_transmit(frame, 60) == -LUME_EIO);
    T(lume_example_gem_receive(out, sizeof(out)) == -LUME_EIO);
    T(!lume_example_gem_layout(&info));
    registers[GEM_MODULE_ID_REGISTER / 4] = 0;
    T(lume_example_gem_eth_start() == -LUME_ENOENT);
    T(lume_device_count() == 0);
    reset_model();
    registers[GEM_DESIGN_CONFIG_6 / 4] = 0;
    T(lume_example_gem_eth_start() == -LUME_ENOSYS);
    reset_model();
    fail_map = 3;
    T(lume_example_gem_eth_start() == -LUME_EFAULT);
    T(lume_device_count() == 0);
    reset_model();
    registers[GEM_NETWORK_STATUS / 4] = 0;
    T(lume_example_gem_eth_start() == -LUME_EIO);
    reset_model();
    const unsigned char invalid_mac[6] = {1};
    const unsigned char valid[6] = {0x52, 0x54, 0, 0x12, 0x34, 0x57};
    T(lume_example_gem_configure(invalid_mac, false) == -LUME_EINVAL);
    T(lume_example_gem_configure(valid, false) == 0);
    T(lume_example_gem_eth_start() == 0);
    T(lume_example_gem_eth_start() == -LUME_EINVAL);
    T(lume_example_gem_configure(0, true) == -LUME_EBUSY);
    T(lume_example_gem_layout(&info));
    T(info.phy_id == 0x01410cc2 && !memcmp(info.mac, valid, 6));
    T(info.tx_descriptor_dma > UINT32_MAX && info.rx_frame_dma > UINT32_MAX);
    T(info.tx_descriptor_cpu != info.rx_descriptor_cpu && info.tx_frame_cpu != info.rx_frame_cpu);
    T(registers[GEM_TX_QUEUE_BASE_HIGH / 4] == 0x10);
    T(tx.descriptor.address_high == 0x10 && rx.descriptor.address_high == 0x10);
    T(tx.descriptor.control == (GEM_TX_USED | GEM_TX_WRAP));
    T((rx.descriptor.address_low & 3) == GEM_RX_WRAP);
    T(lume_example_gem_link());
    cfg_status = 0;
    T(!lume_example_gem_link());
    cfg_status = 1;
    for (unsigned i = 0; i < sizeof(frame); ++i)
        frame[i] = (unsigned char)(i * 17 + 3);
    T(lume_example_gem_transmit(0, 60) == -LUME_EINVAL);
    T(lume_example_gem_transmit(frame, 13) == -LUME_EINVAL);
    T(lume_example_gem_transmit(frame, 1515) == -LUME_EINVAL);
    T(lume_example_gem_transmit(frame, 14) == 0 && dma_submitted);
    T(!memcmp(tx_frame, frame, 14));
    for (unsigned i = 14; i < 60; ++i)
        T(tx_frame[i] == 0);
    frame[0] ^= 0xff;
    T(tx_frame[0] != frame[0]);
    T((tx.descriptor.control & GEM_DESCRIPTOR_LENGTH) == 60);
    T(lume_example_gem_transmit(frame, 60) == -LUME_EAGAIN);
    T(lume_example_gem_tx_complete() == -LUME_EAGAIN);
    tx.descriptor.control |= GEM_TX_USED;
    registers[GEM_TX_STATUS / 4] = GEM_TX_STATUS_COMPLETE | 1;
    T(lume_example_gem_tx_complete() == 0);
    T(lume_example_gem_transmit(frame, 1514) == 0);
    T(!memcmp(tx_frame, frame, 1514));
    tx.descriptor.control |= GEM_TX_USED;
    registers[GEM_TX_STATUS / 4] = 1u << 6;
    T(lume_example_gem_tx_complete() == -LUME_EIO);
    T(lume_example_gem_receive(out, sizeof(out)) == 0);
    receive_frame(frame, 1514, GEM_RX_START | GEM_RX_END);
    memset(out, 0xa5, sizeof(out));
    T(lume_example_gem_receive(out, sizeof(out)) == 1514);
    T(!memcmp(out, frame, 1514) && out[1514] == 0xa5);
    T(!(rx.descriptor.address_low & GEM_RX_OWNED));
    T(lume_example_gem_receive(out, sizeof(out)) == 0);
    receive_frame(frame, 100, GEM_RX_START | GEM_RX_END);
    T(lume_example_gem_receive(out, 60) == -LUME_E2BIG);
    T(!(rx.descriptor.address_low & GEM_RX_OWNED));
    receive_frame(frame, 60, GEM_RX_START);
    T(lume_example_gem_receive(out, sizeof(out)) == -LUME_EIO);
    receive_frame(frame, 1515, GEM_RX_START | GEM_RX_END);
    T(lume_example_gem_receive(out, sizeof(out)) == -LUME_EIO);
    fail_sync = sync_calls + 1;
    T(lume_example_gem_transmit(frame, 60) == -LUME_EIO);
    fail_sync = 0;
    T(lume_example_gem_eth_stop() == 0);
    T(lume_device_count() == 0 && !lume_example_gem_link());
    T(lume_example_gem_receive(out, sizeof(out)) == -LUME_EIO);
    T(lume_example_gem_configure(0, true) == 0);
    T(lume_example_gem_eth_start() == 0);
    T(lume_example_gem_layout(&info) && info.loopback);
    T(registers[GEM_NETWORK_CONTROL / 4] & GEM_CONTROL_LOCAL_LOOPBACK);
    T(lume_example_gem_eth_stop() == 0);
    printf("GEM polling and DMA ownership: %u checks PASS\n", checks);
    exercise_netif();
    printf("GEM netif lifecycle and frame path: %u total checks PASS\n", checks);
    return 0;
}
