/* The CPU controls Ethernet through memory-mapped registers. A register address selects
 * hardware, while a six-byte MAC address identifies a network station. DMA means Direct Memory
 * Access: the controller reads and writes RAM without copying each byte through a CPU
 * instruction. A descriptor is a small RAM record telling DMA where a frame buffer is and who
 * may use it. TX means transmit and RX means receive. This example implements the qemu,rp1-gem
 * binding and is not a validated physical-board driver.
 * C names beginning with GEM_ are constants or flags. Hexadecimal numbers start with 0x. A byte
 * contains eight bits. Expressions such as 1 << 3 move the value one three bit positions left,
 * producing a mask that selects bit 3.
 */

/* Test whether this header has already been included in the current compilation unit. */
#ifndef LUME_EXAMPLE_GEM_H
/* Mark the header as included. The matching endif prevents duplicate definitions. */
#define LUME_EXAMPLE_GEM_H
/* Provide the C names bool, true and false for yes/no values. */
#include <stdbool.h>
/* Provide size_t for byte counts and offsetof for checking structure field positions. */
#include <stddef.h>
/* Provide integer types with explicit widths and uintptr_t for integers that can hold pointers. */
#include <stdint.h>

/* CPU physical MMIO addresses in the qemu,rp1-gem binding. */
/* Start of the MAC controller register bank in the CPU physical address space. MAC here means
 * Media Access Controller. The UINT64_C macro gives the hexadecimal literal a 64-bit integer
 * type.
 */
#define GEM_MAC_BASE UINT64_C(0x1c00100000)
/* Start of the separate RP1 Ethernet configuration register bank. This is another MMIO address,
 * not the six-byte Ethernet MAC address.
 */
#define GEM_CFG_BASE UINT64_C(0x1c00104000)
/* Reserve 16384 bytes for each register bank. The u suffix makes this an unsigned integer. */
#define GEM_MMIO_BYTES 0x4000u
/* GIC interrupt ID 307 is SPI275 plus the 32 IDs reserved before the SPI range. This polling
 * example reserves the resource but leaves the device interrupt masked.
 */
#define GEM_IRQ 307u
/* Expected module identification value of this modeled controller. It identifies the controller
 * implementation, not a network station.
 */
#define GEM_MODULE_ID UINT32_C(0x00020118)
/* MDIO bus address of the PHY in this device binding. */
#define GEM_PHY_MDIO_ADDRESS 0u

/* Byte offsets from GEM_MAC_BASE. Access registers as aligned 32-bit words. */
/* Associate readable C names with byte offsets. An enum defines numbers and does not read
 * hardware by itself. A register address is mac_regs plus one of these offsets.
 */
enum gem_register
{
    /* Enable RX, TX and MDIO, request transmission, or select local loopback. */
    GEM_NETWORK_CONTROL = 0x000,
    /* Configure link mode and received-frame formatting, including whether FCS is removed. */
    GEM_NETWORK_CONFIG = 0x004,
    /* Read live controller status. This example checks the MDIO-idle bit here. */
    GEM_NETWORK_STATUS = 0x008,
    /* Select DMA address width, RX buffer size and DMA burst configuration. */
    GEM_DMA_CONFIG = 0x010,
    /* Read TX completion/error flags. Writing a one clears the corresponding writable status
     * bit.
     */
    GEM_TX_STATUS = 0x014,
    /* Low address word of the RX descriptor ring in DMA address space. This points to
     * descriptors rather than directly to received bytes.
     */
    GEM_RX_QUEUE_BASE = 0x018,
    /* Low address word of the TX descriptor ring in DMA address space. */
    GEM_TX_QUEUE_BASE = 0x01c,
    /* Read reception events and clear acknowledged flags by writing ones. */
    GEM_RX_STATUS = 0x020,
    /* Read pending MAC events. Reading this modeled register also acknowledges its read-to-clear
     * events.
     */
    GEM_INTERRUPT_STATUS = 0x024,
    /* Write a bit mask to enable selected MAC interrupt sources. */
    GEM_INTERRUPT_ENABLE = 0x028,
    /* Write a bit mask to disable selected MAC interrupt sources. */
    GEM_INTERRUPT_DISABLE = 0x02c,
    /* Read which MAC interrupt sources are masked. */
    GEM_INTERRUPT_MASK = 0x030,
    /* Issue MDIO commands and read their returned 16-bit PHY data. */
    GEM_PHY_MAINTENANCE = 0x034,
    /* Lower half of the address hash filter. This example uses a specific station address
     * instead.
     */
    GEM_HASH_LOW = 0x080,
    /* Upper half of the address hash filter. */
    GEM_HASH_HIGH = 0x084,
    /* First four bytes of the station Ethernet address, packed into one 32-bit register. */
    GEM_MAC_ADDRESS_LOW = 0x088,
    /* Last two bytes of the station Ethernet address. Programming this word activates the
     * modeled specific-address filter.
     */
    GEM_MAC_ADDRESS_HIGH = 0x08c,
    /* Read the implementation identifier. For this binding the expected value is GEM_MODULE_ID. */
    GEM_MODULE_ID_REGISTER = 0x0fc,
    /* Read implemented hardware capabilities. Bit 23 advertises 64-bit DMA addressing. */
    GEM_DESIGN_CONFIG_6 = 0x294,
    /* Upper 32 bits of the TX descriptor ring DMA address. These bits extend TX_QUEUE_BASE. */
    GEM_TX_QUEUE_BASE_HIGH = 0x4c8,
    /* Upper 32 bits of the RX descriptor ring DMA address. These bits extend RX_QUEUE_BASE. */
    GEM_RX_QUEUE_BASE_HIGH = 0x4d4,
};

/* ETH_CFG is a separate bank. These aliases do not apply to MAC registers. */
/* Offsets for the separate configuration bank. Add these numbers to cfg_regs rather than
 * mac_regs.
 */
enum gem_cfg_register
{
    /* General Ethernet wrapper control. Writable fields reset to zero. */
    GEM_CFG_CONTROL = 0x00,
    /* Read-only AXI transaction errors and RGMII link, speed and duplex indications. */
    GEM_CFG_STATUS = 0x04,
    /* Low word of the optional timestamp timer. The current model returns zero. */
    GEM_CFG_TSU_TIMER_0 = 0x08,
    /* Middle word of that timer. Timestamping is not used by this example. */
    GEM_CFG_TSU_TIMER_1 = 0x0c,
    /* Upper timer bits. The current model does not implement this timer. */
    GEM_CFG_TSU_TIMER_2 = 0x10,
    /* Clock-generation configuration. The model retains supported fields without simulating
     * electrical clocks.
     */
    GEM_CFG_CLOCK_GENERATOR = 0x14,
    /* Select a clock source for the fabric-facing clock output. */
    GEM_CFG_CLOCK_TO_FABRIC = 0x18,
    /* Read raw wrapper event sources, including the MAC Ethernet interrupt output. */
    GEM_CFG_INTERRUPT_RAW = 0x1c,
    /* Mask that selects which raw wrapper event sources may reach the interrupt controller. */
    GEM_CFG_INTERRUPT_ENABLE = 0x20,
    /* Force selected wrapper events for diagnostics. */
    GEM_CFG_INTERRUPT_FORCE = 0x24,
    /* Read the result of combining raw, enabled and forced wrapper events. */
    GEM_CFG_INTERRUPT_STATUS = 0x28,
    /* Add this offset to a writable ETH_CFG register address to toggle the bits written as one. */
    GEM_CFG_ALIAS_XOR = 0x1000,
    /* Add this offset to set selected bits without rewriting the other bits. */
    GEM_CFG_ALIAS_SET = 0x2000,
    /* Add this offset to clear selected bits without rewriting the other bits. */
    GEM_CFG_ALIAS_CLEAR = 0x3000,
};

/* CONTROL fields are read/write. Bits 31:5 are reserved. */
enum gem_cfg_control_fields
{
    /* Power down the Ethernet memories. */
    GEM_CFG_CONTROL_MEM_PD = 1u << 4,
    /* Forward MAC bus errors to the fabric, including accesses to unmapped addresses. */
    GEM_CFG_CONTROL_BUSERR_EN = 1u << 3,
    /* Drive gem_tsu_inc_ctrl[1:0] with the selected increment control. */
    GEM_CFG_CONTROL_TSU_INC_CTRL_SHIFT = 1,
    GEM_CFG_CONTROL_TSU_INC_CTRL_MASK = 0x3u << 1,
    /* Drive the timestamp unit master/slave selection pin. */
    GEM_CFG_CONTROL_TSU_MS = 1u << 0,
};

/* STATUS is read-only. RGMII_SPEED resets to 2 and the remaining fields reset to zero. */
enum gem_cfg_status_fields
{
    /* An AXI write burst exceeded 16 beats. Recovery requires a block reset. */
    GEM_CFG_STATUS_AWLEN_ILLEGAL = 1u << 5,
    /* An AXI read burst exceeded 16 beats. Recovery requires a block reset. */
    GEM_CFG_STATUS_ARLEN_ILLEGAL = 1u << 4,
    GEM_CFG_STATUS_RGMII_DUPLEX = 1u << 3,
    GEM_CFG_STATUS_RGMII_SPEED_SHIFT = 1,
    GEM_CFG_STATUS_RGMII_SPEED_MASK = 0x3u << 1,
    GEM_CFG_STATUS_RGMII_LINK_STATUS = 1u << 0,
    GEM_CFG_LINK_UP = GEM_CFG_STATUS_RGMII_LINK_STATUS,
};

/* Decoded speed values shared by STATUS and CLKGEN. Value 3 is reserved. */
enum gem_cfg_speed
{
    GEM_CFG_SPEED_10_MBPS = 0,
    GEM_CFG_SPEED_100_MBPS = 1,
    GEM_CFG_SPEED_1000_MBPS = 2,
};

/* TSU_TIMER_0 and TSU_TIMER_1 contain full 32-bit words of the read-only 94-bit counter. */
enum gem_cfg_tsu_timer_fields
{
    /* TSU_TIMER_2 contains counter bits 93:64. Register bits 31:30 are reserved. */
    GEM_CFG_TSU_TIMER_2_COUNT_SHIFT = 0,
    GEM_CFG_TSU_TIMER_2_COUNT_MASK = 0x3fffffffu,
};

enum gem_cfg_clock_generator_fields
{
    /* Add delay to the RGMII transmit clock. */
    GEM_CFG_CLOCK_GENERATOR_TXCLKDELEN = 1u << 9,
    /* Correct the duty cycle for odd clock divisors. */
    GEM_CFG_CLOCK_GENERATOR_DC50 = 1u << 8,
    /* Start or stop the clock generator cleanly. This bit resets to one. */
    GEM_CFG_CLOCK_GENERATOR_ENABLE = 1u << 7,
    /* Stop the clock generator asynchronously. */
    GEM_CFG_CLOCK_GENERATOR_KILL = 1u << 6,
    /* Read-only speed reported by the MAC. */
    GEM_CFG_CLOCK_GENERATOR_SPEED_FROM_MAC_SHIFT = 4,
    GEM_CFG_CLOCK_GENERATOR_SPEED_FROM_MAC_MASK = 0x3u << 4,
    /* Select SPEED_OVERRIDE instead of the MAC speed. */
    GEM_CFG_CLOCK_GENERATOR_SPEED_OVERRIDE_ENABLE = 1u << 3,
    GEM_CFG_CLOCK_GENERATOR_SPEED_OVERRIDE_SHIFT = 0,
    GEM_CFG_CLOCK_GENERATOR_SPEED_OVERRIDE_MASK = 0x3u << 0,
};

enum gem_cfg_clock_to_fabric_fields
{
    GEM_CFG_CLOCK_TO_FABRIC_SELECT_SHIFT = 0,
    GEM_CFG_CLOCK_TO_FABRIC_SELECT_MASK = 0x3u << 0,
    GEM_CFG_CLOCK_TO_FABRIC_NONE = 0,
    GEM_CFG_CLOCK_TO_FABRIC_TX = 1,
    GEM_CFG_CLOCK_TO_FABRIC_RX = 2,
};

/* INTR, INTE, INTF and INTS share these bit positions. INTE and INTF are read/write.
 * INTR bits 12:1 are write-one-to-clear and bit 0 is read-only. INTS is read-only.
 */
enum gem_cfg_interrupt_fields
{
    GEM_CFG_INTERRUPT_IEEE1588_TSU_TIMER_CMP_VAL = 1u << 12,
    GEM_CFG_INTERRUPT_IEEE1588_SOF_RX = 1u << 11,
    GEM_CFG_INTERRUPT_IEEE1588_SYNC_FRAME_RX = 1u << 10,
    GEM_CFG_INTERRUPT_IEEE1588_DELAY_REQ_RX = 1u << 9,
    GEM_CFG_INTERRUPT_IEEE1588_PDELAY_REQ_RX = 1u << 8,
    GEM_CFG_INTERRUPT_IEEE1588_PDELAY_RESP_RX = 1u << 7,
    GEM_CFG_INTERRUPT_IEEE1588_SOF_TX = 1u << 6,
    GEM_CFG_INTERRUPT_IEEE1588_SYNC_FRAME_TX = 1u << 5,
    GEM_CFG_INTERRUPT_IEEE1588_DELAY_REQ_TX = 1u << 4,
    GEM_CFG_INTERRUPT_IEEE1588_PDELAY_REQ_TX = 1u << 3,
    GEM_CFG_INTERRUPT_IEEE1588_PDELAY_RESP_TX = 1u << 2,
    GEM_CFG_INTERRUPT_WOL = 1u << 1,
    /* Route the MAC Ethernet interrupt output. */
    GEM_CFG_INTERRUPT_ETHERNET = 1u << 0,
};

/* ETH fields describe the GEM register layout. Optional functions depend on hardware capabilities. */
enum gem_control_fields
{
    /* Send TX frames to this controller's RX path. */
    GEM_CONTROL_LOCAL_LOOPBACK = 1u << 1,
    /* Allow the controller to receive frames into RX buffers. */
    GEM_CONTROL_RX_ENABLE = 1u << 2,
    /* Allow the controller to read TX descriptors and send frames. */
    GEM_CONTROL_TX_ENABLE = 1u << 3,
    /* Enable the PHY management interface. */
    GEM_CONTROL_MDIO_ENABLE = 1u << 4,
    GEM_CONTROL_CLEAR_STATISTICS = 1u << 5,
    GEM_CONTROL_INCREMENT_STATISTICS = 1u << 6,
    GEM_CONTROL_STATISTICS_WRITE_ENABLE = 1u << 7,
    GEM_CONTROL_BACK_PRESSURE = 1u << 8,
    /* Notify the controller that a TX descriptor is ready. */
    GEM_CONTROL_TX_START = 1u << 9,
    /* Stop the transmit engine after its current frame. */
    GEM_CONTROL_TX_HALT = 1u << 10,
    GEM_CONTROL_TX_PAUSE_FRAME = 1u << 11,
    GEM_CONTROL_TX_ZERO_PAUSE_FRAME = 1u << 12,
    GEM_CONTROL_STATISTICS_TAKE_SNAPSHOT = 1u << 13,
    GEM_CONTROL_STATISTICS_READ_SNAPSHOT = 1u << 14,
    GEM_CONTROL_STORE_RX_TIMESTAMP = 1u << 15,
    GEM_CONTROL_PFC_ENABLE = 1u << 16,
    GEM_CONTROL_TX_PFC_PAUSE_FRAME = 1u << 17,
    GEM_CONTROL_FLUSH_RX_PACKET = 1u << 18,
    GEM_CONTROL_TX_LPI_ENABLE = 1u << 19,
    GEM_CONTROL_PTP_UNICAST_ENABLE = 1u << 20,
    GEM_CONTROL_ALT_SGMII_MODE = 1u << 21,
    GEM_CONTROL_STORE_UDP_OFFSET = 1u << 22,
    GEM_CONTROL_EXTERNAL_TSU_PORT_ENABLE = 1u << 23,
    GEM_CONTROL_ONE_STEP_SYNC_MODE = 1u << 24,
    GEM_CONTROL_PFC_CONTROL = 1u << 25,
    GEM_CONTROL_EXTERNAL_RX_QUEUE_SELECT = 1u << 26,
    GEM_CONTROL_ONE_STEP_CORRECTION_FIELD = 1u << 27,
    GEM_CONTROL_MII_ON_RGMII = 1u << 28,
    GEM_CONTROL_2500_MBPS = 1u << 29,
    GEM_CONTROL_IFG_QAV_CREDIT = 1u << 30,
};

enum gem_network_config_fields
{
    GEM_CONFIG_SPEED_100 = 1u << 0,
    /* Permit simultaneous transmission and reception. */
    GEM_CONFIG_FULL_DUPLEX = 1u << 1,
    GEM_CONFIG_DISCARD_NON_VLAN = 1u << 2,
    GEM_CONFIG_JUMBO_FRAMES = 1u << 3,
    GEM_CONFIG_COPY_ALL_FRAMES = 1u << 4,
    GEM_CONFIG_NO_BROADCAST = 1u << 5,
    GEM_CONFIG_MULTICAST_HASH_ENABLE = 1u << 6,
    GEM_CONFIG_UNICAST_HASH_ENABLE = 1u << 7,
    GEM_CONFIG_RECEIVE_1536_BYTES = 1u << 8,
    GEM_CONFIG_EXTERNAL_ADDRESS_MATCH = 1u << 9,
    /* Select the modeled 1 Gbit/s Ethernet mode. */
    GEM_CONFIG_GIGABIT = 1u << 10,
    GEM_CONFIG_PCS_SELECT = 1u << 11,
    GEM_CONFIG_RETRY_TEST = 1u << 12,
    GEM_CONFIG_PAUSE_ENABLE = 1u << 13,
    GEM_CONFIG_RX_BUFFER_OFFSET_SHIFT = 14,
    GEM_CONFIG_RX_BUFFER_OFFSET_MASK = 0x3u << 14,
    GEM_CONFIG_LENGTH_ERROR_DISCARD = 1u << 16,
    /* Remove the four-byte frame check sequence from received data. */
    GEM_CONFIG_REMOVE_FCS = 1u << 17,
    GEM_CONFIG_MDC_CLOCK_DIV_SHIFT = 18,
    GEM_CONFIG_MDC_CLOCK_DIV_MASK = 0x7u << 18,
    GEM_CONFIG_DATA_BUS_WIDTH_SHIFT = 21,
    GEM_CONFIG_DATA_BUS_WIDTH_MASK = 0x3u << 21,
    GEM_CONFIG_DISABLE_COPY_PAUSE = 1u << 23,
    GEM_CONFIG_RX_CHECKSUM_OFFLOAD = 1u << 24,
    GEM_CONFIG_HALF_DUPLEX_RX_ENABLE = 1u << 25,
    GEM_CONFIG_IGNORE_RX_FCS = 1u << 26,
    GEM_CONFIG_SGMII_MODE_ENABLE = 1u << 27,
    GEM_CONFIG_IPG_STRETCH_ENABLE = 1u << 28,
    GEM_CONFIG_ACCEPT_NONSTANDARD_PREAMBLE = 1u << 29,
    GEM_CONFIG_IGNORE_IPG_RX_ERROR = 1u << 30,
    GEM_CONFIG_UNIDIRECTIONAL_ENABLE = 1u << 31,
};

enum gem_network_status_fields
{
    GEM_STATUS_PCS_LINK = 1u << 0,
    GEM_STATUS_MDIO_INPUT = 1u << 1,
    /* The management interface can accept the next PHY command. */
    GEM_STATUS_MDIO_IDLE = 1u << 2,
};

enum gem_dma_config_fields
{
    GEM_DMA_BURST_LENGTH_SHIFT = 0,
    GEM_DMA_BURST_LENGTH_MASK = 0x1fu << 0,
    /* Encode a four-beat DMA burst. */
    GEM_DMA_BURST_INCR4 = 4u << GEM_DMA_BURST_LENGTH_SHIFT,
    GEM_DMA_HEADER_DATA_SPLIT_ENABLE = 1u << 5,
    GEM_DMA_DESCRIPTOR_ENDIAN_SWAP = 1u << 6,
    GEM_DMA_PACKET_ENDIAN_SWAP = 1u << 7,
    GEM_DMA_RX_PACKET_BUFFER_SIZE_SHIFT = 8,
    GEM_DMA_RX_PACKET_BUFFER_SIZE_MASK = 0x3u << 8,
    GEM_DMA_TX_PACKET_BUFFER_SIZE = 1u << 10,
    GEM_DMA_TX_CHECKSUM_OFFLOAD = 1u << 11,
    GEM_DMA_INFINITE_LAST_BUFFER_ENABLE = 1u << 12,
    GEM_DMA_CRC_ERROR_REPORT = 1u << 13,
    /* RX buffer size counts groups of 64 bytes. */
    GEM_DMA_RX_BUFFER_SHIFT = 16u,
    GEM_DMA_RX_BUFFER_MASK = 0xffu << 16,
    GEM_DMA_DISCARD_ON_ERROR = 1u << 24,
    GEM_DMA_FORCE_MAX_BURST_RX = 1u << 25,
    GEM_DMA_FORCE_MAX_BURST_TX = 1u << 26,
    GEM_DMA_RX_EXTENDED_DESCRIPTOR = 1u << 28,
    GEM_DMA_TX_EXTENDED_DESCRIPTOR = 1u << 29,
    /* Use DMA descriptors and queue addresses that can address memory above 4 GiB. */
    GEM_DMA_ADDRESS_64 = 1u << 30,
    GEM_DMA_BROADCAST_ALL_QUEUES = 1u << 31,
};

/* TX_STATUS records events cleared by writing ones. ACTIVE reports live engine activity. */
enum gem_tx_status_fields
{
    /* DMA reached a CPU-owned descriptor at the end of the submitted ring. */
    GEM_TX_STATUS_USED_BIT_READ = 1u << 0,
    GEM_TX_STATUS_COLLISION = 1u << 1,
    GEM_TX_STATUS_RETRY_LIMIT = 1u << 2,
    GEM_TX_STATUS_ACTIVE = 1u << 3,
    GEM_TX_STATUS_AMBA_ERROR = 1u << 4,
    GEM_TX_STATUS_COMPLETE = 1u << 5,
    GEM_TX_STATUS_UNDERRUN = 1u << 6,
    GEM_TX_STATUS_LATE_COLLISION = 1u << 7,
    GEM_TX_STATUS_RESPONSE_ERROR = 1u << 8,
    GEM_TX_STATUS_MAC_LOCKUP = 1u << 9,
    GEM_TX_STATUS_DMA_LOCKUP = 1u << 10,
    GEM_TX_STATUS_FRAME_TOO_LARGE = 1u << 11,
    GEM_TX_STATUS_USED_MIDFRAME = 1u << 12,
    /* Exclude ordinary end-of-ring USED_BIT_READ from the error mask. */
    GEM_TX_STATUS_ERRORS = GEM_TX_STATUS_RETRY_LIMIT | GEM_TX_STATUS_AMBA_ERROR |
        GEM_TX_STATUS_UNDERRUN | GEM_TX_STATUS_LATE_COLLISION | GEM_TX_STATUS_RESPONSE_ERROR |
        GEM_TX_STATUS_MAC_LOCKUP | GEM_TX_STATUS_DMA_LOCKUP | GEM_TX_STATUS_FRAME_TOO_LARGE |
        GEM_TX_STATUS_USED_MIDFRAME,
};

/* RX_STATUS events are cleared by writing ones to the corresponding bits. */
enum gem_rx_status_fields
{
    GEM_RX_STATUS_BUFFER_NOT_AVAILABLE = 1u << 0,
    GEM_RX_STATUS_FRAME_RECEIVED = 1u << 1,
    GEM_RX_STATUS_OVERRUN = 1u << 2,
    GEM_RX_STATUS_RESPONSE_ERROR = 1u << 3,
    GEM_RX_STATUS_MAC_LOCKUP = 1u << 4,
    GEM_RX_STATUS_DMA_LOCKUP = 1u << 5,
};

/* INTERRUPT_STATUS, INTERRUPT_ENABLE, INTERRUPT_DISABLE and INTERRUPT_MASK share these bits.
 * Reading INTERRUPT_STATUS acknowledges the read-to-clear events in this binding.
 * Write ones to INTERRUPT_ENABLE or INTERRUPT_DISABLE to change selected interrupt sources.
 */
enum gem_interrupt_fields
{
    GEM_IRQ_MDIO_COMPLETE = 1u << 0,
    GEM_IRQ_RX_COMPLETE = 1u << 1,
    GEM_IRQ_RX_USED = 1u << 2,
    GEM_IRQ_TX_USED = 1u << 3,
    GEM_IRQ_TX_UNDERRUN = 1u << 4,
    GEM_IRQ_RETRY_LIMIT = 1u << 5,
    GEM_IRQ_TX_AMBA_ERROR = 1u << 6,
    GEM_IRQ_TX_COMPLETE = 1u << 7,
    GEM_IRQ_USXGMII = 1u << 8,
    GEM_IRQ_LINK_CHANGE = 1u << 9,
    GEM_IRQ_RX_OVERRUN = 1u << 10,
    GEM_IRQ_RESPONSE_ERROR = 1u << 11,
    GEM_IRQ_PAUSE_FRAME_RX = 1u << 12,
    GEM_IRQ_PAUSE_TIME_ELAPSED = 1u << 13,
    GEM_IRQ_PAUSE_FRAME_TX = 1u << 14,
    GEM_IRQ_EXTERNAL = 1u << 15,
    GEM_IRQ_PCS_AUTONEG_COMPLETE = 1u << 16,
    GEM_IRQ_PCS_LINK_PARTNER_PAGE_RX = 1u << 17,
    GEM_IRQ_PTP_DELAY_REQ_RX = 1u << 18,
    GEM_IRQ_PTP_SYNC_RX = 1u << 19,
    GEM_IRQ_PTP_DELAY_REQ_TX = 1u << 20,
    GEM_IRQ_PTP_SYNC_TX = 1u << 21,
    GEM_IRQ_PTP_PDELAY_REQ_RX = 1u << 22,
    GEM_IRQ_PTP_PDELAY_RESP_RX = 1u << 23,
    GEM_IRQ_PTP_PDELAY_REQ_TX = 1u << 24,
    GEM_IRQ_PTP_PDELAY_RESP_TX = 1u << 25,
    GEM_IRQ_TSU_SECOND_INCREMENT = 1u << 26,
    GEM_IRQ_RX_LPI = 1u << 27,
    GEM_IRQ_WOL = 1u << 28,
    GEM_IRQ_TSU_TIMER = 1u << 29,
    GEM_IRQ_RX_LOCKUP = 1u << 30,
    GEM_IRQ_TX_LOCKUP = 1u << 31,
};

enum gem_phy_maintenance_fields
{
    GEM_PHY_DATA_SHIFT = 0,
    GEM_PHY_DATA_MASK = 0xffffu << 0,
    /* Clause 22 requires the code field to contain binary 10. */
    GEM_PHY_CODE_SHIFT = 16,
    GEM_PHY_CODE_MASK = 0x3u << 16,
    GEM_PHY_CODE_CLAUSE_22 = 0x2u << 16,
    GEM_PHY_REGISTER_SHIFT = 18,
    GEM_PHY_REGISTER_MASK = 0x1fu << 18,
    GEM_PHY_ADDRESS_SHIFT = 23,
    GEM_PHY_ADDRESS_MASK = 0x1fu << 23,
    GEM_PHY_OPERATION_SHIFT = 28,
    GEM_PHY_OPERATION_MASK = 0x3u << 28,
    GEM_PHY_OPERATION_WRITE = 0x1u << 28,
    GEM_PHY_OPERATION_READ = 0x2u << 28,
    /* Clause 22 requires the start-of-frame field to contain binary 01. */
    GEM_PHY_START_OF_FRAME_SHIFT = 30,
    GEM_PHY_START_OF_FRAME_MASK = 0x3u << 30,
    GEM_PHY_START_OF_FRAME_CLAUSE_22 = 0x1u << 30,
};

/* MAC_ADDRESS_LOW holds the first four address bytes. HASH_LOW and HASH_HIGH use all 32 bits. */
enum gem_mac_address_high_fields
{
    GEM_MAC_ADDRESS_HIGH_SHIFT = 0,
    GEM_MAC_ADDRESS_HIGH_MASK = 0xffffu << 0,
};

enum gem_module_id_fields
{
    GEM_MODULE_REVISION_SHIFT = 0,
    GEM_MODULE_REVISION_MASK = 0xffffu << 0,
    GEM_MODULE_ID_SHIFT = 16,
    GEM_MODULE_ID_MASK = 0xfffu << 16,
};

enum gem_design_config_6_fields
{
    /* The controller implements 64-bit DMA addresses. */
    GEM_DESIGN_DMA_64 = 1u << 23,
};

/* RX_QUEUE_BASE and TX_QUEUE_BASE contain aligned low address words. Their HIGH registers
 * contain the full upper 32 bits of each DMA address.
 */
enum gem_queue_base_fields
{
    GEM_QUEUE_BASE_ADDRESS_SHIFT = 2,
    GEM_QUEUE_BASE_ADDRESS_MASK = 0xfffffffcu,
};

/* DMA_ADDRESS_64 selects four little-endian words when timestamp extensions
 * are disabled. address_low/high hold a DMA bus address, not a CPU pointer. */
/* Describe one RAM record that the CPU and DMA engine exchange. A descriptor gives the hardware
 * a frame buffer address and ownership information. The frame bytes live elsewhere.
 */
struct gem_dma_descriptor
{
    /* Low 32 bits of the frame buffer DMA address. An RX descriptor also stores OWNED and WRAP
     * in its two lowest bits, which are otherwise zero because the buffer is aligned.
     */
    uint32_t address_low;
    /* TX stores length and submission flags here. RX writes completion flags and the received
     * frame length here. uint32_t always occupies four bytes.
     */
    uint32_t control;
    /* High 32 bits of the frame buffer DMA address. Combine this word shifted left by 32 with
     * the low address word to reconstruct the 64-bit address.
     */
    uint32_t address_high;
    /* Unused fourth word of this descriptor format. Aggregate initialization leaves it zero. */
    uint32_t reserved;
};

/* Fail compilation if the compiler layout is not exactly four 32-bit words. Hardware reads bytes
 * at fixed positions and cannot adapt to a different C layout.
 */
_Static_assert(sizeof(struct gem_dma_descriptor) == 16, "GEM descriptor size");
/* Check at compilation time that the control word starts four bytes after the descriptor base. */
_Static_assert(offsetof(struct gem_dma_descriptor, control) == 4, "GEM control offset");
/* Check that the upper address word starts eight bytes after the descriptor base. */
_Static_assert(offsetof(struct gem_dma_descriptor, address_high) == 8, "GEM high address offset");

/* TX USED=1 belongs to the CPU. Clear USED to submit the descriptor.
 * RX OWNED=1 belongs to the CPU. Clear OWNED to return it to DMA.
 * RX WRAP/OWNED share address_low with the aligned buffer address. */
/* TX ownership flag in descriptor.control. One lets the CPU modify or reuse the descriptor. Zero
 * hands it to the DMA engine.
 */
#define GEM_TX_USED (UINT32_C(1) << 31)
/* Make the transmit engine return to the ring base after this descriptor. */
#define GEM_TX_WRAP (UINT32_C(1) << 30)
/* Mark the end of a transmitted frame. This example puts each entire frame in a single
 * descriptor.
 */
#define GEM_TX_LAST (UINT32_C(1) << 15)
/* RX ownership flag in descriptor.address_low. Hardware sets it after receiving data. Software
 * clears it after consuming the data.
 */
#define GEM_RX_OWNED UINT32_C(1)
/* Make the receive engine return to the ring base after this descriptor. */
#define GEM_RX_WRAP UINT32_C(2)
/* Mark a descriptor containing the beginning of a received frame. */
#define GEM_RX_START (UINT32_C(1) << 14)
/* Mark a descriptor containing the end of a received frame. Both START and END must be present
 * for the single-buffer receive path.
 */
#define GEM_RX_END (UINT32_C(1) << 15)
/* Keep only the low 13 bits containing the byte length and discard the other flag bits. */
#define GEM_DESCRIPTOR_LENGTH UINT32_C(0x1fff)

/* Six destination bytes plus six source bytes plus two EtherType bytes occupy 14 bytes. */
#define GEM_FRAME_HEADER_BYTES 14u
/* Minimum frame bytes handled here, excluding the four-byte FCS. The driver pads shorter input
 * with zero bytes.
 */
#define GEM_FRAME_MIN_BYTES 60u
/* A 14-byte Ethernet header plus a 1500-byte payload. This example excludes VLAN and jumbo
 * frames.
 */
#define GEM_FRAME_MAX_BYTES 1514u
/* Tell the receive engine that its DMA buffer can hold 2048 bytes. */
#define GEM_FRAME_BUFFER_BYTES 2048u
/* Each unit in the DMA RX buffer-size field represents 64 bytes. */
#define GEM_DMA_RX_BUFFER_UNIT_BYTES 64u
/* Reserve and align each DMA object on a 4096-byte boundary. The actual frame may be much
 * shorter than its allocation.
 */
#define GEM_DMA_PAGE_BYTES 4096u

_Static_assert(GEM_FRAME_BUFFER_BYTES <= GEM_DMA_PAGE_BYTES, "GEM frame buffer allocation");
_Static_assert(GEM_FRAME_BUFFER_BYTES % GEM_DMA_RX_BUFFER_UNIT_BYTES == 0,
               "GEM RX buffer size granularity");
_Static_assert(GEM_FRAME_BUFFER_BYTES / GEM_DMA_RX_BUFFER_UNIT_BYTES <=
                   (GEM_DMA_RX_BUFFER_MASK >> GEM_DMA_RX_BUFFER_SHIFT),
               "GEM RX buffer size field");
_Static_assert(GEM_PHY_MDIO_ADDRESS <= (GEM_PHY_ADDRESS_MASK >> GEM_PHY_ADDRESS_SHIFT),
               "GEM MDIO PHY address field");

/* Ethernet bytes in RAM, without preamble or FCS. EtherType is in network order.
 * Payload follows this header. Short frames are padded to 60 bytes. */
/* Lay out the first 14 bytes of an ordinary Ethernet frame. Byte arrays avoid alignment and
 * host-endian conversions for the two-byte EtherType field.
 */
struct gem_ethernet_header
{
    /* Bytes 0 through 5 identify the receiving station or a broadcast/multicast destination. */
    uint8_t destination[6];
    /* Bytes 6 through 11 identify the station sending the frame. */
    uint8_t source[6];
    /* Bytes 12 and 13 identify the payload protocol. Store the most significant byte first. For
     * ARP these bytes are 0x08 and 0x06.
     */
    uint8_t ether_type_be[2];
};

/* Ensure the compiler inserts no extra bytes into the Ethernet header. */
_Static_assert(sizeof(struct gem_ethernet_header) == 14, "Ethernet header size");

/* CPU pointers identify identity-mapped kernel RAM. DMA addresses use the RP1
 * inbound aperture. Frames reside in tx_frame_cpu/rx_frame_cpu, not in MMIO.
 * Each descriptor and buffer occupies its own page to isolate cache lines. */
/* Return a description of the allocated memory. This structure contains addresses and
 * configuration metadata. It is not itself a hardware descriptor or an Ethernet frame.
 */
struct lume_example_gem_layout
{
    /* CPU addresses of the TX and RX descriptor records. uintptr_t can hold a pointer without
     * truncating it.
     */
    uintptr_t tx_descriptor_cpu, rx_descriptor_cpu;
    /* CPU addresses of the RAM arrays containing transmitted and received frame bytes. */
    uintptr_t tx_frame_cpu, rx_frame_cpu;
    /* Addresses that the DMA engine uses to read or update the descriptor records. The
     * queue-base registers receive these values.
     */
    uint64_t tx_descriptor_dma, rx_descriptor_dma;
    /* DMA addresses of the frame byte arrays. The address fields inside the descriptors receive
     * these values.
     */
    uint64_t tx_frame_dma, rx_frame_dma;
    /* The actual six-byte Ethernet station address. This value is separate from the address of
     * the MAC register bank.
     */
    unsigned char mac[6];
    /* Two 16-bit PHY identification registers combined into one value. The PHY is the device
     * that represents the physical Ethernet link.
     */
    uint32_t phy_id;
    /* Record whether local loopback is configured. In this mode the controller sends its own
     * transmitted frames back to its RX path.
     */
    bool loopback;
};

/* Calls run in serialized kernel context. Configure before attaching.
 * A null MAC uses the address currently programmed in hardware. */
/* Select the station address and loopback setting while detached. Zero means success. A negative
 * LUME_E value explains a rejected configuration.
 */
int lume_example_gem_configure(const unsigned char mac[6], bool loopback);
/* Attach the device through the DDK. The DDK validates resources, calls probe and then calls
 * start.
 */
int lume_example_gem_eth_start(void);
/* Detach through the DDK. The stop callback must disable device activity before its resources
 * can be released.
 */
int lume_example_gem_eth_stop(void);
/* Copy the active memory layout into a structure supplied by the caller. The asterisk denotes a
 * pointer to that structure.
 */
bool lume_example_gem_layout(struct lume_example_gem_layout *);
/* Return true when the example is running and the modeled Ethernet link reports that it is up. */
bool lume_example_gem_link(void);
/* Read one Clause 22 PHY register numbered 0 through 31. Write its 16-bit value through the
 * output pointer.
 */
int lume_example_gem_phy_read(unsigned reg, uint16_t *);
/* TX copies the caller's bytes. Zero means accepted, not transmitted.
 * EAGAIN means the single descriptor is still owned by DMA. */
/* Copy a complete Ethernet frame from the caller and submit it to DMA. const void * accepts
 * read-only bytes of any C object type.
 */
int lume_example_gem_transmit(const void *, size_t);
/* Check the outstanding TX operation without waiting. Return zero after completion or when there
 * is no pending operation, and negative EAGAIN while DMA still owns it.
 */
int lume_example_gem_tx_complete(void);
/* RX copies and consumes a frame, then rearms DMA. Zero means no frame.
 * E2BIG drops the frame when the caller's buffer is too short. */
/* Copy one completed RX frame into the caller buffer. Return its byte length, zero when no frame
 * is ready, or a negative error.
 */
int lume_example_gem_receive(void *, size_t);
/* End the conditional header guard opened by ifndef. */
#endif
