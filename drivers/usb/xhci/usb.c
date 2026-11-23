#include "lume/cpu.h"
#if !defined(LUME_ARCH_AMD64)
#include "lume/board.h"
#endif
/* Polling xHCI transport for USB 2 hubs and HID boot keyboards and mice. Platform bindings provide
 * the controller address and DMA aperture. Transfer and command completion use event rings. */
#include "lume/early.h"
#include "lume/mmio.h"
#include "lume/counter.h"
#include "lume/usb.h"
#include "lume/xhci_ring.h"
#include "lume/abi.h"
#include "lume/dma.h"
#include "lume/worker.h"
#include "lume/work.h"
#if defined(LUME_LINUX_USB_HID)
#include "lume/linux_usb_hid.h"
#endif
#if defined(LUME_LINUX_USB_STORAGE) || defined(LUME_LINUX_USB_AUDIO)
#include <linux/usb.h>
#endif
#if defined(LUME_LINUX_USB_STORAGE)
struct lume_linux_usb_storage;
struct lume_linux_usb_storage *lume_linux_usb_storage_attach(struct usb_device *, unsigned,
                                                             unsigned);
void lume_linux_usb_storage_detach(struct lume_linux_usb_storage *);
#endif
#if defined(LUME_LINUX_USB_AUDIO)
struct lume_linux_usb_audio;
struct lume_linux_usb_audio *lume_linux_usb_audio_attach(struct usb_device *, unsigned, unsigned,
                                                         unsigned);
void lume_linux_usb_audio_detach(struct lume_linux_usb_audio *);
#endif
#if defined(LUME_ARCH_AMD64)
#include "lume/usb_inventory.h"
#include "lume/hardware_abi.h"

static struct
{
    uint64_t base, size;
    uint32_t parent;
    bool (*present)(unsigned);
} pci_hosts[LUME_XHCI_HOSTS];
#endif
#define RP1 LUME_BOARD_RP1_BASE
#define MAX_SLOTS 8u
#define MAX_PAGES 192u

enum host_operation
{
    HOST_IDLE,
    HOST_START,
    HOST_SCAN,
    HOST_CONTROL,
    HOST_STOP,
};

enum completion
{
    COMMAND_READY = 1,
    TRANSFER_READY = 2,
};

#if defined(LUME_LINUX_USB_HID)
struct control_context
{
    struct host *host;
    struct device *device;
};
#endif

struct input
{
    unsigned endpoint, packet;
    unsigned char *report;
    struct lume_xhci_ring ring;
    uint64_t trb;
    bool keyboard, mouse, pending;
    struct lume_hid hid;
#if defined(LUME_LINUX_USB_HID)
    struct control_context control;
    struct lume_linux_usb_hid *linux_hid;
#endif
};

#if defined(LUME_LINUX_USB_STORAGE) || defined(LUME_LINUX_USB_AUDIO)
struct linux_endpoint
{
    unsigned address, id, packet, type, active_bytes;
    struct lume_xhci_ring ring;
    uint64_t trb;
    struct urb *active;
};
#endif

struct device
{
    unsigned slot, speed, root, depth, route, tt_slot, tt_port, mtt;
    uint32_t inventory;
    unsigned hub_ports;
    unsigned ep0_packet, inputs;
    uint32_t *input, *output;
    unsigned char *buffer;
    struct lume_xhci_ring ep0;
    bool hub, disconnected, control_blocked;
#if defined(LUME_LINUX_USB_HID)
    uint16_t vendor, product;
#endif
#if defined(LUME_LINUX_USB_STORAGE) || defined(LUME_LINUX_USB_AUDIO)
    struct control_context linux_control;
    struct usb_device linux_device;
    struct linux_endpoint linux_endpoints[LUME_USB_ENDPOINTS];
    unsigned linux_endpoint_count;
#endif
#if defined(LUME_LINUX_USB_STORAGE)
    struct lume_linux_usb_storage *storage;
#endif
#if defined(LUME_LINUX_USB_AUDIO)
    struct lume_linux_usb_audio *audio;
#endif
    struct input channels[LUME_USB_INPUTS];
};

struct host
{
    uint64_t base, op, rt, doorbells, frequency;
    unsigned index, ports, context_bytes, event_head, event_cycle, allocated;
    uint64_t pages[MAX_PAGES], *dcbaa;
    struct lume_xhci_trb *events;
    struct lume_xhci_ring command;
    struct device devices[MAX_SLOTS + 1];
    uint64_t waiting_command, waiting_transfer, waiting_data;
    unsigned command_code, command_slot, transfer_code, transfer_slot;
    unsigned transfer_bytes, transfer_actual;
    bool transfer_invalid;
    volatile uint32_t completion;
    bool running, valid;
    unsigned errors, completions;
    uint32_t root_ready, next_scan;
    uint64_t next_poll;
    unsigned retired_reports, retired_characters, retired_dropped;
    bool scanning, event_polling, poll_scheduled;
    /* The host operation owns command, EP0 and DMA storage across worker waits. */
    enum host_operation operation;
    struct lume_work topology;
};
static struct host hosts[LUME_XHCI_HOSTS];
static void topology_work(void *);
static unsigned keyboard_count, mouse_count, hub_count;
#ifdef LUME_PLATFORM_LKM
static unsigned configured_hosts;
#else
static unsigned configured_hosts = 3;
#endif

bool lume_usb_configure_host(unsigned index, bool enabled)
{
    if (index >= LUME_XHCI_HOSTS || hosts[index].valid || hosts[index].operation != HOST_IDLE)
        return false;
    if (enabled)
        configured_hosts |= 1u << index;
    else
        configured_hosts &= ~(1u << index);
    return true;
}
#if defined(LUME_ARCH_AMD64)
bool lume_usb_configure_pci(unsigned index, uint64_t base, uint64_t size, uint32_t parent,
                            bool (*present)(unsigned))
{
    if (index >= LUME_XHCI_HOSTS || hosts[index].valid || hosts[index].operation != HOST_IDLE ||
        !parent || !present || !base || (base & 4095) || size < 4096 || size > UINT64_MAX - base)
        return false;
    pci_hosts[index].base = base;
    pci_hosts[index].size = size;
    pci_hosts[index].parent = parent;
    pci_hosts[index].present = present;
    return true;
}
#endif

static void write64(uint64_t a, uint64_t v)
{
    lume_mmio_write32(a, (uint32_t)v);
    lume_mmio_write32(a + 4, (uint32_t)(v >> 32));
}

static uint32_t milliseconds(struct host *h)
{
    return (uint32_t)(lume_counter_read() / (h->frequency / 1000));
}

static void delay(struct host *h, unsigned us)
{
    uint64_t start = lume_counter_read();
    uint64_t ticks = h->frequency * us / 1000000;
    if (lume_worker_pause(start + ticks))
        return;
    while (lume_counter_read() - start < ticks)
    {
        lume_uart_poll_input();
        lume_cpu_relax();
    }
}

static void wait_progress(struct host *h, uint32_t mask, uint64_t deadline)
{
    uint64_t now = lume_counter_read();
    if ((int64_t)(now - deadline) >= 0)
        return;
    uint64_t next = now + h->frequency / 1000 + (h->frequency % 1000 != 0);
    if ((int64_t)(next - deadline) > 0)
        next = deadline;
    if (lume_worker_wait_bits(&h->completion, mask, next))
        return;
    uint64_t until = now + h->frequency / 20000 + (h->frequency % 20000 != 0);
    if ((int64_t)(until - deadline) > 0)
        until = deadline;
    while ((int64_t)(lume_counter_read() - until) < 0)
    {
        lume_uart_poll_input();
        lume_cpu_relax();
    }
}

static bool wait_register(struct host *h, uint64_t a, uint32_t mask, uint32_t value, unsigned ms)
{
    uint64_t deadline = lume_counter_read() + h->frequency * ms / 1000;
    for (;;)
    {
        if ((lume_mmio_read32(a) & mask) == value)
            return true;
        if ((int64_t)(lume_counter_read() - deadline) >= 0)
            break;
        wait_progress(h, 0, deadline);
    }
    return false;
}

static void zero(void *p, size_t bytes)
{
    unsigned char *d = p;
    while (bytes--)
        *d++ = 0;
}

static void copy(void *out, const void *in, size_t bytes)
{
    unsigned char *d = out;
    const unsigned char *s = in;
    while (bytes--)
        *d++ = *s++;
}

static uint64_t dma(const void *p)
{
    uint64_t address;
#if defined(LUME_ARCH_AMD64)
    const struct lume_dma_domain identity = {0, 0, UINT64_C(0x200000000), UINT64_MAX};
    if (!lume_dma_map(&identity, (uintptr_t)p, 1, &address))
#else
    if (!lume_dma_map(&lume_rp1_dma_domain, (uintptr_t)p, 1, &address))
#endif
        lume_fail("USB DMA address");
    return address;
}

static void cache(void *p, size_t bytes, bool invalidate)
{
    bool valid = invalidate ? lume_dma_sync_for_cpu(p, bytes, LUME_DMA_FROM_DEVICE)
                            : lume_dma_sync_for_device(p, bytes, LUME_DMA_TO_DEVICE);
    if (!valid)
        lume_fail("USB DMA buffer");
}

static void *page(struct host *h)
{
    unsigned index = 0;
    while (index < h->allocated && h->pages[index])
        ++index;
    if (index == MAX_PAGES)
        return 0;
    uint64_t p = lume_page_alloc(0);
    if (!p)
        return 0;
    h->pages[index] = p;
    if (index == h->allocated)
        ++h->allocated;
    void *v = (void *)(uintptr_t)p;
    zero(v, 4096);
    cache(v, 4096, false);
    return v;
}

static void release_page(struct host *h, const void *p)
{
    if (!p)
        return;
    for (unsigned n = 0; n < h->allocated; ++n)
        if (h->pages[n] == (uintptr_t)p)
        {
            if (!lume_page_free(h->pages[n]))
                lume_fail("USB page release");
            h->pages[n] = 0;
            return;
        }
    lume_fail("USB page ownership");
}

static bool ring_init(struct host *h, struct lume_xhci_ring *r)
{
    r->trbs = page(h);
    if (!r->trbs)
        return false;
    r->dma = dma(r->trbs);
    r->head = 0;
    r->cycle = 1;
    return true;
}

static uint64_t enqueue(struct lume_xhci_ring *r, uint64_t p, uint32_t status, uint32_t control)
{
    unsigned i = r->head;
    uint64_t address = lume_xhci_enqueue(r, p, status, control);
    cache(&r->trbs[i], 16, false);
    if (!r->head)
        cache(&r->trbs[255], 16, false);
    return address;
}

static void arm_input(struct host *h, struct device *d, struct input *input)
{
    if (input->pending || d->disconnected)
        return;
#if defined(LUME_LINUX_USB_HID)
    if (!lume_linux_usb_hid_pending(input->linux_hid))
        return;
#endif
    if (!lume_dma_sync_for_device(input->report, 64, LUME_DMA_FROM_DEVICE))
        lume_fail("USB HID DMA buffer");
    input->trb = enqueue(&input->ring, dma(input->report), input->packet, (1u << 10) | 32 | 4);
    input->pending = true;
    lume_mmio_write32(h->doorbells + d->slot * 4, input->endpoint);
}

#if defined(LUME_LINUX_USB_STORAGE) || defined(LUME_LINUX_USB_AUDIO)
static struct linux_endpoint *linux_endpoint_for(struct device *device, unsigned pipe)
{
    unsigned address = usb_pipeendpoint(pipe) | (usb_pipein(pipe) ? USB_DIR_IN : 0);
    for (unsigned index = 0; index < device->linux_endpoint_count; ++index)
        if (device->linux_endpoints[index].address == address)
            return &device->linux_endpoints[index];
    return NULL;
}

static int arm_linux_endpoint(struct host *host, struct device *device,
                              struct linux_endpoint *endpoint)
{
    if (endpoint->active || device->disconnected)
        return 0;
    struct urb *urb = device->linux_device.pending;
    while (urb && linux_endpoint_for(device, urb->pipe) != endpoint)
        urb = urb->pending_next;
    if (!urb)
        return 0;
    bool input = endpoint->address & USB_DIR_IN;
    enum lume_dma_direction direction = input ? LUME_DMA_FROM_DEVICE : LUME_DMA_TO_DEVICE;
    if (!lume_dma_sync_for_device(urb->transfer_buffer, (size_t)urb->transfer_buffer_length,
                                  direction))
        return -LUME_EIO;
    void *buffer = urb->transfer_buffer;
    unsigned bytes = (unsigned)urb->transfer_buffer_length;
    uint32_t control = (1u << 10) | 32u | (input ? 4u : 0);
    if (endpoint->type == USB_ENDPOINT_XFER_ISOC)
    {
        buffer = (unsigned char *)buffer + urb->iso_frame_desc[0].offset;
        bytes = urb->iso_frame_desc[0].length;
        control = (5u << 10) | 32u | (1u << 31) | (input ? 4u : 0);
    }
    endpoint->active_bytes = bytes;
    endpoint->trb = enqueue(&endpoint->ring, dma(buffer), bytes, control);
    endpoint->active = urb;
    lume_mmio_write32(host->doorbells + device->slot * 4, endpoint->id);
    return 0;
}

static int linux_submit(void *context, struct urb *urb)
{
    struct control_context *wire = context;
    struct host *host = wire->host;
    struct device *device = wire->device;
    if (!urb || urb->dev != &device->linux_device || urb->transfer_buffer_length > 65536)
        return -LUME_EINVAL;
    struct linux_endpoint *endpoint = linux_endpoint_for(device, urb->pipe);
    if (!endpoint)
        return -LUME_ENXIO;
    unsigned pipe_type = usb_pipetype(urb->pipe);
    if ((endpoint->type == USB_ENDPOINT_XFER_BULK && pipe_type != PIPE_BULK) ||
        (endpoint->type == USB_ENDPOINT_XFER_ISOC &&
         (pipe_type != PIPE_ISOCHRONOUS || urb->number_of_packets != 1)) ||
        (endpoint->type != USB_ENDPOINT_XFER_BULK &&
         endpoint->type != USB_ENDPOINT_XFER_ISOC))
        return -LUME_EINVAL;
    return arm_linux_endpoint(host, device, endpoint);
}
#endif

static bool events(struct host *h)
{
    if (h->event_polling || !h->events)
        return false;
    h->event_polling = true;
    bool advanced = false;
    unsigned used = 0;
    while (used++ < 256)
    {
        struct lume_xhci_trb *entry = &h->events[h->event_head];
        cache(entry, 16, true);
        struct lume_xhci_trb e = *entry;
        if ((e.control & 1) != h->event_cycle)
            break;
        advanced = true;
        lume_dma_read_barrier();
        unsigned type = (e.control >> 10) & 63, cc = e.status >> 24, slot = e.control >> 24,
                 ep = (e.control >> 16) & 31;
        ++h->completions;
        if (type == 33 && h->waiting_command && e.parameter == h->waiting_command)
        {
            h->command_code = cc;
            h->command_slot = slot;
            h->completion |= COMMAND_READY;
        }
        else if (type == 32 && slot && slot <= MAX_SLOTS)
        {
            struct device *d = &h->devices[slot];
            if (ep == 1 && h->waiting_transfer && slot == h->transfer_slot)
            {
                if (h->waiting_data && e.parameter == h->waiting_data && cc == 13)
                {
                    /* A short data stage still needs an acknowledged status stage. */
                    unsigned remaining = e.status & 0xffffffu;
                    if (remaining > h->transfer_bytes)
                        h->transfer_invalid = true;
                    else
                        h->transfer_actual = h->transfer_bytes - remaining;
                    h->waiting_data = 0;
                }
                else if (e.parameter == h->waiting_transfer || (cc != 1 && cc != 13))
                {
                    h->transfer_code = cc;
                    h->completion |= TRANSFER_READY;
                }
            }
            else if (!d->disconnected)
            {
                bool handled = false;
#if defined(LUME_LINUX_USB_STORAGE) || defined(LUME_LINUX_USB_AUDIO)
                for (unsigned n = 0; n < d->linux_endpoint_count; ++n)
                {
                    struct linux_endpoint *endpoint = &d->linux_endpoints[n];
                    struct urb *urb = endpoint->active;
                    if (!urb || ep != endpoint->id || e.parameter != endpoint->trb)
                        continue;
                    handled = true;
                    endpoint->active = NULL;
                    unsigned remaining = e.status & 0xffffffu;
                    int status = 0;
                    unsigned bytes = 0;
                    bool isochronous_underrun =
                        endpoint->type == USB_ENDPOINT_XFER_ISOC &&
                        !(endpoint->address & USB_DIR_IN) && cc == 14;
                    if ((cc == 1 || cc == 13 || isochronous_underrun) &&
                        remaining <= endpoint->active_bytes)
                    {
                        bytes = endpoint->active_bytes - remaining;
                        if ((endpoint->address & USB_DIR_IN) &&
                            !lume_dma_sync_for_cpu(urb->transfer_buffer,
                                                   (size_t)urb->transfer_buffer_length,
                                                   LUME_DMA_FROM_DEVICE))
                            status = -LUME_EIO;
                    }
                    else
                        status = -LUME_EIO;
                    if (status)
                    {
                        ++h->errors;
                        lume_value("USB_LINUX_ENDPOINT_ERROR", endpoint->id);
                        lume_value("USB_LINUX_COMPLETION_ERROR", cc);
                        lume_value("USB_LINUX_REMAINING_BYTES", remaining);
                    }
                    lume_linux_usb_complete_urb(
                        urb, endpoint->address & USB_DIR_IN ? urb->transfer_buffer : NULL, bytes,
                        status);
                    int armed = arm_linux_endpoint(h, d, endpoint);
                    if (armed)
                    {
                        ++h->errors;
                        lume_value("USB_LINUX_ARM_ERROR", (unsigned)-armed);
                    }
                    break;
                }
#endif
                if (handled)
                    goto transfer_handled;
                for (unsigned n = 0; n < d->inputs; ++n)
                {
                    struct input *input = d->channels + n;
                    if (!input->pending || ep != input->endpoint || e.parameter != input->trb)
                        continue;
                    input->pending = false;
                    unsigned remaining = e.status & 0xffffff;
                    if ((cc == 1 || cc == 13) && remaining <= input->packet)
                    {
                        unsigned bytes = input->packet - remaining;
                        cache(input->report, 64, true);
#if defined(LUME_LINUX_USB_HID)
                        lume_linux_usb_hid_complete(input->linux_hid, input->report, bytes, 0,
                                                    milliseconds(h));
#else
                        if (input->mouse)
                        {
                            ++input->hid.reports;
                            lume_hid_mouse_report(input->report, bytes, milliseconds(h));
                        }
                        else
                            lume_hid_report(&input->hid, input->report, bytes, milliseconds(h));
#endif
                    }
                    else
                    {
                        ++h->errors;
                        d->disconnected = true;
                        input->hid.repeat_key = 0;
#if defined(LUME_LINUX_USB_HID)
                        lume_linux_usb_hid_complete(input->linux_hid, NULL, 0, -LUME_EIO,
                                                    milliseconds(h));
#endif
                        lume_value("USB_HID_COMPLETION_ENDPOINT", ep);
                        lume_value("USB_HID_COMPLETION_ERROR", cc);
                    }
                    break;
                }
            transfer_handled:
                ;
            }
        }
        else if (type == 37)
        {
            ++h->errors;
            lume_value("USB_HOST_EVENT_ERROR", cc);
        }
        if (++h->event_head == 256)
        {
            h->event_head = 0;
            h->event_cycle ^= 1;
        }
    }
    if (advanced)
        write64(h->rt + 0x38, dma(&h->events[h->event_head]) | 8);
    if (advanced || (lume_mmio_read32(h->rt + 0x20) & 1))
        lume_mmio_write32(h->rt + 0x20, 1); /* IP W1C with interrupt delivery disabled. */
    h->event_polling = false;
    return advanced;
}

static bool command(struct host *h, unsigned type, uint64_t parameter, unsigned control,
                    unsigned *slot)
{
    if (h->operation == HOST_IDLE || h->waiting_command || h->waiting_transfer)
        return false;
    h->command_code = 0;
    h->completion &= ~COMMAND_READY;
    h->waiting_command = enqueue(&h->command, parameter, 0, (type << 10) | control);
    lume_mmio_write32(h->doorbells, 0);
    uint64_t deadline = lume_counter_read() + h->frequency;
    for (;;)
    {
        events(h);
        if (h->command_code)
            break;
        if ((int64_t)(lume_counter_read() - deadline) >= 0)
            break;
        wait_progress(h, COMMAND_READY, deadline);
    }
    h->waiting_command = 0;
    if (h->command_code != 1)
    {
        ++h->errors;
        lume_value("USB_COMMAND_TYPE", type);
        lume_value("USB_COMMAND_ERROR", h->command_code);
        return false;
    }
    if (slot)
        *slot = h->command_slot;
    return true;
}

static bool control_timed(struct host *h, struct device *d, uint8_t request_type, uint8_t request,
                          uint16_t value, uint16_t index, uint16_t bytes, unsigned timeout_ms,
                          unsigned *actual)
{
    if (bytes > 512 || d->control_blocked || h->operation == HOST_IDLE || h->waiting_command ||
        h->waiting_transfer)
        return false;
    bool input = (request_type & 0x80) != 0;
    uint64_t setup = request_type | ((uint64_t)request << 8) | ((uint64_t)value << 16) |
                     ((uint64_t)index << 32) | ((uint64_t)bytes << 48);
    h->transfer_code = 0;
    h->transfer_slot = d->slot;
    h->transfer_actual = h->transfer_bytes = bytes;
    h->transfer_invalid = false;
    h->waiting_data = 0;
    h->completion &= ~TRANSFER_READY;
    if (bytes)
    {
        cache(d->buffer, 512, false);
        cache(d->buffer, 512, true);
    }
    enqueue(&d->ep0, setup, 8, (2u << 10) | 64 | (bytes ? ((input ? 3u : 2u) << 16) : 0));
    if (bytes)
        h->waiting_data =
            enqueue(&d->ep0, dma(d->buffer), bytes, (3u << 10) | (input ? (1u << 16) | 4u : 0));
    h->waiting_transfer =
        enqueue(&d->ep0, 0, 0, (4u << 10) | 32 | ((!bytes || !input) ? 1u << 16 : 0));
    lume_mmio_write32(h->doorbells + d->slot * 4, 1);
    uint64_t ticks =
        (h->frequency / 1000) * timeout_ms + ((h->frequency % 1000) * timeout_ms + 999) / 1000;
    uint64_t deadline = lume_counter_read() + ticks;
    for (;;)
    {
        events(h);
        if (h->transfer_code)
            break;
        if ((int64_t)(lume_counter_read() - deadline) >= 0)
            break;
        wait_progress(h, TRANSFER_READY, deadline);
    }
    h->waiting_transfer = 0;
    h->waiting_data = 0;
    h->transfer_slot = 0;
    if (h->transfer_code != 1 || h->transfer_invalid)
    {
        d->control_blocked = true;
        ++h->errors;
        lume_value("USB_CONTROL_SLOT", d->slot);
        lume_value("USB_CONTROL_REQUEST", ((unsigned)request_type << 8) | request);
        lume_value("USB_CONTROL_ERROR", h->transfer_code);
        return false;
    }
    if (bytes && input)
        cache(d->buffer, 512, true);
    *actual = h->transfer_actual;
    return true;
}

static bool control(struct host *h, struct device *d, uint8_t request_type, uint8_t request,
                    uint16_t value, uint16_t index, uint16_t bytes)
{
    unsigned actual;
    return control_timed(h, d, request_type, request, value, index, bytes, 500, &actual) &&
           actual == bytes;
}

#if defined(LUME_LINUX_USB_HID)
static int linux_control(void *context, uint8_t type, uint8_t request, uint16_t value,
                         uint16_t index, void *data, uint16_t bytes, unsigned timeout_ms)
{
    struct control_context *wire = context;
    struct host *host = wire->host;
    struct device *device = wire->device;
    if (device->disconnected)
        return -LUME_ENXIO;
    if (device->control_blocked)
        return -LUME_EIO;
    if (bytes > 512 || (bytes && !data))
        return -LUME_EINVAL;
    if (host->operation == HOST_STOP || host->operation == HOST_CONTROL || host->waiting_command ||
        host->waiting_transfer || host->event_polling)
        return -LUME_EBUSY;
    if (timeout_ms > 500)
        timeout_ms = 500;
    bool input = type & 0x80u;
    if (bytes && !input && data != device->buffer)
        copy(device->buffer, data, bytes);
    bool runtime = host->operation == HOST_IDLE;
    /* Keep topology and shutdown from releasing this slot while EP0 is active. */
    if (runtime)
        host->operation = HOST_CONTROL;
    unsigned actual;
    bool done =
        control_timed(host, device, type, request, value, index, bytes, timeout_ms, &actual);
    if (runtime)
        host->operation = HOST_IDLE;
    if (!done)
        return host->transfer_code ? -LUME_EIO : -LUME_ETIMEDOUT;
    if (actual && input && data != device->buffer)
        copy(data, device->buffer, actual);
    if (input && actual < bytes)
        lume_value("USB_LINUX_CONTROL_SHORT_BYTES", actual);
    return (int)actual;
}
#endif

static uint32_t *context(struct host *h, struct device *d, unsigned index)
{
    return (void *)((unsigned char *)d->input + index * h->context_bytes);
}

static void slot_context(struct host *h, struct device *d, unsigned entries)
{
    uint32_t *s = context(h, d, 1);
    s[0] = d->route | (d->speed << 20) | (d->mtt << 25) | (entries << 27);
    s[1] = d->root << 16;
    s[2] = d->tt_slot | (d->tt_port << 8);
}

static bool address_device(struct host *h, struct device *d)
{
    unsigned slot;
    if (!command(h, 9, 0, 0, &slot) || !slot || slot > MAX_SLOTS)
        return false;
    unsigned speed = d->speed, root = d->root, depth = d->depth, route = d->route,
             tt_slot = d->tt_slot, tt_port = d->tt_port, mtt = d->mtt;
    struct device *actual = &h->devices[slot];
    *actual = *d;
    d = actual;
    d->slot = slot;
    d->speed = speed;
    d->root = root;
    d->depth = depth;
    d->route = route;
    d->tt_slot = tt_slot;
    d->tt_port = tt_port;
    d->mtt = mtt;
    d->input = page(h);
    d->output = page(h);
    d->buffer = page(h);
    if (!d->input || !d->output || !d->buffer || !ring_init(h, &d->ep0))
        return false;
    h->dcbaa[slot] = dma(d->output);
    cache(h->dcbaa, 4096, false);
    context(h, d, 0)[1] = 3;
    slot_context(h, d, 1);
    d->ep0_packet = speed == 3 ? 64 : speed >= 4 ? 512 : 8;
    uint32_t *ep = context(h, d, 2);
    ep[1] = (d->ep0_packet << 16) | (4u << 3) | (3u << 1);
    ep[2] = (uint32_t)d->ep0.dma | 1;
    ep[3] = (uint32_t)(d->ep0.dma >> 32);
    ep[4] = 8;
    cache(d->input, 4096, false);
    if (!command(h, 11, dma(d->input), slot << 24, 0))
        return false;
    delay(h, 10000);
    return true;
}

static bool update_ep0(struct host *h, struct device *d, unsigned packet)
{
    if (packet == d->ep0_packet)
        return true;
    if (packet != 8 && packet != 16 && packet != 32 && packet != 64)
        return false;
    cache(d->output, 4096, true);
    zero(d->input, 4096);
    context(h, d, 0)[1] = 2;
    uint32_t *ep = context(h, d, 2);
    copy(ep, (unsigned char *)d->output + h->context_bytes, h->context_bytes);
    ep[1] = (ep[1] & 0xffff) | (packet << 16);
    cache(d->input, 4096, false);
    if (!command(h, 13, dma(d->input), d->slot << 24, 0))
        return false;
    d->ep0_packet = packet;
    return true;
}

static bool input_device(struct host *h, struct device *d, const struct lume_usb_interface *i)
{
    if (d->inputs == LUME_USB_INPUTS)
        return false;
#if !defined(LUME_LINUX_USB_HID)
    if (!control(h, d, 0x21, 11, 0, i->interface, 0))
        return false; /* SET_PROTOCOL(BOOT) */
#endif
    struct input *input = d->channels + d->inputs;
    input->endpoint = (i->endpoint & 15) * 2 + 1;
    input->packet = i->packet;
    if (!ring_init(h, &input->ring) || !(input->report = page(h)))
        return false;
    cache(d->output, 4096, true);
    zero(d->input, 4096);
    context(h, d, 0)[1] = 1 | (1u << input->endpoint);
    copy(context(h, d, 1), d->output, h->context_bytes);
    uint32_t *s = context(h, d, 1);
    unsigned entries = s[0] >> 27;
    if (entries < input->endpoint)
        s[0] = (s[0] & 0x07ffffff) | (input->endpoint << 27);
    unsigned interval = i->interval;
    if (d->speed >= 3)
        interval = interval > 16 ? 15 : interval - 1;
    else
    {
        unsigned log = 0;
        while ((1u << log) < interval && log < 8)
            ++log;
        interval = log + 3;
    }
    uint32_t *ep = context(h, d, input->endpoint + 1);
    ep[0] = interval << 16;
    ep[1] = (i->packet << 16) | (7u << 3) | (3u << 1);
    ep[2] = (uint32_t)input->ring.dma | 1;
    ep[3] = (uint32_t)(input->ring.dma >> 32);
    ep[4] = i->packet | ((unsigned)i->packet << 16);
    cache(d->input, 4096, false);
    if (!command(h, 12, dma(d->input), d->slot << 24, 0))
        return false;
#if defined(LUME_LINUX_USB_HID)
    input->control = (struct control_context){h, d};
    input->linux_hid = lume_linux_usb_hid_attach(i, d->vendor, d->product, (uint8_t)d->slot,
                                                 &input->hid, &input->control, linux_control);
    if (!input->linux_hid)
        return false;
#endif
    input->keyboard = i->keyboard;
    input->mouse = i->mouse;
    ++d->inputs;
    lume_value("USB_HID_INTERFACE", i->interface);
    if (input->keyboard)
    {
        ++keyboard_count;
        lume_value("USB_KEYBOARD_SLOT", d->slot);
        lume_value("USB_KEYBOARD_ENDPOINT", input->endpoint);
        lume_puts("LUME USB HID KEYBOARD READY layout=PT\n");
    }
    else
    {
        ++mouse_count;
        lume_value("USB_MOUSE_SLOT", d->slot);
        lume_value("USB_MOUSE_ENDPOINT", input->endpoint);
        lume_puts("LUME USB HID MOUSE READY\n");
    }
    arm_input(h, d, input);
    return true;
}

#if defined(LUME_LINUX_USB_STORAGE) || defined(LUME_LINUX_USB_AUDIO)
static bool configure_linux_endpoints(struct host *host, struct device *device,
                                      const struct lume_usb_endpoint *const *descriptors,
                                      unsigned count)
{
    cache(device->output, 4096, true);
    zero(device->input, 4096);
    context(host, device, 0)[1] = 1;
    copy(context(host, device, 1), device->output, host->context_bytes);
    uint32_t *slot = context(host, device, 1);
    unsigned entries = slot[0] >> 27;
    device->linux_endpoint_count = 0;
    for (unsigned index = 0; index < count; ++index)
    {
        const struct lume_usb_endpoint *descriptor = descriptors[index];
        struct linux_endpoint *endpoint =
            &device->linux_endpoints[device->linux_endpoint_count++];
        endpoint->address = descriptor->address;
        endpoint->id = (descriptor->address & USB_ENDPOINT_NUMBER_MASK) * 2 +
                       ((descriptor->address & USB_DIR_IN) ? 1u : 0u);
        endpoint->packet = descriptor->packet;
        endpoint->type = descriptor->attributes & USB_ENDPOINT_XFERTYPE_MASK;
        if (!ring_init(host, &endpoint->ring))
            return false;
        context(host, device, 0)[1] |= 1u << endpoint->id;
        if (entries < endpoint->id)
            entries = endpoint->id;
        unsigned interval = descriptor->interval;
        if (endpoint->type == USB_ENDPOINT_XFER_ISOC)
        {
            if (device->speed >= 3)
                interval = interval > 16 ? 15 : interval - 1;
            else
            {
                unsigned log = 0;
                while ((1u << log) < interval && log < 8)
                    ++log;
                interval = log + 3;
            }
        }
        uint32_t *ep = context(host, device, endpoint->id + 1);
        bool input = descriptor->address & USB_DIR_IN;
        unsigned type = endpoint->type == USB_ENDPOINT_XFER_ISOC ? (input ? 5u : 1u)
                                                                 : (input ? 6u : 2u);
        unsigned errors = endpoint->type == USB_ENDPOINT_XFER_ISOC ? 0 : 3;
        ep[0] = interval << 16;
        ep[1] = (descriptor->packet << 16) | (type << 3) | (errors << 1);
        ep[2] = (uint32_t)endpoint->ring.dma | 1;
        ep[3] = (uint32_t)(endpoint->ring.dma >> 32);
        ep[4] = descriptor->packet |
                (endpoint->type == USB_ENDPOINT_XFER_ISOC ? descriptor->packet << 16 : 0);
    }
    slot[0] = (slot[0] & 0x07ffffff) | (entries << 27);
    cache(device->input, 4096, false);
    if (!command(host, 12, dma(device->input), device->slot << 24, NULL))
        return false;
    device->linux_control = (struct control_context){host, device};
    device->linux_device = (struct usb_device){.vendor = device->vendor,
                                                .product = device->product,
                                                .address = (u8)device->slot,
                                                .online = true,
                                                .transport_context = &device->linux_control,
                                                .control = linux_control,
                                                .submit = linux_submit};
    return true;
}
#endif

#if defined(LUME_LINUX_USB_STORAGE)
static bool linux_bulk_pair(const struct lume_usb_configuration *configuration,
                            const struct lume_usb_endpoint **input,
                            const struct lume_usb_endpoint **output)
{
    *input = NULL;
    *output = NULL;
    unsigned interface = UINT32_MAX;
    for (unsigned index = 0; index < configuration->endpoints; ++index)
    {
        const struct lume_usb_endpoint *endpoint = &configuration->endpoint[index];
        if (endpoint->alternate || endpoint->interface_class != 8 ||
            endpoint->interface_subclass != 6 ||
            endpoint->interface_protocol != 0x50 || (endpoint->attributes & 3) != 2)
            continue;
        if (interface == UINT32_MAX)
            interface = endpoint->interface;
        if (interface != endpoint->interface)
            continue;
        if (endpoint->address & USB_DIR_IN)
            *input = endpoint;
        else
            *output = endpoint;
    }
    return *input && *output;
}

static bool linux_bulk_device(struct host *host, struct device *device,
                              const struct lume_usb_configuration *configuration)
{
    const struct lume_usb_endpoint *input, *output;
    if (!linux_bulk_pair(configuration, &input, &output))
        return false;
    const struct lume_usb_endpoint *descriptors[] = {output, input};
    if (!configure_linux_endpoints(host, device, descriptors, 2))
        return false;
    device->storage = lume_linux_usb_storage_attach(
        &device->linux_device, input->address & USB_ENDPOINT_NUMBER_MASK,
        output->address & USB_ENDPOINT_NUMBER_MASK);
    if (!device->storage)
    {
        device->linux_device.online = false;
        return false;
    }
    lume_value("USB_LINUX_BULK_IN_ENDPOINT", input->address);
    lume_value("USB_LINUX_BULK_OUT_ENDPOINT", output->address);
    return true;
}
#endif

#if defined(LUME_LINUX_USB_AUDIO)
static const struct lume_usb_endpoint *linux_audio_endpoint(
    const struct lume_usb_configuration *configuration)
{
    for (unsigned index = 0; index < configuration->endpoints; ++index)
    {
        const struct lume_usb_endpoint *endpoint = &configuration->endpoint[index];
        if (endpoint->alternate && endpoint->interface_class == 1 &&
            endpoint->interface_subclass == 2 &&
            (endpoint->attributes & USB_ENDPOINT_XFERTYPE_MASK) == USB_ENDPOINT_XFER_ISOC &&
            !(endpoint->address & USB_DIR_IN) && endpoint->interval)
            return endpoint;
    }
    return NULL;
}

static bool linux_audio_device(struct host *host, struct device *device,
                               const struct lume_usb_configuration *configuration)
{
    const struct lume_usb_endpoint *endpoint = linux_audio_endpoint(configuration);
    if (!endpoint ||
        !control(host, device, 1, 11, endpoint->alternate, endpoint->interface, 0))
        return false;
    const struct lume_usb_endpoint *descriptors[] = {endpoint};
    if (!configure_linux_endpoints(host, device, descriptors, 1))
        return false;
    device->audio = lume_linux_usb_audio_attach(
        &device->linux_device, endpoint->address & USB_ENDPOINT_NUMBER_MASK, endpoint->packet,
        endpoint->interval);
    if (!device->audio)
    {
        device->linux_device.online = false;
        return false;
    }
    lume_value("USB_LINUX_AUDIO_ENDPOINT", endpoint->address);
    lume_value("USB_LINUX_AUDIO_PACKET", endpoint->packet);
    return true;
}
#endif

static bool enumerate(struct host *, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned,
                      unsigned);

static bool hub(struct host *h, struct device *d, unsigned protocol)
{
    /* USB 2 high-speed hubs with protocol 2 expose multiple-TT operation in alternate setting
     * 1. The endpoint contexts must use that setting. */
    if (protocol == 2 && d->speed == 3 && !control(h, d, 1, 11, 1, 0, 0))
        return false;
    if (!control(h, d, 0xa0, 6, 0x2900, 0, 7))
        return false;
    unsigned bytes = d->buffer[0], ports = d->buffer[2],
             characteristics = d->buffer[3] | ((unsigned)d->buffer[4] << 8), power = d->buffer[5];
    if (bytes < 9 || bytes > 32 || !ports || ports > 15 || d->depth >= 4)
        return false;
    if (!control(h, d, 0xa0, 6, 0x2900, 0, (uint16_t)bytes))
        return false;
    cache(d->output, 4096, true);
    zero(d->input, 4096);
    context(h, d, 0)[1] = 1;
    uint32_t *s = context(h, d, 1);
    copy(s, d->output, h->context_bytes);
    s[0] |= 1u << 26;
    if (protocol == 2 && d->speed == 3)
    {
        s[0] |= 1u << 25;
        d->mtt = 1;
    }
    s[1] = (s[1] & 0x00ffffff) | (ports << 24);
    if (d->speed == 3)
        s[2] = (s[2] & ~(3u << 16)) | (((characteristics >> 5) & 3) << 16);
    s[3] = 0;
    cache(d->input, 4096, false);
    /* xHCI 1.x updates the hub and transaction translator through Configure Endpoint. */
    if (!command(h, 12, dma(d->input), d->slot << 24, 0))
        return false;
    ++hub_count;
    d->hub = true;
    d->hub_ports = ports;
    lume_value("USB_HUB_SLOT", d->slot);
    lume_value("USB_HUB_PORTS", ports);
    for (unsigned port = 1; port <= ports; ++port)
        if (!control(h, d, 0x23, 3, 8, (uint16_t)port, 0))
            return false;
    delay(h, 20000 + power * 2000);
    for (unsigned port = 1; port <= ports; ++port)
    {
        if (!control(h, d, 0xa3, 0, 0, (uint16_t)port, 4))
            return false;
        unsigned status = d->buffer[0] | ((unsigned)d->buffer[1] << 8);
        if (!control(h, d, 0x23, 1, 16, (uint16_t)port, 0))
            return false;
        if (!(status & 1))
            continue;
        if (!control(h, d, 0x23, 3, 4, (uint16_t)port, 0))
            return false;
        bool ready = false;
        for (unsigned tries = 0; tries < 50; ++tries)
        {
            delay(h, 10000);
            if (!control(h, d, 0xa3, 0, 0, (uint16_t)port, 4))
                return false;
            status = d->buffer[0] | ((unsigned)d->buffer[1] << 8);
            if ((status & 3) == 3 && !(status & 16))
            {
                ready = true;
                break;
            }
        }
        if (!ready)
        {
            lume_value("USB_HUB_RESET_FAILED", port);
            continue;
        }
        if (!control(h, d, 0x23, 1, 20, (uint16_t)port, 0) ||
            !control(h, d, 0x23, 1, 16, (uint16_t)port, 0))
            return false;
        unsigned speed = (status & 0x200) ? 2 : (status & 0x400) ? 3 : 1;
        unsigned tt_slot = speed <= 2 ? (d->speed == 3 ? d->slot : d->tt_slot) : 0;
        unsigned tt_port = speed <= 2 ? (d->speed == 3 ? port : d->tt_port) : 0;
        unsigned route = d->route | (port << (d->depth * 4));
        if (!enumerate(h, d->root, speed, d->depth + 1, route, tt_slot, tt_port, d->mtt))
            lume_value("USB_HUB_CHILD_FAILED", port);
    }
    return true;
}

static bool enumerate(struct host *h, unsigned root, unsigned speed, unsigned depth, unsigned route,
                      unsigned tt_slot, unsigned tt_port, unsigned mtt)
{
    if (speed < 1 || speed > 3)
        return false; /* USB2 keyboard/hub path only. */
    struct device requested = {0};
    requested.speed = speed;
    requested.root = root;
    requested.depth = depth;
    requested.route = route;
    requested.tt_slot = tt_slot;
    requested.tt_port = tt_port;
    requested.mtt = mtt;
    if (!address_device(h, &requested))
        return false;
    unsigned slot = h->command_slot;
    struct device *d = &h->devices[slot];
    if (!control(h, d, 0x80, 6, 0x100, 0, 8) || d->buffer[0] != 18 || d->buffer[1] != 1)
        return false;
    if (!update_ep0(h, d, d->buffer[7]))
        return false;
#if defined(LUME_LINUX_USB_HID)
    struct control_context wire = {h, d};
    if (!lume_linux_usb_device_descriptor((uint8_t)d->slot, &wire, linux_control, d->buffer, 64))
        return false;
#else
    if (!control(h, d, 0x80, 6, 0x100, 0, 18))
        return false;
#endif
    unsigned device_class = d->buffer[4], protocol = d->buffer[6];
#if defined(LUME_ARCH_AMD64) || defined(LUME_LINUX_USB_HID)
    uint16_t vendor = d->buffer[8] | (uint16_t)d->buffer[9] << 8;
    uint16_t product = d->buffer[10] | (uint16_t)d->buffer[11] << 8;
#endif
#if defined(LUME_LINUX_USB_HID)
    d->vendor = vendor;
    d->product = product;
#endif
#if defined(LUME_ARCH_AMD64)
    uint32_t parent = pci_hosts[h->index].parent;
    if (depth)
        for (unsigned n = 1; n <= MAX_SLOTS; ++n)
        {
            struct device *p = h->devices + n;
            uint32_t mask = depth == 1 ? 0 : (1u << ((depth - 1) * 4)) - 1;
            if (p->hub && p->root == root && p->depth + 1 == depth && p->route == (route & mask))
                parent = p->inventory;
        }
    if (lume_usb_inventory_add(parent, root, route, slot, vendor, product, device_class,
                               &d->inventory))
        return false;
#endif
    lume_value("USB_HOST", h->index);
    lume_value("USB_DEVICE_SLOT", d->slot);
    lume_value("USB_VID_PID", ((uint32_t)d->buffer[9] << 24) | ((uint32_t)d->buffer[8] << 16) |
                                  ((uint32_t)d->buffer[11] << 8) | d->buffer[10]);
    if (!control(h, d, 0x80, 6, 0x200, 0, 9) || d->buffer[0] != 9 || d->buffer[1] != 2)
        return false;
    unsigned bytes = d->buffer[2] | ((unsigned)d->buffer[3] << 8);
    if (bytes < 9 || bytes > 512 || !control(h, d, 0x80, 6, 0x200, 0, (uint16_t)bytes))
        return false;
    struct lume_usb_configuration selected;
    if (!lume_usb_configuration_inputs(d->buffer, bytes, &selected))
        return false;
    lume_value("USB_HID_INTERFACES", selected.inputs);
    bool bulk = false, audio = false;
#if defined(LUME_LINUX_USB_STORAGE)
    const struct lume_usb_endpoint *bulk_input, *bulk_output;
    bulk = linux_bulk_pair(&selected, &bulk_input, &bulk_output);
#endif
#if defined(LUME_LINUX_USB_AUDIO)
    audio = linux_audio_endpoint(&selected) != NULL;
#endif
    if (!selected.inputs && !selected.hub && device_class != 9 && !bulk && !audio)
    {
#if defined(LUME_ARCH_AMD64)
        (void)lume_usb_inventory_state(d->inventory, LUME_HARDWARE_UNSUPPORTED, -LUME_ENOSYS);
#endif
        lume_puts("USB DEVICE NO INPUT DRIVER\n");
        return true;
    }
    if (!control(h, d, 0, 9, selected.value, 0, 0))
        return false;
    delay(h, 10000);
    bool ready = true;
    if (selected.hub || device_class == 9)
        ready = hub(h, d, protocol);
#if defined(LUME_LINUX_USB_STORAGE)
    else if (bulk)
        ready = linux_bulk_device(h, d, &selected);
#endif
#if defined(LUME_LINUX_USB_AUDIO)
    else if (audio)
        ready = linux_audio_device(h, d, &selected);
#endif
    else
        for (unsigned n = 0; n < selected.inputs; ++n)
            if (!input_device(h, d, selected.input + n))
            {
                ready = false;
                break;
            }
#if defined(LUME_ARCH_AMD64)
    (void)lume_usb_inventory_state(d->inventory, ready ? LUME_HARDWARE_READY : LUME_HARDWARE_FAILED,
                                   ready ? 0 : -LUME_EIO);
#endif
    return ready;
}

static bool remove_root(struct host *h, unsigned root)
{
    for (unsigned depth = 5; depth-- > 0;)
        for (unsigned slot = 1; slot <= MAX_SLOTS; ++slot)
        {
            struct device *d = &h->devices[slot];
            if (!d->slot || d->root != root || d->depth != depth)
                continue;
            d->disconnected = true;
            if (!command(h, 10, 0, slot << 24, 0))
                return false;
#if defined(LUME_ARCH_AMD64)
            if (d->inventory && lume_usb_inventory_remove(d->inventory))
                return false;
#endif
            h->dcbaa[slot] = 0;
            cache(h->dcbaa, 4096, false);
#if defined(LUME_LINUX_USB_STORAGE) || defined(LUME_LINUX_USB_AUDIO)
            d->linux_device.online = false;
#endif
#if defined(LUME_LINUX_USB_STORAGE)
            lume_linux_usb_storage_detach(d->storage);
            d->storage = NULL;
#endif
#if defined(LUME_LINUX_USB_AUDIO)
            lume_linux_usb_audio_detach(d->audio);
            d->audio = NULL;
#endif
#if defined(LUME_LINUX_USB_STORAGE) || defined(LUME_LINUX_USB_AUDIO)
            for (unsigned n = 0; n < d->linux_endpoint_count; ++n)
                release_page(h, d->linux_endpoints[n].ring.trbs);
#endif
            for (unsigned n = 0; n < LUME_USB_INPUTS; ++n)
            {
                struct input *input = d->channels + n;
#if defined(LUME_LINUX_USB_HID)
                lume_linux_usb_hid_detach(input->linux_hid);
#endif
                const uint8_t released[8] = {0};
                if (input->keyboard)
                {
                    --keyboard_count;
                    lume_hid_report(&input->hid, released, sizeof(released), milliseconds(h));
                }
                if (input->mouse)
                {
                    --mouse_count;
                    lume_hid_mouse_report(released, sizeof(released), milliseconds(h));
                }
                h->retired_reports += input->hid.reports;
                h->retired_characters += input->hid.characters;
                h->retired_dropped += input->hid.dropped;
                release_page(h, input->report);
                release_page(h, input->ring.trbs);
            }
            if (d->hub)
                --hub_count;
            release_page(h, d->input);
            release_page(h, d->output);
            release_page(h, d->buffer);
            release_page(h, d->ep0.trbs);
            *d = (struct device){0};
        }
    h->root_ready &= ~(1u << (root - 1));
    return true;
}

static void scan_roots(struct host *h, bool initial)
{
    uint32_t now = milliseconds(h);
    if ((h->operation != HOST_START && h->operation != HOST_SCAN) || h->scanning ||
        (!initial && (int32_t)(now - h->next_scan) < 0))
        return;
    h->scanning = true;
    h->next_scan = now + 1000;
    for (unsigned port = 1; port <= h->ports; ++port)
    {
        uint64_t reg = h->op + 0x400 + (port - 1) * 16;
        uint32_t status = lume_mmio_read32(reg), mask = 1u << (port - 1);
        bool connected = (status & 1) && ((status >> 10) & 15) < 4;
        bool failed = false, allocated = false;
        for (unsigned slot = 1; slot <= MAX_SLOTS; ++slot)
            if (h->devices[slot].slot && h->devices[slot].root == port)
            {
                allocated = true;
                failed |= h->devices[slot].disconnected;
            }
        if (connected && (h->root_ready & mask) && !failed)
        {
            /* A hub connection change rebuilds its root subtree after disabling all slots. */
            bool changed = false;
            for (unsigned slot = 1; slot <= MAX_SLOTS && !changed; ++slot)
            {
                struct device *d = h->devices + slot;
                if (!d->hub || d->disconnected || d->root != port)
                    continue;
                for (unsigned p = 1; p <= d->hub_ports; ++p)
                    if (!control(h, d, 0xa3, 0, 0, (uint16_t)p, 4) || (d->buffer[2] & 1))
                    {
                        changed = true;
                        break;
                    }
            }
            if (!changed)
                continue;
        }
        if (!connected && !allocated)
            continue;
        if (!remove_root(h, port) || !connected)
            continue;
        lume_mmio_write32(reg, (1u << 9) | 16);
        if (!wait_register(h, reg, 18, 2, 300))
        {
            lume_value("USB_ROOT_RESET_FAILED", port);
            continue;
        }
        delay(h, 10000);
        status = lume_mmio_read32(reg);
        unsigned speed = (status >> 10) & 15;
        lume_mmio_write32(reg, (1u << 9) | 0xfe0000);
        if (enumerate(h, port, speed, 0, 0, 0, 0, 0))
        {
            h->root_ready |= mask;
            if (!initial)
            {
                lume_value("USB_HOTPLUG_HUBS", hub_count);
                lume_value("USB_HOTPLUG_KEYBOARDS", keyboard_count);
            }
        }
        else
            lume_value("USB_ROOT_DEVICE_FAILED", port);
    }
    h->next_scan = milliseconds(h) + 1000;
    h->scanning = false;
}

static void topology_work(void *context)
{
    struct host *h = context;
    if (!h->running || h->operation != HOST_IDLE)
        return;
#if defined(LUME_ARCH_AMD64)
    if (!pci_hosts[h->index].present(h->index))
        return;
#endif
    h->operation = HOST_SCAN;
    scan_roots(h, false);
    h->operation = HOST_IDLE;
}

static void queue_topology(struct host *h)
{
    if (h->operation != HOST_IDLE || lume_work_queued(&h->topology) ||
        (int32_t)(milliseconds(h) - h->next_scan) < 0)
        return;
    h->topology.run = topology_work;
    h->topology.context = h;
    h->topology.domain = LUME_WORK_USB;
    (void)lume_work_submit(&h->topology);
}

#if defined(LUME_ARCH_AMD64)
static bool release_firmware(struct host *h, uint32_t hcc, uint64_t size)
{
    uint64_t offset = (hcc >> 16) * 4u;
    while (offset)
    {
        if (offset > size - 4)
            return false;
        uint64_t address = h->base + offset;
        uint32_t support = lume_mmio_read32(address);
        if (support == UINT32_MAX)
            return false;
        if ((support & 255) == 1)
        {
            if (offset > size - 8)
                return false;
            lume_mmio_write32(address, support | (1u << 24));
            if (!wait_register(h, address, 1u << 16, 0, 1000))
            {
                lume_value("USB_FIRMWARE_OWNED", h->index);
                return false;
            }
            /* Disable the legacy SMI sources and acknowledge their pending events. */
            uint32_t control = lume_mmio_read32(address + 4);
            lume_mmio_write32(address + 4, (control & 0x000e1fee) | 0xe0000000);
            return true;
        }
        unsigned next = (support >> 8) & 255;
        if (!next)
            break;
        offset += next * 4u;
    }
    return true;
}
#endif

static bool host_start(struct host *h, unsigned index, uint64_t frequency)
{
    *h = (struct host){0};
    h->operation = HOST_START;
    h->index = index;
    h->frequency = frequency;
#if !defined(LUME_ARCH_AMD64)
    h->base = RP1 + 0x200000 + index * 0x100000;
#else
    h->base = pci_hosts[index].base;
    if (!h->base)
        return false;
#endif
    uint32_t cap = lume_mmio_read32(h->base), hcs = lume_mmio_read32(h->base + 4),
             hcc = lume_mmio_read32(h->base + 16);
    lume_value(index ? "USB1_CAP" : "USB0_CAP", cap);
    if ((cap >> 16) < 0x100 || (cap >> 16) > 0x120 || (cap & 255) < 0x20 || !(hcc & 1))
        return false;
    h->op = h->base + (cap & 255);
    h->rt = h->base + (lume_mmio_read32(h->base + 0x18) & ~31u);
    h->doorbells = h->base + (lume_mmio_read32(h->base + 0x14) & ~3u);
    if (h->rt - h->base > 0x100000 || h->doorbells - h->base > 0x100000)
        return false;
    h->ports = hcs >> 24;
    h->context_bytes = (hcc & 4) ? 64 : 32;
    if (!h->ports || h->ports > 16 || (hcs & 255) < MAX_SLOTS)
        return false;
#if defined(LUME_ARCH_AMD64)
    uint64_t size = pci_hosts[index].size;
    if (h->op - h->base + 0x400 + h->ports * 16 > size || h->rt - h->base + 0x40 > size ||
        h->doorbells - h->base + (MAX_SLOTS + 1) * 4 > size)
        return false;
    if (!release_firmware(h, hcc, size) || !wait_register(h, h->op + 4, 0x800, 0, 1000))
        return false;
#endif
    lume_mmio_write32(h->op, 0);
    if (!wait_register(h, h->op + 4, 1, 1, 200))
        return false;
    lume_mmio_write32(h->op, 2);
    if (!wait_register(h, h->op, 2, 0, 1000) || !wait_register(h, h->op + 4, 0x800, 0, 1000))
        return false;
    if (!(lume_mmio_read32(h->op + 8) & 1))
        return false;
    h->valid = true;
    h->dcbaa = page(h);
    h->events = page(h);
    uint64_t *erst = page(h);
    if (!h->dcbaa || !h->events || !erst || !ring_init(h, &h->command))
        return false;
    unsigned hcs2 = lume_mmio_read32(h->base + 8),
             scratch = ((hcs2 >> 27) & 31) | ((hcs2 >> 16) & 0x3e0);
    if (scratch > 64)
        return false;
    if (scratch)
    {
        uint64_t *array = page(h);
        if (!array)
            return false;
        for (unsigned i = 0; i < scratch; ++i)
        {
            void *p = page(h);
            if (!p)
                return false;
            array[i] = dma(p);
        }
        cache(array, 4096, false);
        h->dcbaa[0] = dma(array);
    }
    cache(h->dcbaa, 4096, false);
    write64(h->op + 0x30, dma(h->dcbaa));
    lume_mmio_write32(h->op + 0x38, MAX_SLOTS);
    write64(h->op + 0x18, h->command.dma | 1);
    erst[0] = dma(h->events);
    erst[1] = 256;
    cache(erst, 64, false);
    h->event_cycle = 1;
    lume_mmio_write32(h->rt + 0x28, 1);
    write64(h->rt + 0x30, dma(erst));
    write64(h->rt + 0x38, dma(h->events));
    lume_mmio_write32(h->rt + 0x24, 0);
    lume_mmio_write32(h->rt + 0x20, 1);
    lume_mmio_write32(h->op, 1);
    if (!wait_register(h, h->op + 4, 1, 0, 200))
        return false;
    h->running = true;
    if (!command(h, 23, 0, 0, 0))
        return false;
    lume_puts(index ? "LUME XHCI1 COMMAND RING PASS\n" : "LUME XHCI0 COMMAND RING PASS\n");
    for (unsigned port = 1; port <= h->ports; ++port)
        lume_mmio_write32(h->op + 0x400 + (port - 1) * 16, 1u << 9);
    delay(h, 100000);
    for (unsigned port = 1; port <= h->ports; ++port)
    {
        uint64_t reg = h->op + 0x400 + (port - 1) * 16;
        uint32_t status = lume_mmio_read32(reg);
        lume_value(index ? "USB1_PORT_STATUS" : "USB0_PORT_STATUS", status);
    }
    scan_roots(h, true);
    return true;
}

bool lume_usb_start_host(unsigned index, uint64_t frequency)
{
    if (index >= LUME_XHCI_HOSTS || !(configured_hosts & (1u << index)) || frequency < 1000 ||
        hosts[index].operation != HOST_IDLE)
        return false;
    if (hosts[index].valid)
        return hosts[index].running;
#if !defined(LUME_ARCH_AMD64)
    if (!lume_pi5_dma_start())
        return false;
#endif
    bool started = host_start(&hosts[index], index, frequency);
    hosts[index].operation = HOST_IDLE;
    return started;
}

bool lume_usb_start(uint64_t frequency)
{
    for (unsigned i = 0; i < LUME_XHCI_HOSTS; ++i)
        if ((configured_hosts & (1u << i)) && !lume_usb_start_host(i, frequency))
            lume_value("USB_HOST_START_FAILED", i);
    lume_value("USB_HUB_COUNT", hub_count);
    lume_value("USB_KEYBOARD_COUNT", keyboard_count);
    lume_value("USB_MOUSE_COUNT", mouse_count);
    return keyboard_count > 0 || mouse_count > 0;
}

void lume_usb_poll(void)
{
    for (unsigned i = 0; i < LUME_XHCI_HOSTS; ++i)
    {
        struct host *h = &hosts[i];
        if (!h->running || h->operation == HOST_STOP)
            continue;
#if defined(LUME_ARCH_AMD64)
        if (!pci_hosts[i].present(i))
            continue;
#endif
        queue_topology(h);
        uint64_t now = lume_counter_read();
        if (h->poll_scheduled && (int64_t)(now - h->next_poll) < 0 && !h->waiting_command &&
            !h->waiting_transfer)
            continue;
        bool advanced = events(h);
        h->poll_scheduled = !advanced;
        h->next_poll = now + (h->frequency + 999) / 1000;
        for (unsigned slot = 1; slot <= MAX_SLOTS; ++slot)
        {
            struct device *d = &h->devices[slot];
            if (!d->inputs || d->disconnected)
                continue;
            if (!(lume_mmio_read32(h->op + 0x400 + (d->root - 1) * 16) & 1))
            {
                d->disconnected = true;
                for (unsigned n = 0; n < d->inputs; ++n)
                    d->channels[n].hid.repeat_key = 0;
                continue;
            }
            for (unsigned n = 0; n < d->inputs; ++n)
            {
                struct input *input = d->channels + n;
                if (input->keyboard)
                    lume_hid_repeat(&input->hid, milliseconds(h));
                arm_input(h, d, input);
#if defined(LUME_LINUX_USB_HID)
                if (input->keyboard && h->operation == HOST_IDLE && !d->control_blocked)
                    lume_linux_usb_hid_sync_caps(input->linux_hid);
#endif
            }
        }
    }
}

int lume_usb_getc(void)
{
#if !defined(LUME_ARCH_AMD64)
    lume_usb_poll();
#endif
    for (unsigned i = 0; i < LUME_XHCI_HOSTS; ++i)
    {
        if (!hosts[i].running)
            continue;
        for (unsigned slot = 1; slot <= MAX_SLOTS; ++slot)
        {
            struct device *d = &hosts[i].devices[slot];
            if (!d->disconnected)
            {
                for (unsigned n = 0; n < d->inputs; ++n)
                    if (d->channels[n].keyboard)
                    {
                        int c = lume_hid_getc(&d->channels[n].hid);
                        if (c >= 0)
                            return c;
                    }
            }
        }
    }
    return -LUME_EAGAIN;
}

unsigned lume_usb_keyboards(void)
{
    return keyboard_count;
}

void lume_usb_stats(void)
{
    unsigned reports = 0, characters = 0, dropped = 0, errors = 0;
    for (unsigned i = 0; i < LUME_XHCI_HOSTS; ++i)
    {
        errors += hosts[i].errors;
        reports += hosts[i].retired_reports;
        characters += hosts[i].retired_characters;
        dropped += hosts[i].retired_dropped;
        for (unsigned slot = 1; slot <= MAX_SLOTS; ++slot)
        {
            struct device *d = &hosts[i].devices[slot];
            for (unsigned n = 0; n < d->inputs; ++n)
            {
                struct lume_hid *h = &d->channels[n].hid;
                reports += h->reports;
                characters += h->characters;
                dropped += h->dropped;
            }
        }
    }
    lume_value("USB_HID_REPORTS", reports);
    lume_value("USB_INPUT_CHARACTERS", characters);
    lume_value("USB_INPUT_DROPPED", dropped);
    lume_value("USB_HOST_ERRORS", errors);
}

bool lume_usb_stop_host(unsigned index)
{
    if (index >= LUME_XHCI_HOSTS)
        return false;
    struct host *h = &hosts[index];
    if (h->operation != HOST_IDLE)
        return false;
    if (!h->valid)
        return true;
    if (lume_work_queued(&h->topology) && !lume_work_cancel(&h->topology))
        return false;
    h->operation = HOST_STOP;
    h->running = false;
    lume_mmio_write32(h->op, 0);
    bool stopped = wait_register(h, h->op + 4, 1, 1, 500);
    lume_value(index ? "USB1_DMA_STOPPED" : "USB0_DMA_STOPPED", stopped);
    if (!stopped)
    {
        h->operation = HOST_IDLE;
        return false;
    }
#if defined(LUME_ARCH_AMD64)
    for (unsigned depth = 5; depth-- > 0;)
        for (unsigned slot = 1; slot <= MAX_SLOTS; ++slot)
        {
            struct device *d = h->devices + slot;
            if (d->inventory && d->depth == depth)
            {
                if (lume_usb_inventory_remove(d->inventory))
                {
                    h->operation = HOST_IDLE;
                    return false;
                }
                d->inventory = 0;
            }
        }
#endif
#if defined(LUME_LINUX_USB_STORAGE) || defined(LUME_LINUX_USB_AUDIO)
    for (unsigned slot = 1; slot <= MAX_SLOTS; ++slot)
    {
        struct device *device = &h->devices[slot];
        device->linux_device.online = false;
#if defined(LUME_LINUX_USB_STORAGE)
        lume_linux_usb_storage_detach(device->storage);
        device->storage = NULL;
#endif
#if defined(LUME_LINUX_USB_AUDIO)
        lume_linux_usb_audio_detach(device->audio);
        device->audio = NULL;
#endif
    }
#endif
    bool ok = true;
    for (unsigned n = 0; n < h->allocated; ++n)
        if (h->pages[n])
        {
            if (lume_page_free(h->pages[n]))
                h->pages[n] = 0;
            else
                ok = false;
        }
    if (!ok)
    {
        h->operation = HOST_IDLE;
        return false;
    }
    for (unsigned slot = 1; slot <= MAX_SLOTS; ++slot)
    {
        for (unsigned n = 0; n < h->devices[slot].inputs; ++n)
        {
#if defined(LUME_LINUX_USB_HID)
            lume_linux_usb_hid_detach(h->devices[slot].channels[n].linux_hid);
#endif
            if (h->devices[slot].channels[n].keyboard && keyboard_count)
                --keyboard_count;
            if (h->devices[slot].channels[n].mouse && mouse_count)
                --mouse_count;
        }
        if (h->devices[slot].hub && hub_count)
            --hub_count;
    }
    *h = (struct host){0};
    return true;
}

bool lume_usb_stop(void)
{
    bool stopped = true;
    for (unsigned i = 0; i < LUME_XHCI_HOSTS; ++i)
        if (!lume_usb_stop_host(i))
            stopped = false;
    return stopped;
}
