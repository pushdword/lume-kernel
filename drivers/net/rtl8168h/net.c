/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * RTL8168H register programming is based on the Linux r8169 driver.
 * Copyright (c) 2002 ShuChen <shuchen@realtek.com.tw>
 * Copyright (c) 2003 - 2007 Francois Romieu <romieu@fr.zoreil.com>
 * Copyright (c) a lot of people too. Please respect their work.
 */
#include "lume/counter.h"
#include "lume/cpu.h"
#include "lume/netif.h"
#include "lume/q35_module.h"
#include <string.h>

enum
{
    MAC = 0x00,
    TX_RING = 0x20,
    COMMAND = 0x37,
    TX_POLL = 0x38,
    IRQ_MASK = 0x3c,
    IRQ_STATUS = 0x3e,
    TX_CONFIG = 0x40,
    RX_CONFIG = 0x44,
    CONFIG_LOCK = 0x50,
    CONFIG_2 = 0x53,
    CONFIG_3 = 0x54,
    CONFIG_5 = 0x56,
    PHY_STATUS = 0x6c,
    ERI_DATA = 0x70,
    ERI_COMMAND = 0x74,
    EPHY = 0x80,
    MAC_OCP = 0xb0,
    PHY_OCP = 0xb8,
    MCU = 0xd3,
    RX_MAX = 0xda,
    CPLUS = 0xe0,
    IRQ_COALESCE = 0xe2,
    RX_RING = 0xe4,
    TX_MAX = 0xec,
    MISC = 0xf0,
    TX_AUTO_FIFO = 1u << 7,
    RX_DMA_GATE = 1u << 19,
    RX_FILTER = 0x3f,
    RX_ACCEPT = 0x0a,
    RESET = 16,
    TX_RX_ENABLE = 12,
    SYSTEM_ERROR = 1u << 15,
    MCU_OOB = 1u << 7,
    MCU_FIFO_READY = 1u << 1,
    RING_COUNT = 64,
    BUFFER_SIZE = 2048,
    RESET_US = 100000,
    REGISTER_US = 2000,
    TX_TIMEOUT_US = 1000000,
    RETRY_US = 1000000
};

#define PCI_ID UINT32_C(0x816810ec)
#define OWN UINT32_C(0x80000000)
#define WRAP UINT32_C(0x40000000)
#define WHOLE_FRAME UINT32_C(0x30000000)
#define RX_ERRORS UINT32_C(0x00780000)

struct descriptor
{
    volatile uint32_t status;
    uint32_t options;
    uint64_t address;
};

struct queues
{
    struct descriptor tx[RING_COUNT], rx[RING_COUNT];
    unsigned char output[RING_COUNT][BUFFER_SIZE], input[RING_COUNT][BUFFER_SIZE];
};

#define QUEUE_PAGES ((sizeof(struct queues) + 4095) / 4096)
_Static_assert(sizeof(struct descriptor) == 16, "Invalid RTL8168H descriptor size");
_Static_assert(offsetof(struct queues, rx) % 256 == 0, "Unaligned RTL8168H RX ring");

static struct
{
    volatile unsigned char *regs;
    struct queues *queues;
    uint64_t bus, retry_started, tx_time[RING_COUNT];
    uint32_t pci;
    unsigned rx, tx, first, pending;
    int interface;
    bool enabled, failed, master;
} nic;

static volatile uint8_t *reg8(unsigned offset)
{
    return nic.regs + offset;
}

static volatile uint16_t *reg16(unsigned offset)
{
    return (volatile uint16_t *)(nic.regs + offset);
}

static volatile uint32_t *reg32(unsigned offset)
{
    return (volatile uint32_t *)(nic.regs + offset);
}

static bool bus_master(bool enable)
{
    if (lume_q35_pci_read(nic.pci, 0) != PCI_ID)
        return false;
    uint32_t command = lume_q35_pci_read(nic.pci, 4) & 0xffff;
    lume_q35_pci_write(nic.pci, 4, (command & ~4u) | 0x402u | (enable ? 4u : 0u));
    return !!(lume_q35_pci_read(nic.pci, 4) & 4u) == enable;
}

static bool wait_flag(unsigned reg, bool set)
{
    uint64_t start = lume_counter_read();
    do
    {
        if (!!(*reg32(reg) & OWN) == set)
            return true;
        lume_cpu_relax();
    } while (lume_counter_read() - start < REGISTER_US);
    return false;
}

static bool wait8(unsigned reg, unsigned mask, unsigned value, unsigned timeout)
{
    uint64_t start = lume_counter_read();
    do
    {
        if ((*reg8(reg) & mask) == value)
            return true;
        lume_cpu_relax();
    } while (lume_counter_read() - start < timeout);
    return false;
}

static bool reset(void)
{
    if (lume_q35_pci_read(nic.pci, 0) != PCI_ID)
        return false;
    *reg16(IRQ_MASK) = 0;
    *reg32(RX_CONFIG) &= ~RX_FILTER;
    *reg32(MISC) |= RX_DMA_GATE;
    *reg8(COMMAND) = RESET;
    if (!wait8(COMMAND, RESET, 0, RESET_US))
        return false;
    *reg16(IRQ_STATUS) = UINT16_MAX;
    return true;
}

static bool eri_write(unsigned address, unsigned mask, uint32_t value)
{
    *reg32(ERI_DATA) = value;
    *reg32(ERI_COMMAND) = OWN | mask << 12 | address;
    return wait_flag(ERI_COMMAND, false);
}

static bool eri_update(unsigned address, uint32_t clear, uint32_t set)
{
    *reg32(ERI_COMMAND) = 15u << 12 | address;
    return wait_flag(ERI_COMMAND, true) &&
           eri_write(address, 15, (*reg32(ERI_DATA) & ~clear) | set);
}

static bool phy_write(unsigned address, unsigned value)
{
    *reg32(PHY_OCP) = OWN | address << 15 | value;
    return wait_flag(PHY_OCP, false);
}

static void mac_update(unsigned reg, unsigned clear, unsigned set)
{
    uint32_t address = (uint32_t)reg << 15;
    *reg32(MAC_OCP) = address;
    unsigned value = (*reg32(MAC_OCP) & 0xffff) & ~clear;
    *reg32(MAC_OCP) = OWN | address | value | set;
}

static bool leave_oob(void)
{
    if (!(*reg8(MCU) & MCU_OOB))
        return true;
    *reg8(MCU) &= ~MCU_OOB;
    mac_update(0xe8de, 1u << 14, 0);
    if (!wait8(MCU, MCU_FIFO_READY, MCU_FIFO_READY, 4200))
        return false;
    mac_update(0xe8de, 0, 1u << 15);
    return wait8(MCU, MCU_FIFO_READY, MCU_FIFO_READY, 4200);
}

static bool configure_mac(void)
{
    static const struct
    {
        uint8_t reg;
        uint16_t clear, set;
    } ephy[] = {{0x1e, 0x0800, 0x0001}, {0x1d, 0, 0x0800},      {0x05, 0xffff, 0x2089},
                {0x06, 0xffff, 0x5881}, {0x04, 0xffff, 0x854a}, {0x01, 0xffff, 0x068b}};

    /* Disable autonomous PCIe power saving before accessing the embedded PHY. */
    mac_update(0xe092, 0xff, 0);
    *reg8(CONFIG_2) &= ~0x80u;
    *reg8(CONFIG_3) &= ~0x02u;
    *reg8(CONFIG_5) &= ~0x01u;
    for (unsigned i = 0; i < sizeof(ephy) / sizeof(ephy[0]); ++i)
    {
        uint32_t request = (uint32_t)ephy[i].reg << 16;
        *reg32(EPHY) = request;
        if (!wait_flag(EPHY, true))
            return false;
        uint32_t value = (*reg32(EPHY) & 0xffff) & ~(uint32_t)ephy[i].clear;
        *reg32(EPHY) = OWN | request | value | ephy[i].set;
        if (!wait_flag(EPHY, false))
            return false;
    }
    if (!eri_write(0xc8, 15, 0x00080002) || !eri_write(0xe8, 15, 0x00100006) ||
        !eri_write(0xcc, 1, 0x38) || !eri_write(0xd0, 1, 0x48) || !eri_update(0xdc, 1, 0) ||
        !eri_update(0xdc, 0, 0x1d) || !eri_write(0x5f0, 3, 0x4f87) || !eri_write(0xc0, 3, 0) ||
        !eri_write(0xb8, 3, 0) || !eri_update(0x1b0, 1u << 12, 0))
        return false;

    *reg8(0xd0) &= ~0xc0u;
    *reg8(0xf2) &= ~0x40u;
    static const uint16_t changes[][3] = {{0xe056, 0x00f0, 0},
                                          {0xe052, 0x6000, 0x8008},
                                          {0xe0d6, 0x01ff, 0x017f},
                                          {0xd420, 0x0fff, 0x047f}};
    for (unsigned i = 0; i < sizeof(changes) / sizeof(changes[0]); ++i)
        mac_update(changes[i][0], changes[i][1], changes[i][2]);
    *reg32(MAC_OCP) = OWN | (0xe63eu << 15) | 1;
    *reg32(MAC_OCP) = OWN | (0xe63eu << 15);
    *reg32(MAC_OCP) = OWN | (0xc094u << 15);
    *reg32(MAC_OCP) = OWN | (0xc09eu << 15);

    return true;
}

static uint32_t wrap(unsigned index)
{
    return index == RING_COUNT - 1 ? WRAP : 0;
}

static void ring_address(unsigned reg, size_t offset)
{
    uint64_t bus = nic.bus + offset;
    *reg32(reg + 4) = (uint32_t)(bus >> 32);
    *reg32(reg) = (uint32_t)bus;
}

/* The MAC must acknowledge reset before either DMA ring can be reused. */
static bool start_rings(bool renegotiate)
{
    if (!leave_oob())
        return false;
    nic.rx = nic.tx = nic.first = nic.pending = 0;
    for (unsigned i = 0; i < RING_COUNT; ++i)
    {
        uint64_t offset = (uint64_t)i * BUFFER_SIZE;
        nic.queues->tx[i] =
            (struct descriptor){wrap(i), 0, nic.bus + offsetof(struct queues, output) + offset};
        nic.queues->rx[i] = (struct descriptor){OWN | wrap(i) | BUFFER_SIZE, 0,
                                                nic.bus + offsetof(struct queues, input) + offset};
    }
    *reg8(CONFIG_LOCK) = 0xc0;
    *reg16(CPLUS) = 0;
    *reg16(IRQ_COALESCE) = 0;
    *reg8(TX_MAX) = 0x27;
    bool configured = configure_mac();
    if (configured && renegotiate)
        configured =
            phy_write(0xa408, 0x01e1) && phy_write(0xa412, 0x0200) && phy_write(0xa400, 0x1200);
    ring_address(TX_RING, offsetof(struct queues, tx));
    ring_address(RX_RING, offsetof(struct queues, rx));
    *reg16(RX_MAX) = BUFFER_SIZE + 1;
    *reg8(CONFIG_LOCK) = 0;
    if (!configured)
        return false;
    (void)*reg8(COMMAND);
    lume_dma_write_complete();
    nic.master = true;
    if (!bus_master(true))
        return false;
    *reg8(COMMAND) = TX_RX_ENABLE;
    *reg32(TX_CONFIG) = (3u << 24) | (7u << 8) | TX_AUTO_FIFO;
    *reg32(RX_CONFIG) =
        (1u << 15) | (1u << 14) | (1u << 11) | (7u << 8) | (nic.enabled ? RX_ACCEPT : 0u);
    *reg32(MISC) &= ~RX_DMA_GATE;
    return true;
}

static void poll(void *context)
{
    (void)context;
    uint64_t now = lume_counter_read();
    struct lume_netif *netif = lume_netif_find((uint32_t)nic.interface);
    if (nic.failed)
    {
        if (now - nic.retry_started >= RETRY_US)
        {
            nic.retry_started = now;
            if (bus_master(false) && reset() && start_rings(false))
            {
                nic.failed = false;
                lume_netif_account(netif, LUME_NET_TX_RESTART, 1);
            }
        }
        return;
    }
    uint16_t status = *reg16(IRQ_STATUS);
    if (status)
        *reg16(IRQ_STATUS) = status;
    unsigned pending_before = nic.pending;
    while (nic.pending && !(nic.queues->tx[nic.first].status & OWN))
    {
        lume_netif_account(netif, LUME_NET_TX_COMPLETE, (now - nic.tx_time[nic.first]) / 1000u);
        nic.first = (nic.first + 1) % RING_COUNT;
        --nic.pending;
    }
    if (nic.pending != pending_before)
        lume_netif_account(netif, LUME_NET_TX_DEPTH, nic.pending);
    bool dma_error = (status & SYSTEM_ERROR) != 0;
    if (dma_error || (nic.pending && now - nic.tx_time[nic.first] >= TX_TIMEOUT_US))
    {
        nic.failed = true;
        nic.retry_started = now;
        (void)bus_master(false);
        lume_netif_account(netif, dma_error ? LUME_NET_TX_DMA_ERROR : LUME_NET_TX_TIMEOUT, 1);
        if (nic.pending > 1)
            lume_netif_account(netif, LUME_NET_TX_CANCELLED, nic.pending - 1);
        lume_netif_account(netif, LUME_NET_TX_DEPTH, 0);
        lume_value("RTL8168H_IRQ_ON_FAILURE", status);
        lume_value("RTL8168H_PHY_ON_FAILURE", *reg8(PHY_STATUS));
    }
}

static bool link(void *context)
{
    (void)context;
    uint8_t status = *reg8(PHY_STATUS);
    return nic.enabled && !nic.failed && status != UINT8_MAX && (status & 2u);
}

static bool admin(void *context, bool up)
{
    (void)context;
    nic.enabled = up;
    if (nic.failed)
    {
        if (up)
            nic.retry_started = lume_counter_read() - RETRY_US;
    }
    else
        *reg32(RX_CONFIG) = (*reg32(RX_CONFIG) & ~RX_FILTER) | (up ? RX_ACCEPT : 0u);
    return true;
}

static int receive(void *context, void *out, size_t capacity)
{
    (void)context;
    if (!nic.enabled || nic.failed)
        return 0;
    unsigned index = nic.rx;
    struct descriptor *desc = &nic.queues->rx[index];
    uint32_t status = desc->status;
    if (status & OWN)
        return 0;
    lume_dma_read_barrier();
    unsigned bytes = status & 0x3fff;
    int result = -LUME_EIO;
    if ((status & (WHOLE_FRAME | RX_ERRORS)) == WHOLE_FRAME && bytes >= 18 && bytes <= 1518 &&
        bytes - 4 <= capacity)
    {
        result = (int)bytes - 4;
        memcpy(out, nic.queues->input[index], (unsigned)result);
    }
    desc->options = 0;
    __atomic_store_n(&desc->status, OWN | wrap(index) | BUFFER_SIZE, __ATOMIC_RELEASE);
    nic.rx = (index + 1) % RING_COUNT;
    return result;
}

static bool transmit(void *context, const void *frame, size_t bytes)
{
    (void)context;
    if (!link(NULL) || bytes < 14 || bytes > 1514)
        return false;
    if (nic.pending == RING_COUNT)
    {
        poll(NULL);
        if (nic.failed || nic.pending == RING_COUNT)
            return false;
    }
    unsigned index = nic.tx;
    unsigned char *buffer = nic.queues->output[index];
    memcpy(buffer, frame, bytes);
    if (bytes < 60)
    {
        memset(buffer + bytes, 0, 60 - bytes);
        bytes = 60;
    }
    nic.tx_time[index] = lume_counter_read();
    ++nic.pending;
    nic.tx = (index + 1) % RING_COUNT;
    __atomic_store_n(&nic.queues->tx[index].status,
                     OWN | WHOLE_FRAME | wrap(index) | (uint32_t)bytes, __ATOMIC_RELEASE);
    lume_dma_write_complete();
    *reg8(TX_POLL) = 0x40;
    lume_netif_account(lume_netif_find((uint32_t)nic.interface), LUME_NET_TX_DEPTH, nic.pending);
    return true;
}

static int module_start(struct lume_hardware *hardware, void *context)
{
    (void)context;
    nic.interface = -1;
    int result = q35_module_pci(hardware, 0x10ec, 0x8168, &nic.pci);
    if (result)
        return result;
    uint32_t low = lume_q35_pci_read(nic.pci, 0x18);
    if ((low & 7) != 4)
        return -LUME_ENOENT;
    uint64_t base = (low & ~UINT32_C(15)) | (uint64_t)lume_q35_pci_read(nic.pci, 0x1c) << 32;
    const struct lume_resource *resource;
    unsigned r = 0;
    while ((resource = lume_hardware_resource(hardware, r++)))
        if (resource->base == base && resource->kind == LUME_RESOURCE_MMIO && resource->size >= 256)
            break;
    if (!resource || !base)
        return -LUME_EIO;
    uint32_t command = lume_q35_pci_read(nic.pci, 4) & 0xffff;
    lume_q35_pci_write(nic.pci, 4, command | 2u);
    nic.regs = (void *)(uintptr_t)base;
    if (((*reg32(TX_CONFIG) >> 20) & 0x7cf) != 0x541)
    {
        lume_q35_pci_write(nic.pci, 4, command);
        return -LUME_ENOENT;
    }
    if (!bus_master(false) || !reset())
        return -LUME_EIO;

    static const struct lume_netif_ops operations = {poll, link, receive, transmit, admin};
    struct lume_netif_descriptor descriptor = {
        .name = "eth0", .kind = LUME_IF_ETHERNET, .mtu = 1500, .ops = &operations};
    uint32_t mac_low = *reg32(MAC);
    uint16_t mac_high = *reg16(MAC + 4);
    if ((mac_low & 1u) || (!mac_low && !mac_high))
        return -LUME_EIO;
    memcpy(descriptor.mac, &mac_low, sizeof(mac_low));
    memcpy(descriptor.mac + sizeof(mac_low), &mac_high, sizeof(mac_high));

    struct lume_dma_domain domain;
    result = lume_hardware_dma(hardware, &domain);
    if (result)
        return result;
    uint64_t physical = lume_pages_alloc_span(QUEUE_PAGES);
    if (!physical)
        return -LUME_ENOMEM;
    nic.queues = (void *)(uintptr_t)physical;
    if (!lume_dma_map(&domain, physical, sizeof(*nic.queues), &nic.bus) || (nic.bus & 255))
        return -LUME_EIO;
    nic.enabled = true;
    if (!start_rings(true))
        return -LUME_EIO;
    result = lume_netif_register(&descriptor);
    if (result < 0)
        return result;
    nic.interface = result;
    return 0;
}

static int module_stop(struct lume_hardware *hardware, void *context)
{
    (void)hardware;
    (void)context;
    if (nic.interface >= 0)
    {
        int result = lume_netif_unregister((uint32_t)nic.interface);
        if (result)
            return result;
        nic.interface = -1;
    }
    if (nic.master && (!reset() || !bus_master(false)))
        return -LUME_EIO;
    if (nic.queues)
        for (unsigned i = 0; i < QUEUE_PAGES; ++i)
            (void)lume_page_free((uintptr_t)nic.queues + (uint64_t)i * 4096);
    memset(&nic, 0, sizeof(nic));
    nic.interface = -1;
    return 0;
}

Q35_MODULE("rtl8168h", LUME_HARDWARE_ID_PCI, "pci,10ec:8168", 1u << LUME_RESOURCE_MMIO);
