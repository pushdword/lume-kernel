/* Driver data flow: queue-base MMIO registers point to descriptor records in RAM. Each
 * descriptor points to a different RAM buffer containing Ethernet bytes. The CPU uses
 * identity-mapped pointers and the controller uses translated DMA bus addresses. Neither address
 * is the six-byte station MAC address.
 * Before TX, software copies and cleans the frame buffer, publishes a descriptor and requests
 * transmission. Before RX access, software observes CPU ownership and invalidates stale cached
 * bytes. After copying RX data, it returns the descriptor to DMA. These calls require serialized
 * kernel context. The example does not register a lwIP interface. These functions run inside
 * the kernel and are not system calls that a shell application can call directly.
 */

/* Single-queue polling example for qemu,rp1-gem. The module identity, fixed
 * resources and 1 Gbit/s configuration target this device binding. */
/* Import the register names, descriptor structures and public example functions defined in the
 * companion header.
 */
#include "gem.h"
/* Import symbolic error numbers such as LUME_EINVAL. A negative error number is returned to the
 * caller.
 */
#include <lume/abi.h>
/* Import the DDK device/resource structures and attach/detach operations. */
#include <lume/device.h>
/* Import DMA address translation and cache synchronization operations. */
#include <lume/dma.h>
/* Import ordered reads and writes of hardware registers. A plain pointer assignment would not
 * express all required ordering.
 */
#include <lume/mmio.h>

/* Bound busy-wait loops to 10000 register reads. This is an iteration limit, not a duration in
 * milliseconds.
 */
#define GEM_POLL_LIMIT 10000u
#define BCM54213PE_PHY_ID UINT32_C(0x600d84a2)
#define BCM_PHY_SHADOW 0x1cu
#define BCM_PHY_SHADOW_WRITE 0x8000u
#define BCM_PHY_SHADOW_DATA_MASK 0x03ffu
#define BCM_PHY_LED_SHADOW (0x0du << 10)
#define BCM_PHY_LED_FIELDS_MASK 0x00ffu
#define BCM_PHY_LED_MULTICOLOR_SOURCE 0x0au
#define BCM_PHY_EXP_SELECT 0x17u
#define BCM_PHY_EXP_DATA 0x15u
#define BCM_PHY_MULTICOLOR_REG 0x0f04u
#define BCM_PHY_LED_IN_PHASE 0x0100u
/* Declare the probe callback before its address is used in driver_ops. static makes the function
 * name private to this C file.
 */
static int probe(struct lume_device *);
/* Declare the callback that prepares buffers and enables the controller after a successful
 * probe.
 */
static int start(struct lume_device *);
/* Declare the callback that stops controller activity before the DDK releases its resource
 * claims.
 */
static int stop(struct lume_device *);

/* Describe the resources before attach runs. const prevents changing this table through this
 * declaration. The array exists for the lifetime of the kernel. Fields omitted from static
 * initializers are zero, including each resource flags field.
 */
static const struct lume_resource gem_resources[] = {
    /* Claim the MAC register window. A resource claim detects overlapping driver ownership and
     * does not allocate frame memory.
     */
    {.base = GEM_MAC_BASE, .size = GEM_MMIO_BYTES, .kind = LUME_RESOURCE_MMIO},
    /* Claim the separate Ethernet wrapper register window, including its atomic aliases. */
    {.base = GEM_CFG_BASE, .size = GEM_MMIO_BYTES, .kind = LUME_RESOURCE_MMIO},
    /* Claim one interrupt ID. The example uses polling, so claiming the ID does not enable it or
     * install an interrupt handler.
     */
    {.base = GEM_IRQ, .size = 1, .kind = LUME_RESOURCE_IRQ},
};
/* Give the DDK three function pointers. The fields on the left are callback names. The
 * identifiers on the right are the actual C functions.
 */
static const struct lume_driver_ops driver_ops = {.probe = probe, .start = start, .stop = stop};
/* Create the software device descriptor. Its state is mutable because the DDK records whether it
 * is detached, claimed, ready or failed. The omitted state field starts at zero, which is
 * LUME_DEVICE_OFF. A dot before a field name in an initializer selects that field explicitly.
 */
static struct lume_device gem_device = {
    /* Tell the DDK which device-structure contract this descriptor uses. */
    .version = LUME_DDK_VERSION,
    /* Classify the device as a network device. This classification alone does not register an
     * interface with lwIP.
     */
    .class_id = LUME_DEVICE_NETWORK,
    /* Give the device a short name in the DDK registry. */
    .name = "eth",
    /* Point to the resource array prepared above. */
    .resources = gem_resources,
    /* Divide the entire array size by one element size to obtain its number of entries. sizeof
     * counts bytes.
     */
    .resource_count = sizeof(gem_resources) / sizeof(gem_resources[0]),
    /* Store the address of the callback table. In C, the unary ampersand obtains an object
     * address.
     */
    .ops = &driver_ops,
};

/* Padding is outside the descriptor ring. Hardware wraps each ring
 * through WRAP. CPU-owned and DMA-owned objects never share a cache line. */
/* A union overlays its members at the same starting address. The descriptor occupies the first
 * 16 bytes and the page member reserves a complete 4096-byte region.
 */
union descriptor_page
{
    /* Provide named access to the descriptor words at the beginning of this allocation. */
    struct gem_dma_descriptor descriptor;
    struct gem_dma_descriptor descriptors[2];
    /* Reserve padding so this allocation cannot share a page with the next DMA object. */
    unsigned char page[GEM_DMA_PAGE_BYTES];
};
/* Keep the TX descriptor alive for the kernel lifetime and start it on a 4096-byte boundary.
 * Static storage is initially zero.
 */
_Alignas(GEM_DMA_PAGE_BYTES) static union descriptor_page tx;
/* Allocate a separately aligned RX descriptor. DMA can update RX without sharing a cache line
 * with TX.
 */
_Alignas(GEM_DMA_PAGE_BYTES) static union descriptor_page rx;
/* Allocate the RAM containing bytes to transmit. The CPU fills this buffer before handing its
 * descriptor to DMA.
 */
_Alignas(GEM_DMA_PAGE_BYTES) static unsigned char tx_frame[GEM_DMA_PAGE_BYTES];
/* Allocate the RAM into which DMA writes received bytes. Software reads it only after RX
 * ownership returns to the CPU.
 */
_Alignas(GEM_DMA_PAGE_BYTES) static unsigned char rx_frame[GEM_DMA_PAGE_BYTES];
/* Remember both CPU and DMA addresses of the four allocations, together with the current station
 * and PHY identifiers.
 */
static struct lume_example_gem_layout layout;
/* Hold CPU addresses of the two register banks. These integers describe register locations, not
 * Ethernet station addresses.
 */
static uintptr_t mac_regs, cfg_regs;
/* Keep the persistent network-control bits. The TX_START request is added only when submitting a
 * frame.
 */
static uint32_t control;
/* Remember whether hardware is enabled, whether a TX result is outstanding, and which
 * configuration choices were selected.
 */
static bool running, tx_pending, custom_mac, loopback;
/* Keep a private copy of an optional station address supplied before attachment. */
static unsigned char requested_mac[6];
static bool physical_rp1;
static unsigned phy_address = GEM_PHY_MDIO_ADDRESS;
static unsigned phy_led_modes[2];
static uint32_t network_config;
static bool phy_link_known;
static unsigned tx_index, tx_count;
static uint64_t tx_expected, tx_observed;

/* Read one 32-bit MAC register. The caller supplies an offset in bytes rather than a full
 * address.
 */
static uint32_t read_mac_reg(unsigned offset)
{
    /* Add the register-bank base and byte offset, then perform an ordered MMIO read and return
     * the register contents.
     */
    return lume_mmio_read32(mac_regs + offset);
}

/* Write one MAC register. void means the function does not return a result value. */
static void write_mac_reg(unsigned offset, uint32_t value)
{
    /* Perform an ordered register write and wait for the MMIO access to complete. Device
     * operations triggered by the write may still finish asynchronously.
     */
    lume_mmio_write32_sync(mac_regs + offset, value);
}

/* Read one 32-bit register from the ETH_CFG bank. */
static uint32_t read_cfg_reg(unsigned offset)
{
    return lume_mmio_read32(cfg_regs + offset);
}

/* Clear selected bits through the ETH_CFG atomic alias. MAC registers have no such alias. */
static void clear_cfg_bits(unsigned offset, uint32_t mask)
{
    lume_mmio_write32_sync(cfg_regs + GEM_CFG_ALIAS_CLEAR + offset, mask);
}

/* Validate the six-byte Ethernet station address. const means this function reads the bytes
 * without modifying them.
 */
static bool valid_mac(const unsigned char address[6])
{
    /* Start an accumulator used to detect the invalid all-zero station address. */
    unsigned any = 0;
    /* Visit the six address bytes. ++i advances the index by one after each iteration. */
    for (unsigned i = 0; i < 6; ++i)
        /* Bitwise OR each byte into the accumulator. The result stays zero only if every address
         * byte is zero.
         */
        any |= address[i];
    /* Require a nonzero address and a clear group bit. Bit zero of the first byte marks
     * multicast addresses. The exclamation mark negates the bit test.
     */
    return any && !(address[0] & 1);
}

/* Store configuration for the next attachment. The function name is externally visible because
 * it is not declared static.
 */
int lume_example_gem_configure(const unsigned char address[6], bool local_loopback)
{
    /* Reject changes while the DDK owns an attached or failed device. Changing its configuration
     * during DMA would break ownership assumptions.
     */
    if (gem_device.state != LUME_DEVICE_OFF)
        /* Report that the device must be detached before configuration can change. */
        return -LUME_EBUSY;
    /* A null pointer requests the current hardware address. Validate only when the caller
     * actually supplies six bytes.
     */
    if (address && !valid_mac(address))
        /* Report an invalid configuration argument. */
        return -LUME_EINVAL;
    /* Remember whether to use the caller address instead of reading the programmed hardware
     * address.
     */
    custom_mac = address != 0;
    /* Only dereference the address pointer when it is non-null. */
    if (address)
        /* Copy exactly the six bytes forming the Ethernet station address. */
        for (unsigned i = 0; i < 6; ++i)
            /* Store one byte in driver-owned memory so no pointer to temporary caller storage is
             * retained.
             */
            requested_mac[i] = address[i];
    /* Remember whether the next attachment should route TX back into RX. */
    loopback = local_loopback;
    physical_rp1 = false;
    phy_address = GEM_PHY_MDIO_ADDRESS;
#ifdef LUME_PLATFORM_LKM
    gem_device.resource_count = 0;
#else
    gem_device.resource_count = sizeof(gem_resources) / sizeof(gem_resources[0]);
#endif
    /* Return success after recording the configuration. Hardware has not been enabled here. */
    return 0;
}

int lume_example_gem_configure_rp1(const unsigned char address[6], unsigned phy,
                                   const unsigned led_modes[2])
{
    if (!address || phy > 31u || !led_modes || led_modes[0] > 9u || led_modes[1] > 9u)
        return -LUME_EINVAL;

    int result = lume_example_gem_configure(address, false);
    if (result)
        return result;

    physical_rp1 = true;
    phy_address = phy;
    phy_led_modes[0] = led_modes[0];
    phy_led_modes[1] = led_modes[1];

    /* Polling leaves the RP1 interrupt domain unclaimed and masked. */
#ifdef LUME_PLATFORM_LKM
    gem_device.resource_count = 0;
#else
    gem_device.resource_count = 2;
#endif
    return 0;
}

/* Check the claimed resources and controller identity. The DDK calls this before start and
 * passes a pointer to its device descriptor.
 */
static int probe(struct lume_device *d)
{
    /* Find the first MMIO resource. The returned pointer is const because the claimed resource
     * description must remain unchanged.
     */
#ifdef LUME_PLATFORM_LKM
    const struct lume_resource *mac = &gem_resources[0];
    const struct lume_resource *cfg = &gem_resources[1];
#else
    const struct lume_resource *mac = lume_device_resource(d, LUME_RESOURCE_MMIO, 0);
    /* Find the second MMIO resource, which describes the separate ETH_CFG bank. */
    const struct lume_resource *cfg = lume_device_resource(d, LUME_RESOURCE_MMIO, 1);
#endif
    /* Require both banks and sufficient reserved space. The arrow operator reads a structure
     * field through a pointer.
     */
    if (!mac || !cfg || mac->size < GEM_MMIO_BYTES || cfg->size < GEM_MMIO_BYTES)
        /* Reject an incomplete resource description before reading hardware. */
        return -LUME_EINVAL;
    /* Store the MAC bank address in the integer type used for CPU register addresses. The kernel
     * already maps this physical window for MMIO.
     */
    mac_regs = (uintptr_t)mac->base;
    /* Store the configuration-bank address separately so later offsets select the correct bank. */
    cfg_regs = (uintptr_t)cfg->base;
    /* Read base plus 0xfc and require the controller identity implemented by this binding. */
    if (read_mac_reg(GEM_MODULE_ID_REGISTER) != (physical_rp1 ? GEM_RP1_MODULE_ID : GEM_MODULE_ID))
    {
        /* Clear both saved addresses after a failed identity/capability check. Rollback can then
         * avoid accessing an unsupported register bank.
         */
        mac_regs = cfg_regs = 0;
        /* Report that the expected controller was not found at this resource address. */
        return -LUME_ENOENT;
    }
    /* Mask the design capability register to test its 64-bit DMA bit. A clear bit means these
     * descriptor addresses cannot be used.
     */
    if (!(read_mac_reg(GEM_DESIGN_CONFIG_6) & GEM_DESIGN_DMA_64))
    {
        /* Clear both saved addresses after a failed identity/capability check. Rollback can then
         * avoid accessing an unsupported register bank.
         */
        mac_regs = cfg_regs = 0;
        /* Reject a controller that lacks the descriptor addressing mode required by this
         * implementation.
         */
        return -LUME_ENOSYS;
    }
    /* Accept this controller. The DDK may now call its start callback. */
    return 0;
}

/* Wait for the PHY management engine to become idle, with a fixed upper limit on the number of
 * polls.
 */
static int mdio_idle(void)
{
    /* Read status repeatedly up to the fixed poll limit, preventing an endless wait on an
     * unresponsive management engine.
     */
    for (unsigned i = 0; i < GEM_POLL_LIMIT; ++i)
        /* Test whether the status register says that no MDIO transaction is active. */
        if (read_mac_reg(GEM_NETWORK_STATUS) & GEM_STATUS_MDIO_IDLE)
            /* The management engine is idle and can accept another command. */
            return 0;
    /* The idle indication never appeared within the bounded polling loop. */
    return -LUME_EIO;
}

/* Read a PHY management register. reg selects the register and out points to caller-owned space
 * for its 16-bit result.
 */
static int phy_read(unsigned reg, uint16_t *out)
{
    /* Require writable result storage and a register number that fits the five-bit Clause 22
     * register-address field.
     */
    if (!out || reg > (GEM_PHY_REGISTER_MASK >> GEM_PHY_REGISTER_SHIFT))
        /* Reject an invalid output pointer or PHY register number. */
        return -LUME_EINVAL;
    /* Do not overwrite a management command that is still in progress. */
    int result = mdio_idle();
    /* Zero means success. Any nonzero result here is the negative error returned by mdio_idle. */
    if (result)
        /* Return either the earlier wait error or the final command status to the caller. */
        return result;
    /* Combine the Clause 22 frame format, read operation, code and MDIO addresses. */
    uint32_t command = GEM_PHY_START_OF_FRAME_CLAUSE_22 | GEM_PHY_OPERATION_READ |
                       GEM_PHY_CODE_CLAUSE_22 |
                       ((phy_address << GEM_PHY_ADDRESS_SHIFT) & GEM_PHY_ADDRESS_MASK) |
                       (((uint32_t)reg << GEM_PHY_REGISTER_SHIFT) & GEM_PHY_REGISTER_MASK);
    write_mac_reg(GEM_PHY_MAINTENANCE, command);
    /* Wait for the command triggered by the register write to finish. */
    result = mdio_idle();
    /* Only extract returned data after management access completed successfully. */
    if (!result)
        /* Read the command register, keep its low 16 data bits and store them through the caller
         * output pointer. The unary asterisk dereferences that pointer.
         */
        *out = (uint16_t)((read_mac_reg(GEM_PHY_MAINTENANCE) & GEM_PHY_DATA_MASK) >>
                          GEM_PHY_DATA_SHIFT);
    /* Return either the earlier wait error or the final command status to the caller. */
    return result;
}

static int phy_write(unsigned reg, uint16_t value)
{
    int result = mdio_idle();
    if (result)
        return result;
    uint32_t command = GEM_PHY_START_OF_FRAME_CLAUSE_22 | GEM_PHY_OPERATION_WRITE |
                       GEM_PHY_CODE_CLAUSE_22 | (phy_address << GEM_PHY_ADDRESS_SHIFT) |
                       (reg << GEM_PHY_REGISTER_SHIFT) | value;
    write_mac_reg(GEM_PHY_MAINTENANCE, command);
    return mdio_idle();
}

static int bcm54213_configure_leds(void)
{
    uint16_t value;
    int result = phy_write(BCM_PHY_SHADOW, BCM_PHY_LED_SHADOW);
    if (result)
        return result;

    result = phy_read(BCM_PHY_SHADOW, &value);
    if (result)
        return result;

    value = (uint16_t)((value & BCM_PHY_SHADOW_DATA_MASK & ~BCM_PHY_LED_FIELDS_MASK) |
                       BCM_PHY_LED_MULTICOLOR_SOURCE | (BCM_PHY_LED_MULTICOLOR_SOURCE << 4));

    result =
        phy_write(BCM_PHY_SHADOW, (uint16_t)(BCM_PHY_SHADOW_WRITE | BCM_PHY_LED_SHADOW | value));
    if (result)
        return result;

    result = phy_write(BCM_PHY_EXP_SELECT, BCM_PHY_MULTICOLOR_REG);
    if (result)
        return result;

    result = phy_read(BCM_PHY_EXP_DATA, &value);
    if (result)
        return result;

    value = (uint16_t)((value & ~BCM_PHY_LED_FIELDS_MASK) | BCM_PHY_LED_IN_PHASE |
                       phy_led_modes[0] | (phy_led_modes[1] << 4));

    return phy_write(BCM_PHY_EXP_DATA, value);
}

static bool rp1_link(void)
{
    uint16_t status, advertise, partner, gigabit, gigabit_partner;
    /* Read twice because the PHY latches a lost carrier until its status is read. */
    if (phy_read(1, &status) || phy_read(1, &status) || (status & 0x24) != 0x24)
    {
        phy_link_known = false;
        return false;
    }
    if (phy_link_known)
        return true;
    if (phy_read(4, &advertise) || phy_read(5, &partner) || phy_read(9, &gigabit) ||
        phy_read(10, &gigabit_partner))
        return false;
    uint32_t config =
        network_config & ~(GEM_CONFIG_SPEED_100 | GEM_CONFIG_GIGABIT | GEM_CONFIG_FULL_DUPLEX);
    uint16_t common = advertise & partner;
    if (((gigabit << 2) & gigabit_partner) & 0x0c00)
    {
        config |= GEM_CONFIG_GIGABIT;
        if (((gigabit << 2) & gigabit_partner) & 0x0800)
            config |= GEM_CONFIG_FULL_DUPLEX;
    }
    else if (common & 0x0180)
    {
        config |= GEM_CONFIG_SPEED_100;
        if (common & 0x0100)
            config |= GEM_CONFIG_FULL_DUPLEX;
    }
    else if (common & 0x0060)
    {
        if (common & 0x0040)
            config |= GEM_CONFIG_FULL_DUPLEX;
    }
    else
        return false;
    write_mac_reg(GEM_NETWORK_CONFIG, config);
    phy_link_known = true;
    return true;
}

/* Expose PHY reads to other kernel code while the example is running. */
int lume_example_gem_phy_read(unsigned reg, uint16_t *out)
{
    /* The conditional operator chooses one result. Perform the read when running, otherwise
     * report an I/O error without touching registers.
     */
    return running ? phy_read(reg, out) : -LUME_EIO;
}

/* Transfer cache visibility for a whole descriptor allocation. cpu selects the direction of
 * ownership transfer.
 */
static bool sync_descriptor(union descriptor_page *p, bool cpu)
{
    /* Before CPU access, invalidate stale cached descriptor data so updates written by DMA
     * become visible. sizeof(*p) describes the entire page-sized object.
     */
    return cpu ? lume_dma_sync_for_cpu(p, sizeof(*p), LUME_DMA_BIDIRECTIONAL)
               /* Before device access, clean and invalidate the descriptor cache lines.
                * BIDIRECTIONAL is required because the CPU and DMA both update descriptor words.
                */
               : lume_dma_sync_for_device(p, sizeof(*p), LUME_DMA_BIDIRECTIONAL);
}

/* Prepare the one-entry RX ring for another frame. Call this while the descriptor is CPU-owned
 * or before RX is first enabled.
 */
static bool receive_rearm(void)
{
    /* Prepare the entire isolated RX allocation for device writes. Cache cleaning/invalidation
     * must finish before ownership is returned to DMA.
     */
    if (!lume_dma_sync_for_device(rx_frame, sizeof(rx_frame), LUME_DMA_FROM_DEVICE))
        /* Report that the RX buffer could not be prepared safely. */
        return false;
    /* Replace the descriptor with a new aggregate value. Unspecified control and reserved words
     * become zero.
     */
    rx.descriptor = (struct gem_dma_descriptor){
        /* Keep the low address bits and set WRAP. OWNED stays zero, granting the RX slot to DMA.
         * The buffer alignment leaves the flag bits available.
         */
        .address_low = (uint32_t)layout.rx_frame_dma | GEM_RX_WRAP,
        /* Shift away the low 32 bits to store the upper half of the RX buffer DMA address. */
        .address_high = (uint32_t)(layout.rx_frame_dma >> 32),
    };
    /* Make the completed descriptor visible to the device after its frame buffer is prepared. */
    return sync_descriptor(&rx, false);
}

/* Translate the buffer addresses, initialize descriptor ownership, configure the MAC and finally
 * enable reception and transmission.
 */
static int start(struct lume_device *d)
{
    /* Explicitly acknowledge that this callback does not use its device parameter. Casting to
     * void avoids an unused-parameter warning.
     */
    (void)d;
    /* Prepare the RP1 inbound DMA window shared with display and USB. This maps bus addresses
     * into kernel RAM.
     */
    if (!lume_pi5_dma_start())
        /* Report a failed hardware or cache preparation step. The DDK invokes stop to roll back
         * failed initialization.
         */
        return -LUME_EIO;
    /* Disable RX and TX before changing descriptor addresses and mode bits. */
    write_mac_reg(GEM_NETWORK_CONTROL, 0);
    /* Write ones to all source positions to mask MAC interrupts. UINT32_MAX has all 32 bits set. */
    write_mac_reg(GEM_INTERRUPT_DISABLE, UINT32_MAX);
    /* Use the ETH_CFG CLEAR alias to disable only the wrapper Ethernet interrupt bit while
     * preserving other wrapper interrupt enables.
     */
    clear_cfg_bits(GEM_CFG_INTERRUPT_ENABLE, GEM_CFG_INTERRUPT_ETHERNET);
    /* Read and acknowledge existing modeled MAC events. The void cast explicitly discards the
     * returned status word.
     */
    (void)read_mac_reg(GEM_INTERRUPT_STATUS);
    /* Initialize an address snapshot. Unspecified fields, including all DMA addresses, start at
     * zero until translation succeeds.
     */
    layout = (struct lume_example_gem_layout){
        /* Record the CPU address of the TX descriptor allocation. The descriptor occupies the
         * allocation beginning.
         */
        .tx_descriptor_cpu = (uintptr_t)&tx,
        /* Record the separate CPU address of the RX descriptor allocation. */
        .rx_descriptor_cpu = (uintptr_t)&rx,
        /* An array expression yields a pointer to its first byte here. Save that pointer as a
         * CPU-address integer.
         */
        .tx_frame_cpu = (uintptr_t)tx_frame,
        /* Record where the CPU will find received frame bytes after DMA completion. */
        .rx_frame_cpu = (uintptr_t)rx_frame,
        /* Record the selected local-loopback setting in the public layout snapshot. */
        .loopback = loopback,
    };
    /* Translate the TX descriptor allocation from CPU physical RAM to the address space seen by
     * the DMA engine. Translation also checks the entire allocation against the DMA window.
     */
    if (!lume_dma_map(&lume_rp1_dma_domain, (uintptr_t)&tx, sizeof(tx),
                      /* Write the translated TX descriptor address into layout. Logical OR stops
                       * at the first failed translation.
                       */
                      &layout.tx_descriptor_dma) ||
        /* Translate the independently allocated RX descriptor page. */
        !lume_dma_map(&lume_rp1_dma_domain, (uintptr_t)&rx, sizeof(rx),
                      /* Store the RX ring DMA address, which will later be programmed into the
                       * RX queue registers.
                       */
                      &layout.rx_descriptor_dma) ||
        /* Translate the TX frame byte array. Its DMA address goes inside the TX descriptor
         * rather than into the queue-base registers.
         */
        !lume_dma_map(&lume_rp1_dma_domain, (uintptr_t)tx_frame, sizeof(tx_frame),
                      /* Store the DMA address of the private TX frame buffer. */
                      &layout.tx_frame_dma) ||
        /* Translate the RX frame byte array that the controller will write. */
        !lume_dma_map(&lume_rp1_dma_domain, (uintptr_t)rx_frame, sizeof(rx_frame),
                      /* Store the final DMA address. All four translations must succeed before
                       * any queue is enabled.
                       */
                      &layout.rx_frame_dma))
        /* Reject an allocation whose full span cannot be represented safely in the configured
         * DMA domain.
         */
        return -LUME_EFAULT;
    /* Read the existing station address from two registers. Four address bytes are in low and
     * the final two are in high.
     */
    uint32_t low = read_mac_reg(GEM_MAC_ADDRESS_LOW);
    uint32_t high = (read_mac_reg(GEM_MAC_ADDRESS_HIGH) & GEM_MAC_ADDRESS_HIGH_MASK) >>
                    GEM_MAC_ADDRESS_HIGH_SHIFT;
    /* Extract or replace each of the six station-address bytes. */
    for (unsigned i = 0; i < 6; ++i)
        /* Assign the current Ethernet station-address byte in the layout snapshot. */
        layout.mac[i] =
            /* Choose a supplied byte when configured. Otherwise select the low/high register and
             * shift the required byte into the least significant position. The cast retains only
             * that byte.
             */
            custom_mac ? requested_mac[i] : (unsigned char)((i < 4 ? low : high) >> (8 * (i % 4)));
    /* Check the address obtained from configuration or hardware before programming the station
     * filter.
     */
    if (!valid_mac(layout.mac))
        /* Reject an all-zero or multicast source station address. */
        return -LUME_EINVAL;
    /* Pack the first address bytes into a 32-bit register. Each left shift moves a byte into its
     * register position. Bitwise OR combines them.
     */
    low = (uint32_t)layout.mac[0] | (uint32_t)layout.mac[1] << 8 | (uint32_t)layout.mac[2] << 16 |
          /* Place the fourth address byte in bits 31:24 to complete the low address register. */
          (uint32_t)layout.mac[3] << 24;
    /* Pack the last two address bytes into the low 16 bits of the high address register. */
    high = (uint32_t)layout.mac[4] | (uint32_t)layout.mac[5] << 8;
    /* Program the first four station-address bytes. */
    write_mac_reg(GEM_MAC_ADDRESS_LOW, low);
    /* Program the last two bytes and activate this specific-address filter in the model. */
    write_mac_reg(GEM_MAC_ADDRESS_HIGH,
                  (high << GEM_MAC_ADDRESS_HIGH_SHIFT) & GEM_MAC_ADDRESS_HIGH_MASK);
    network_config = GEM_CONFIG_FULL_DUPLEX | GEM_CONFIG_GIGABIT | GEM_CONFIG_REMOVE_FCS;
    if (physical_rp1)
    {
        unsigned width = (read_mac_reg(GEM_DESIGN_CONFIG_1) >> 25) & 7;
        if (width != 1 && width != 2 && width != 4)
            return -LUME_ENOSYS;
        /* RP1 pclk is 200 MHz. Divide by 96 for Clause 22 MDIO. */
        network_config |=
            (5u << GEM_CONFIG_MDC_CLOCK_DIV_SHIFT) | ((width == 4   ? 2u
                                                       : width == 2 ? 1u
                                                                    : 0u)
                                                      << GEM_CONFIG_DATA_BUS_WIDTH_SHIFT);
        write_mac_reg(GEM_NETWORK_CONFIG, network_config);
        write_mac_reg(GEM_USER_IO, 1);
    }
    /* Enable PHY management while keeping RX and TX disabled during setup. */
    write_mac_reg(GEM_NETWORK_CONTROL, GEM_CONTROL_MDIO_ENABLE);
    /* Reserve two 16-bit values for the standard PHY identification words. */
    uint16_t id1, id2;
    /* Read PHY ID registers 2 and 3. Reject failed reads and values treated here as an absent or
     * unsupported PHY.
     */
    if (phy_read(2, &id1) || phy_read(3, &id2) || !id1 || id1 == UINT16_MAX || id2 == UINT16_MAX)
        /* Report a failed hardware or cache preparation step. The DDK invokes stop to roll back
         * failed initialization.
         */
        return -LUME_EIO;
    /* Combine ID1 as the high 16 bits and ID2 as the low 16 bits of one diagnostic value. */
    layout.phy_id = (uint32_t)id1 << 16 | id2;
    phy_link_known = false;
    if (physical_rp1 && layout.phy_id == BCM54213PE_PHY_ID)
    {
        int result = bcm54213_configure_leds();
        if (result)
            return result;
    }
    if (physical_rp1 && (phy_write(4, 0x01e1) || phy_write(9, 0x0200) || phy_write(0, 0x1200)))
        return -LUME_EIO;
    /* RP1 alternates entries so the next descriptor remains CPU-owned during transmission. */
    tx_index = 0;
    tx_count = physical_rp1 ? 2 : 1;
    tx_expected = tx_observed = 0;
    if (physical_rp1)
        (void)read_mac_reg(GEM_TX_FRAMES);
    for (unsigned i = 0; i < tx_count; ++i)
        tx.descriptors[i] = (struct gem_dma_descriptor){
            /* Store the low 32 bits of the private TX buffer DMA address. */
            .address_low = (uint32_t)layout.tx_frame_dma,
            /* Keep every entry CPU-owned and wrap only at the end of the ring. */
            .control = GEM_TX_USED | (i + 1 == tx_count ? GEM_TX_WRAP : 0),
            /* Store the upper 32 address bits of the RP1 DMA address. */
            .address_high = (uint32_t)(layout.tx_frame_dma >> 32),
        };
    /* Publish the CPU-owned TX descriptor and initialize the RX descriptor as device-owned. Stop
     * initialization if either cache preparation fails.
     */
    if (!sync_descriptor(&tx, false) || !receive_rearm())
        /* Report a failed hardware or cache preparation step. The DDK invokes stop to roll back
         * failed initialization.
         */
        return -LUME_EIO;
    /* Select full duplex, the modeled gigabit mode and received frames without the four-byte FCS
     * before enabling data transfer.
     */
    write_mac_reg(GEM_NETWORK_CONFIG, network_config);
    /* Encode the RX capacity in hardware units and select a four-beat DMA burst. */
    uint32_t rx_buffer_units = GEM_FRAME_BUFFER_BYTES / GEM_DMA_RX_BUFFER_UNIT_BYTES;
    uint32_t dma_config = GEM_DMA_ADDRESS_64 | GEM_DMA_BURST_INCR4 |
                          ((rx_buffer_units << GEM_DMA_RX_BUFFER_SHIFT) & GEM_DMA_RX_BUFFER_MASK);
    if (physical_rp1)
        dma_config |= GEM_DMA_TX_PACKET_BUFFER_SIZE | GEM_DMA_RX_PACKET_BUFFER_SIZE_MASK;
    write_mac_reg(GEM_DMA_CONFIG, dma_config);
    /* Program the upper TX ring address bits before writing the low queue-base word. */
    write_mac_reg(GEM_TX_QUEUE_BASE_HIGH, (uint32_t)(layout.tx_descriptor_dma >> 32));
    /* Program the upper RX ring address bits. */
    write_mac_reg(GEM_RX_QUEUE_BASE_HIGH, (uint32_t)(layout.rx_descriptor_dma >> 32));
    /* Point the transmit queue at the descriptor page, not at tx_frame. */
    write_mac_reg(GEM_TX_QUEUE_BASE, (uint32_t)layout.tx_descriptor_dma);
    /* Point the receive queue at the RX descriptor page. That descriptor contains the rx_frame
     * DMA address.
     */
    write_mac_reg(GEM_RX_QUEUE_BASE, (uint32_t)layout.rx_descriptor_dma);
    /* Clear old writable TX status bits before the first transmission. */
    write_mac_reg(GEM_TX_STATUS, UINT32_MAX);
    /* Clear old writable RX status bits before enabling reception. */
    write_mac_reg(GEM_RX_STATUS, UINT32_MAX);
    /* Construct the persistent control word by combining the independent MDIO, RX and TX enable
     * bits.
     */
    control = GEM_CONTROL_MDIO_ENABLE | GEM_CONTROL_RX_ENABLE | GEM_CONTROL_TX_ENABLE |
              /* Include the local-loopback bit only when that configuration was requested. */
              (loopback ? GEM_CONTROL_LOCAL_LOOPBACK : 0);
    /* No submitted frame is waiting for a completion result at initial startup. */
    tx_pending = false;
    /* Allow the public polling functions to access the initialized controller state. */
    running = true;
    /* Enable the data engines only after addresses, descriptors and cache visibility are
     * prepared.
     */
    write_mac_reg(GEM_NETWORK_CONTROL, control);
    /* Report a successfully initialized and running device to the DDK. */
    return 0;
}

/* Poll the single TX slot. Success means that the CPU may reuse it. EAGAIN means the controller
 * still owns it.
 */
int lume_example_gem_tx_complete(void)
{
    /* Require an active controller and fetch the descriptor state written by DMA before testing
     * its ownership flag.
     */
    if (!running || !sync_descriptor(&tx, true))
        /* Report that the device is unavailable or its descriptor cannot be synchronized. */
        return -LUME_EIO;
    if (physical_rp1)
        tx_observed += read_mac_reg(GEM_TX_FRAMES);
    /* A clear USED bit means DMA still owns the descriptor. The CPU must not overwrite its frame
     * buffer yet.
     */
    if (!(tx.descriptors[tx_index].control & GEM_TX_USED))
    {
        uint32_t status = physical_rp1 ? read_mac_reg(GEM_TX_STATUS) : 0;
        if (physical_rp1 && tx_pending && (status & GEM_TX_STATUS_USED_BIT_READ) &&
            !(status & (GEM_TX_STATUS_ACTIVE | GEM_TX_STATUS_ERRORS)))
        {
            /* One frame is outstanding. The counter proves egress before CPU reuse. */
            if (tx_observed == tx_expected)
            {
                tx.descriptors[tx_index].control |= GEM_TX_USED;
                if (!sync_descriptor(&tx, false))
                    return -LUME_EIO;
            }
            else
            {
                /* Resume an idle queue that stopped at a CPU-owned entry. */
                write_mac_reg(GEM_TX_STATUS, GEM_TX_STATUS_USED_BIT_READ);
                write_mac_reg(GEM_NETWORK_CONTROL, control | GEM_CONTROL_TX_START);
                return -LUME_EAGAIN;
            }
        }
        else
            /* Tell the caller to try again later without blocking the kernel. */
            return -LUME_EAGAIN;
    }
    /* A CPU-owned descriptor can also be the empty descriptor prepared at startup, with no
     * result to collect.
     */
    if (!tx_pending)
        /* The TX slot is free and there is no outstanding result. */
        return 0;
    /* Read controller completion and error flags for the submitted frame. */
    uint32_t status = read_mac_reg(GEM_TX_STATUS);
    if (physical_rp1 && (status & GEM_TX_STATUS_ERRORS))
        --tx_expected;
    /* Mark this completion as collected so a later poll does not report the same operation
     * again.
     */
    tx_pending = false;
    tx_index = (tx_index + 1) % tx_count;
    /* Acknowledge the status bits that were observed. Writable flags use write-one-to-clear
     * behavior.
     */
    write_mac_reg(GEM_TX_STATUS, status);
    /* Return an error when any selected TX error bit is set. Normal completion and reaching a
     * CPU-owned ring end are accepted.
     */
    return (status & GEM_TX_STATUS_ERRORS) ? -LUME_EIO : 0;
}

/* Submit an entire Ethernet frame. data points to caller bytes and bytes is their count. Copying
 * into private RAM lets the caller reuse its buffer after this function returns.
 */
int lume_example_gem_transmit(const void *data, size_t bytes)
{
    /* Do not submit work before attachment or after shutdown. */
    if (!running)
        /* Report unavailable hardware or failed DMA cache preparation. */
        return -LUME_EIO;
    /* Require a source pointer and a complete supported Ethernet frame between 14 and 1514
     * bytes, without FCS.
     */
    if (!data || bytes < GEM_FRAME_HEADER_BYTES || bytes > GEM_FRAME_MAX_BYTES)
        /* Reject the frame before reading any caller bytes. */
        return -LUME_EINVAL;
    /* Collect the previous completion or find out that the sole TX descriptor is still busy. */
    int result = lume_example_gem_tx_complete();
    /* Preserve busy/error results rather than overwriting a descriptor that is not available. */
    if (result)
        /* Pass the previous completion error or EAGAIN to the caller. */
        return result;
    /* View the untyped source pointer as individual read-only bytes. Indexing this pointer
     * advances one byte at a time.
     */
    const unsigned char *source = data;
    /* Increase a short frame to 60 bytes while leaving longer supported frames unchanged. */
    size_t padded = bytes < GEM_FRAME_MIN_BYTES ? GEM_FRAME_MIN_BYTES : bytes;
    /* Fill every byte that the controller will transmit, including padding. */
    for (size_t i = 0; i < padded; ++i)
        /* Copy caller bytes first and write zero for padding. DMA never receives a pointer into
         * temporary caller storage.
         */
        tx_frame[i] = i < bytes ? source[i] : 0;
    /* Clean CPU cache data to RAM before DMA reads the TX frame. Synchronizing the isolated
     * allocation cannot affect an RX-owned cache line.
     */
    if (!lume_dma_sync_for_device(tx_frame, sizeof(tx_frame), LUME_DMA_TO_DEVICE))
        /* Report unavailable hardware or failed DMA cache preparation. */
        return -LUME_EIO;
    /* Describe a complete frame of the padded length and clear USED to hand this descriptor to
     * DMA. The buffer address words remain unchanged.
     */
    tx.descriptors[tx_index].control =
        GEM_TX_LAST | (uint32_t)padded | (tx_index + 1 == tx_count ? GEM_TX_WRAP : 0);
    /* Publish the descriptor only after the frame bytes have been made visible to DMA. */
    if (!sync_descriptor(&tx, false))
        /* Report unavailable hardware or failed DMA cache preparation. */
        return -LUME_EIO;
    /* Remember that a completion must be collected before the next reuse of this TX slot. */
    tx_pending = true;
    if (physical_rp1)
        ++tx_expected;
    /* Add the one-shot transmit-start request to the persistent enable bits and notify the
     * controller to start processing the TX queue.
     */
    write_mac_reg(GEM_NETWORK_CONTROL, control | GEM_CONTROL_TX_START);
    /* The frame has been accepted and copied. Completion is checked separately because real
     * device work can finish later.
     */
    return 0;
}

/* Consume one received frame. data is a writable caller buffer and capacity is the maximum
 * number of bytes that may be copied there.
 */
int lume_example_gem_receive(void *data, size_t capacity)
{
    /* Reject reads while the controller is detached or stopped. */
    if (!running)
        /* Report inactive hardware or a failed descriptor/cache synchronization step. */
        return -LUME_EIO;
    /* Require writable caller storage large enough for at least an Ethernet header. */
    if (!data || capacity < GEM_FRAME_HEADER_BYTES)
        /* Reject the caller buffer without consuming a received frame. */
        return -LUME_EINVAL;
    /* Fetch the descriptor contents written by DMA before inspecting its RX ownership bit. */
    if (!sync_descriptor(&rx, true))
        /* Report inactive hardware or a failed descriptor/cache synchronization step. */
        return -LUME_EIO;
    /* A clear OWNED bit means the receive engine still owns this slot. No completed frame is
     * available for the CPU.
     */
    if (!(rx.descriptor.address_low & GEM_RX_OWNED))
        /* Report that no frame is ready. This is a normal nonblocking polling result. */
        return 0;
    /* Copy the completion word after confirming CPU ownership. It contains frame-boundary flags
     * and a byte count.
     */
    uint32_t status = rx.descriptor.control;
    /* Mask away flags and retain the low 13 length bits. */
    size_t bytes = status & GEM_DESCRIPTOR_LENGTH;
    /* Remember either the number of copied bytes or a negative error while the common rearm path
     * returns the descriptor to DMA.
     */
    int result;
    /* Require this descriptor to contain both the beginning and end of a frame. This
     * single-buffer implementation cannot assemble multi-descriptor frames.
     */
    if ((status & (GEM_RX_START | GEM_RX_END)) != (GEM_RX_START | GEM_RX_END) ||
        /* Reject malformed lengths and frame types larger than this example supports before
         * accessing the byte array.
         */
        bytes < GEM_FRAME_HEADER_BYTES || bytes > GEM_FRAME_MAX_BYTES)
        /* Record a receive-format or cache-access error. The completed slot will still be
         * consumed and rearmed.
         */
        result = -LUME_EIO;
    /* A valid frame must also fit the particular buffer supplied by this caller. */
    else if (bytes > capacity)
        /* Drop this frame and report that the caller buffer was too short. No partial frame is
         * copied.
         */
        result = -LUME_E2BIG;
    /* Invalidate stale cached RX bytes so the CPU sees the frame that the controller wrote into
     * RAM.
     */
    else if (!lume_dma_sync_for_cpu(rx_frame, sizeof(rx_frame), LUME_DMA_FROM_DEVICE))
        /* Record a receive-format or cache-access error. The completed slot will still be
         * consumed and rearmed.
         */
        result = -LUME_EIO;
    else
    {
        /* View the caller output storage as writable bytes. */
        unsigned char *destination = data;
        /* Copy exactly the validated frame length, leaving the rest of the caller buffer
         * untouched.
         */
        for (size_t i = 0; i < bytes; ++i)
            /* Move one received byte from driver-owned DMA storage into caller-owned memory. */
            destination[i] = rx_frame[i];
        /* Record the complete received frame length as a positive result. */
        result = (int)bytes;
    }
    /* Return the consumed slot and its frame buffer to DMA even when the frame was dropped for a
     * format/capacity error.
     */
    if (!receive_rearm())
        /* Report inactive hardware or a failed descriptor/cache synchronization step. */
        return -LUME_EIO;
    /* Clear writable receive-status flags after consuming this completion. */
    write_mac_reg(GEM_RX_STATUS, UINT32_MAX);
    /* Refreshing enabled control reloads the model's cached RX descriptor
     * and lets frames waiting in the backend queue progress. */
    /* Keep RX/TX enabled and prompt the modeled controller to reload the rearmed RX descriptor
     * and process waiting frames.
     */
    /* RP1 RX rearming preserves any outstanding TX_START request. */
    if (!physical_rp1)
        write_mac_reg(GEM_NETWORK_CONTROL, control);
    /* Return the frame length or the saved receive error after ownership has been handed back. */
    return result;
}

/* Copy the active memory and identity information into a structure belonging to the caller. */
bool lume_example_gem_layout(struct lume_example_gem_layout *out)
{
    /* Require an initialized controller and a destination structure supplied by the caller. */
    if (!running || !out)
        /* No valid layout can be returned in the current state. */
        return false;
    /* Copy the entire structure through the output pointer. This copies addresses and metadata,
     * not descriptor pages or frame contents.
     */
    *out = layout;
    /* Report that the caller structure now contains the current layout. */
    return true;
}

/* Query the modeled link status through ETH_CFG without performing an ARP, DHCP or IP operation. */
bool lume_example_gem_link(void)
{
    /* Logical AND short-circuits when stopped. When running, read wrapper status and test only
     * its link-up bit.
     */
    return running &&
           (physical_rp1 ? rp1_link()
                         : (read_cfg_reg(GEM_CFG_STATUS) & GEM_CFG_STATUS_RGMII_LINK_STATUS));
}

/* Disable controller activity without freeing the static RAM allocations. The DDK calls this
 * during normal detach and initialization rollback.
 */
static int stop(struct lume_device *d)
{
    /* Explicitly acknowledge that this callback does not use its device parameter. Casting to
     * void avoids an unused-parameter warning.
     */
    (void)d;
    /* A failed probe clears the saved address. In that case rollback must avoid touching
     * controller registers.
     */
    if (!mac_regs)
        /* Report that the controller needs no further stopping or that it has become inactive. */
        return 0;
    /* Request TX halt while clearing RX and TX enable bits in this control write. */
    write_mac_reg(GEM_NETWORK_CONTROL, GEM_CONTROL_TX_HALT);
    /* Keep all MAC interrupt sources disabled during shutdown. */
    write_mac_reg(GEM_INTERRUPT_DISABLE, UINT32_MAX);
    /* Also mask the Ethernet source at the wrapper interrupt gate. */
    clear_cfg_bits(GEM_CFG_INTERRUPT_ENABLE, GEM_CFG_INTERRUPT_ETHERNET);
    /* Prevent subsequent public operations from submitting or consuming frames. */
    running = false;
    /* Poll the active-TX indication for a bounded number of reads. */
    for (unsigned i = 0; i < GEM_POLL_LIMIT; ++i)
        /* Continue shutdown only once the controller reports no active transmission. */
        if (!(read_mac_reg(GEM_TX_STATUS) & GEM_TX_STATUS_ACTIVE))
        {
            /* Leave every data/management enable bit clear. */
            write_mac_reg(GEM_NETWORK_CONTROL, 0);
            /* Discard the software record of any prior transmission after stopping the engine. */
            tx_pending = false;
            /* Report that the controller needs no further stopping or that it has become
             * inactive.
             */
            return 0;
        }
    /* Report a controller that failed to stop within the bound. The DDK retains its claims when
     * stop fails.
     */
    return -LUME_EIO;
}

/* Provide an explicit attachment entry point for a caller that manages device startup manually. */
int lume_example_gem_eth_start(void)
{
    /* Ask the DDK to validate and claim the resources, run probe and run start. The address
     * passed here is the software descriptor address.
     */
    return lume_device_attach(&gem_device);
}

/* Provide the matching manual detachment entry point. */
int lume_example_gem_eth_stop(void)
{
    /* Ask the DDK to call stop and release its claims if stopping succeeds. */
    return gem_device.state == LUME_DEVICE_OFF ? 0 : lume_device_detach(&gem_device);
}

/* Place a pointer to gem_device in the linker-retained driver table for automatic startup.
 * gem_driver_entry is the name of the pointer created by the macro, not an MMIO base address.
 */
LUME_DRIVER_ENTRY(gem_driver_entry, gem_device);
