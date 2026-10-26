/* This diagnostic constructs Ethernet bytes directly, without lwIP. Local loopback checks
 * descriptor and buffer handling. A separate broadcast ARP request asks which station owns
 * 10.0.3.2 and verifies the backend reply. The request source address 10.0.3.15 is test data,
 * not a DHCP lease obtained by this example.
 * An Ethernet frame starts with 6 destination bytes, 6 source bytes and 2 EtherType bytes. ARP
 * then adds a 28-byte message with hardware/protocol types, lengths, an operation, and
 * sender/target addresses. A 42-byte Ethernet-plus-ARP request needs zero padding to reach the
 * 60-byte frame size used without FCS.
 */

/* Link after the GEM example to exercise real MMIO and DMA on qemu,rp1-gem.
 * The Ethernet backend supplies 10.0.3.2 on the 10.0.3.0/24 subnet. */
/* Import the frame layout and polling API of the GEM example being exercised. */
#include "../../../examples/ddk/gem.h"
/* Import the symbolic negative error codes used by the test and driver. */
#include "lume/abi.h"
/* Import the DDK structures used to run this test as a diagnostic driver. */
#include "lume/device.h"
/* Import the kernel clock and serial diagnostic output functions. */
#include "lume/early.h"

/* Allocate a frame to submit and a separate copy of the bytes expected back. Static storage
 * lasts throughout the boot and avoids putting these arrays on a small callback stack.
 */
static unsigned char frame[GEM_FRAME_MAX_BYTES], expected[GEM_FRAME_MAX_BYTES];
/* Allocate caller-owned storage for received bytes. The GEM driver copies from its own RX DMA
 * buffer into this array.
 */
static unsigned char received[GEM_FRAME_BUFFER_BYTES];

/* Compare two memory regions byte by byte. The pointers can refer to any object type because
 * they are passed as void pointers.
 */
static bool same(const void *a, const void *b, size_t bytes)
{
    /* Interpret the two regions as read-only byte arrays so indexing advances by exactly one
     * byte.
     */
    const unsigned char *x = a, *y = b;
    /* Inspect every byte in the requested comparison range. */
    for (size_t i = 0; i < bytes; ++i)
        /* A single differing byte is enough to fail the comparison. */
        if (x[i] != y[i])
            /* Report that the compared byte sequences differ. */
            return false;
    /* All compared bytes matched. */
    return true;
}

/* Poll for transmit completion with a three-second limit. This waiting behavior belongs to the
 * diagnostic test, not the nonblocking driver API.
 */
static int wait_tx(void)
{
    /* Save a starting timestamp in milliseconds. Unsigned subtraction below handles wraparound
     * of this 32-bit counter.
     */
    uint32_t begin = lume_clock_millis();
    /* Keep the result of the most recent polling call. */
    int result;
    /* Execute the polling body at least once before deciding whether the deadline has expired. */
    do
    {
        /* Ask whether the TX descriptor has returned to CPU ownership and collect its status. */
        result = lume_example_gem_tx_complete();
        /* Only EAGAIN requests another poll. Success and permanent errors both finish this wait. */
        if (result != -LUME_EAGAIN)
            /* Return the completion result without changing its meaning. */
            return result;
        /* Keep polling while elapsed unsigned milliseconds remain below 3000. */
    } while ((uint32_t)(lume_clock_millis() - begin) < 3000);
    /* Report a failed diagnostic condition or a polling deadline that expired without the
     * expected result.
     */
    return -LUME_EIO;
}

/* Poll for one received frame with the same diagnostic deadline. */
static int wait_rx(void)
{
    /* Save a starting timestamp in milliseconds. Unsigned subtraction below handles wraparound
     * of this 32-bit counter.
     */
    uint32_t begin = lume_clock_millis();
    /* Keep the result of the most recent polling call. */
    int result;
    /* Execute the polling body at least once before deciding whether the deadline has expired. */
    do
    {
        /* Poll RX and offer the entire received array as the caller output buffer. */
        result = lume_example_gem_receive(received, sizeof(received));
        /* Zero means no frame yet. A positive byte count or negative error ends the wait. */
        if (result)
            /* Return the received frame length or the driver error. */
            return result;
        /* Keep polling while elapsed unsigned milliseconds remain below 3000. */
    } while ((uint32_t)(lume_clock_millis() - begin) < 3000);
    /* Report a failed diagnostic condition or a polling deadline that expired without the
     * expected result.
     */
    return -LUME_EIO;
}

/* Run the frame-path checks after the GEM driver has attached. This callback is registered as a
 * separate diagnostic driver.
 */
static int exercise(struct lume_device *d)
{
    /* This callback uses the GEM public API rather than fields of its own DDK descriptor. */
    (void)d;
    /* Reserve an ordinary C structure for the GEM memory-address and identity snapshot. */
    struct lume_example_gem_layout info;
    /* Require an already started GEM device, then detach it so its configuration may be changed.
     * Logical OR stops evaluation as soon as a step fails.
     */
    if (!lume_example_gem_layout(&info) || lume_example_gem_eth_stop() ||
        /* Keep the current station address, enable local loopback and attach again. A zero
         * pointer selects the existing address.
         */
        lume_example_gem_configure(0, true) || lume_example_gem_eth_start() ||
        /* Obtain the addresses and mode selected by the new attachment. */
        !lume_example_gem_layout(&info))
        /* Report a failed diagnostic condition or a polling deadline that expired without the
         * expected result.
         */
        return -LUME_EIO;
    /* Print where the CPU accesses the TX descriptor record in RAM. */
    lume_value("GEM_TX_DESC_CPU", info.tx_descriptor_cpu);
    /* Print the translated address programmed into the controller TX queue registers. */
    lume_value("GEM_TX_DESC_DMA", info.tx_descriptor_dma);
    /* Print where the CPU accesses the RX descriptor record. */
    lume_value("GEM_RX_DESC_CPU", info.rx_descriptor_cpu);
    /* Print the address the DMA engine uses for the RX descriptor. */
    lume_value("GEM_RX_DESC_DMA", info.rx_descriptor_dma);
    /* Print the CPU address containing bytes that will actually be transmitted. */
    lume_value("GEM_TX_FRAME_CPU", info.tx_frame_cpu);
    /* Print the frame-buffer address stored in the TX descriptor. */
    lume_value("GEM_TX_FRAME_DMA", info.tx_frame_dma);
    /* Print where the CPU can read received frame bytes after ownership and cache
     * synchronization.
     */
    lume_value("GEM_RX_FRAME_CPU", info.rx_frame_cpu);
    /* Print the frame-buffer address stored in the RX descriptor. */
    lume_value("GEM_RX_FRAME_DMA", info.rx_frame_dma);
    /* Print the combined PHY identification words read over MDIO. */
    lume_value("GEM_PHY_ID", info.phy_id);
    /* Require local loopback and the expected virtual PHY identity for this binding. */
    if (!info.loopback || info.phy_id != UINT32_C(0x01410cc2) ||
        /* Check that the TX frame DMA address is the CPU physical address plus the configured
         * RP1 inbound-window base.
         */
        info.tx_frame_dma != UINT64_C(0x1000000000) + info.tx_frame_cpu ||
        /* Check the same translation for the RX descriptor, independently of its payload
         * address.
         */
        info.rx_descriptor_dma != UINT64_C(0x1000000000) + info.rx_descriptor_cpu ||
        /* Before submitting a frame, verify that polling RX reports an empty queue rather than
         * stale completion data.
         */
        lume_example_gem_receive(received, sizeof(received)) != 0 ||
        /* Verify that an oversized transmit request is rejected before the driver reads past the
         * caller buffer.
         */
        lume_example_gem_transmit(frame, GEM_FRAME_MAX_BYTES + 1) != -LUME_EINVAL)
        /* Report a failed diagnostic condition or a polling deadline that expired without the
         * expected result.
         */
        return -LUME_EIO;
    /* Exercise the header-only input that needs padding, a minimum frame, intermediate data and
     * maximum supported frames. Repeated lengths also test descriptor reuse.
     */
    static const unsigned lengths[] = {14, 60, 1514, 256, 1514, 60};
    /* Run once for each array element. Divide the complete array size by the element size to
     * count test cases.
     */
    for (unsigned k = 0; k < sizeof(lengths) / sizeof(lengths[0]); ++k)
    {
        /* Select the number of caller bytes for this transmission. */
        unsigned bytes = lengths[k];
        /* Calculate the length expected after the driver pads a short frame to 60 bytes. */
        unsigned padded = bytes < GEM_FRAME_MIN_BYTES ? GEM_FRAME_MIN_BYTES : bytes;
        /* Fill the whole source array with a deterministic pattern to make accidental corruption
         * visible.
         */
        for (unsigned i = 0; i < sizeof(frame); ++i)
            /* Produce a repeatable changing byte pattern. The cast keeps the low eight bits of
             * the arithmetic result.
             */
            frame[i] = (unsigned char)(i * 17 + k);
        /* Visit the six bytes that form an Ethernet station address. */
        for (unsigned i = 0; i < 6; ++i)
        {
            /* Use the local station address as the destination so the controller accepts its
             * looped-back frame.
             */
            frame[i] = info.mac[i];
            /* Place the same local address in Ethernet source bytes 6 through 11. */
            frame[i + 6] = info.mac[i];
        }
        /* Write the high byte of experimental EtherType 0x88b5 for these raw loopback frames. */
        frame[12] = 0x88;
        /* Write the low EtherType byte. Protocol fields use network byte order, with the high
         * byte first.
         */
        frame[13] = 0xb5;
        /* Build an independent expected frame covering both caller data and required padding. */
        for (unsigned i = 0; i < padded; ++i)
            /* Expect original bytes inside the input length and zero bytes in the padding area. */
            expected[i] = i < bytes ? frame[i] : 0;
        /* Submit the complete caller frame. A nonzero result means it was not accepted. */
        if (lume_example_gem_transmit(frame, bytes))
            /* Report a failed diagnostic condition or a polling deadline that expired without
             * the expected result.
             */
            return -LUME_EIO;
        /* Immediately change caller storage after submission. XOR with 0xff flips all eight
         * bits. The transmitted copy must remain unchanged.
         */
        frame[0] ^= 0xff;
        /* Require TX completion, exactly the padded RX byte count and byte-for-byte equality
         * with the saved expected frame.
         */
        if (wait_tx() || wait_rx() != (int)padded || !same(expected, received, padded))
            /* Report a failed diagnostic condition or a polling deadline that expired without
             * the expected result.
             */
            return -LUME_EIO;
        /* Print the size of the successfully verified frame. */
        lume_value("GEM_LOOPBACK_BYTES", padded);
    }
    /* Record that all loopback sizes and repeated descriptor transfers passed. */
    lume_puts("LUME CHECK GEM DRIVER LOOPBACK PASS\n");
    /* Detach again and disable local loopback so the next frame reaches the external network
     * backend.
     */
    if (lume_example_gem_eth_stop() || lume_example_gem_configure(0, false) ||
        /* Attach in ordinary Ethernet mode, refresh the address snapshot and require the modeled
         * link to be up.
         */
        lume_example_gem_eth_start() || !lume_example_gem_layout(&info) || !lume_example_gem_link())
        /* Report a failed diagnostic condition or a polling deadline that expired without the
         * expected result.
         */
        return -LUME_EIO;
    /* Prepare a complete minimum-sized Ethernet storage region for the ARP request. */
    for (unsigned i = 0; i < 60; ++i)
        /* Zero the request and its padding first. Fields not explicitly assigned below remain
         * zero.
         */
        frame[i] = 0;
    /* Visit the six bytes that form an Ethernet station address. */
    for (unsigned i = 0; i < 6; ++i)
    {
        /* Ethernet destination bytes 0 through 5 become ff:ff:ff:ff:ff:ff. A broadcast ARP
         * request reaches any station that might own the target IP address.
         */
        frame[i] = 0xff;
        /* Put the local MAC address in two places: the Ethernet source at bytes 6..11 and the
         * ARP sender hardware address at bytes 22..27.
         */
        frame[i + 6] = frame[i + 22] = info.mac[i];
    }
    /* Write 0x08 as the high byte of Ethernet EtherType 0x0806, selecting the ARP protocol. */
    frame[12] = 8;
    /* Write 0x06 as the low EtherType byte. The ARP message starts immediately after this
     * Ethernet header.
     */
    frame[13] = 6;
    /* ARP bytes 14..15 encode hardware type 0x0001 for Ethernet. Byte 14 remains zero from
     * initialization.
     */
    frame[15] = 1;
    /* ARP bytes 16..17 encode protocol type 0x0800 for IPv4. Byte 17 remains zero. */
    frame[16] = 8;
    /* The ARP hardware-address length is six bytes, matching the Ethernet MAC address length. */
    frame[18] = 6;
    /* The ARP protocol-address length is four bytes, matching an IPv4 address. */
    frame[19] = 4;
    /* ARP bytes 20..21 encode operation 0x0001, an address-resolution request. */
    frame[21] = 1;
    /* Write the first octet of both IPv4 addresses. Sender IP begins at byte 28 and target IP
     * begins at byte 38.
     */
    frame[28] = frame[38] = 10;
    /* Write the third octet of both addresses. Their second octets remain zero, selecting subnet
     * 10.0.3.0/24.
     */
    frame[30] = frame[40] = 3;
    /* Finish the sender IPv4 address as 10.0.3.15. */
    frame[31] = 15;
    /* Finish the requested target IPv4 address as 10.0.3.2. ARP target hardware bytes 32..37
     * remain zero because its MAC address is not known yet.
     */
    frame[41] = 2;
    /* Submit 14 Ethernet header bytes plus 28 ARP bytes. The driver pads these 42 bytes to 60
     * for transmission.
     */
    if (lume_example_gem_transmit(frame, 42) || wait_tx())
        /* Report a failed diagnostic condition or a polling deadline that expired without the
         * expected result.
         */
        return -LUME_EIO;
    /* Wait for the backend to answer. This response travels through the actual RX DMA path. */
    int bytes = wait_rx();
    /* Require a complete ARP-sized reply addressed to the local station and tagged with
     * EtherType 0x0806.
     */
    if (bytes < 42 || !same(received, info.mac, 6) || received[12] != 8 || received[13] != 6 ||
        /* Require operation 0x0002 for an ARP reply and confirm that its sender IPv4 address is
         * the target that was requested.
         */
        received[20] != 0 || received[21] != 2 || !same(received + 28, frame + 38, 4) ||
        /* Require the reply target hardware address to match the local MAC and its target IPv4
         * address to match the request sender IP.
         */
        !same(received + 32, info.mac, 6) || !same(received + 38, frame + 28, 4))
        /* Report a failed diagnostic condition or a polling deadline that expired without the
         * expected result.
         */
        return -LUME_EIO;
    /* Record that a remote backend ARP reply was received and its Ethernet/ARP fields were
     * validated.
     */
    lume_puts("LUME CHECK GEM DRIVER BACKEND ARP PASS\n");
    /* Tell the DDK that the diagnostic driver started successfully after completing its checks. */
    return 0;
}

/* Install the test function as this diagnostic device start callback. It is a separate
 * descriptor from the GEM hardware device.
 */
static const struct lume_driver_ops ops = {.start = exercise};
/* Describe the diagnostic device that will run after the real GEM example is attached. */
static struct lume_device diagnostic = {
    /* Set its DDK version, class, registry name and callback table. It has no extra MMIO
     * resources because it calls the existing GEM driver.
     */
    .version = LUME_DDK_VERSION,
    .class_id = LUME_DEVICE_NETWORK,
    .name = "gem-test",
    .ops = &ops};
/* Register the diagnostic descriptor in the retained driver table. Linking this object after
 * gem.o makes its checks run after GEM initialization.
 */
LUME_DRIVER_ENTRY(gem_diagnostic_entry, diagnostic);
