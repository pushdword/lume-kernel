/* SPDX-License-Identifier: GPL-2.0-only */
#include <linux/err.h>
#include <linux/scatterlist.h>
#include <linux/usb.h>
#include <lume/kernel_memory.h>

const struct usb_device_id *usb_match_id(struct usb_interface *interface,
                                         const struct usb_device_id *id)
{
    if (!interface || !interface->usb_dev || !interface->cur_altsetting || !id)
        return NULL;
    const struct usb_device *device = interface->usb_dev;
    const struct usb_interface_descriptor *desc = &interface->cur_altsetting->desc;
    const unsigned supported = USB_DEVICE_ID_MATCH_VENDOR | USB_DEVICE_ID_MATCH_PRODUCT |
                               USB_DEVICE_ID_MATCH_INT_CLASS | USB_DEVICE_ID_MATCH_INT_SUBCLASS |
                               USB_DEVICE_ID_MATCH_INT_PROTOCOL;
    for (; id->match_flags; ++id)
    {
        unsigned flags = id->match_flags;
        if ((flags & ~supported) ||
            ((flags & USB_DEVICE_ID_MATCH_VENDOR) && id->idVendor != device->vendor) ||
            ((flags & USB_DEVICE_ID_MATCH_PRODUCT) && id->idProduct != device->product) ||
            ((flags & USB_DEVICE_ID_MATCH_INT_CLASS) &&
             id->bInterfaceClass != desc->bInterfaceClass) ||
            ((flags & USB_DEVICE_ID_MATCH_INT_SUBCLASS) &&
             id->bInterfaceSubClass != desc->bInterfaceSubClass) ||
            ((flags & USB_DEVICE_ID_MATCH_INT_PROTOCOL) &&
             id->bInterfaceProtocol != desc->bInterfaceProtocol))
            continue;
        return id;
    }
    return NULL;
}

struct urb *usb_alloc_urb(int iso_packets, gfp_t flags)
{
    if (iso_packets < 0 || flags != GFP_KERNEL ||
        (size_t)iso_packets >
            (SIZE_MAX - sizeof(struct urb)) / sizeof(struct usb_iso_packet_descriptor))
        return NULL;
    size_t bytes = sizeof(struct urb) +
                   (size_t)iso_packets * sizeof(struct usb_iso_packet_descriptor);
    struct urb *urb = lume_kernel_allocate(bytes);
    if (urb)
    {
        memset(urb, 0, bytes);
        urb->references = 1;
        urb->number_of_packets = (unsigned)iso_packets;
    }
    return urb;
}

struct urb *usb_get_urb(struct urb *urb)
{
    if (urb)
        ++urb->references;
    return urb;
}

void usb_free_urb(struct urb *urb)
{
    if (urb && !--urb->references)
        lume_kernel_free(urb);
}

void init_usb_anchor(struct usb_anchor *anchor)
{
    if (anchor)
        *anchor = (struct usb_anchor){0};
}

void usb_anchor_urb(struct urb *urb, struct usb_anchor *anchor)
{
    if (!urb || !anchor || urb->anchor)
        return;
    usb_get_urb(urb);
    urb->anchor = anchor;
    urb->anchor_previous = anchor->tail;
    if (anchor->tail)
        anchor->tail->anchor_next = urb;
    else
        anchor->head = urb;
    anchor->tail = urb;
}

void usb_unanchor_urb(struct urb *urb)
{
    if (!urb || !urb->anchor)
        return;
    struct usb_anchor *anchor = urb->anchor;
    if (urb->anchor_previous)
        urb->anchor_previous->anchor_next = urb->anchor_next;
    else
        anchor->head = urb->anchor_next;
    if (urb->anchor_next)
        urb->anchor_next->anchor_previous = urb->anchor_previous;
    else
        anchor->tail = urb->anchor_previous;
    urb->anchor = NULL;
    urb->anchor_next = NULL;
    urb->anchor_previous = NULL;
    usb_put_urb(urb);
}

void usb_kill_anchored_urbs(struct usb_anchor *anchor)
{
    while (anchor && anchor->head)
    {
        struct urb *urb = usb_get_urb(anchor->head);
        usb_kill_urb(urb);
        if (urb->anchor == anchor)
            usb_unanchor_urb(urb);
        usb_put_urb(urb);
    }
}

bool usb_anchor_empty(const struct usb_anchor *anchor)
{
    return !anchor || !anchor->head;
}

void usb_fill_int_urb(struct urb *urb, struct usb_device *device, unsigned pipe, void *buffer,
                      int bytes, void (*callback)(struct urb *), void *context, int interval)
{
    urb->dev = device;
    urb->pipe = pipe;
    urb->transfer_buffer = buffer;
    urb->transfer_buffer_length = bytes;
    urb->complete = callback;
    urb->context = context;
    urb->interval = interval;
}

void usb_fill_bulk_urb(struct urb *urb, struct usb_device *device, unsigned pipe, void *buffer,
                       int bytes, void (*callback)(struct urb *), void *context)
{
    urb->dev = device;
    urb->pipe = pipe;
    urb->transfer_buffer = buffer;
    urb->transfer_buffer_length = bytes;
    urb->complete = callback;
    urb->context = context;
    urb->interval = 0;
}

static bool transfer_sg(const struct urb *urb, void *buffer, unsigned bytes, bool to_sg)
{
    struct scatterlist *segment = urb->sg;
    unsigned char *linear = buffer;
    for (int index = 0; bytes && index < urb->num_sgs; ++index)
    {
        if (!segment || (!segment->buffer && segment->length))
            return false;
        unsigned count = bytes < segment->length ? bytes : segment->length;
        if (linear && count)
        {
            if (to_sg)
                memcpy(segment->buffer, linear, count);
            else
                memcpy(linear, segment->buffer, count);
            linear += count;
        }
        bytes -= count;
        if (bytes && index + 1 < urb->num_sgs)
            segment = sg_next(segment);
    }
    return !bytes;
}

static int validate_urb(const struct urb *urb)
{
    if (!urb || !urb->dev || !urb->complete || urb->transfer_buffer_length <= 0 ||
        !usb_pipeendpoint(urb->pipe))
        return -EINVAL;
    unsigned type = usb_pipetype(urb->pipe);
    if (type == PIPE_CONTROL)
        return -EINVAL;
    if (urb->num_sgs)
    {
        unsigned bytes = (unsigned)urb->transfer_buffer_length;
        if (urb->num_sgs < 0 || (type != PIPE_BULK && type != PIPE_INTERRUPT) ||
            bytes > LUME_USB_LINEAR_TRANSFER_MAX || !transfer_sg(urb, NULL, bytes, false))
            return -EINVAL;
    }
    else if (urb->sg || !urb->transfer_buffer)
        return -EINVAL;
    if (type == PIPE_INTERRUPT)
    {
        if (urb->number_of_packets || urb->interval <= 0)
            return -EINVAL;
        const struct usb_device *device = urb->dev;
        if ((usb_pipein(urb->pipe) && device->interrupt_in &&
             usb_pipeendpoint(urb->pipe) !=
                 (device->interrupt_in & USB_ENDPOINT_NUMBER_MASK)) ||
            (device->packet && (unsigned)urb->transfer_buffer_length > device->packet))
            return -EINVAL;
    }
    else if (type == PIPE_BULK)
    {
        if (urb->number_of_packets)
            return -EINVAL;
    }
    else
    {
        if (!urb->number_of_packets || urb->interval <= 0)
            return -EINVAL;
        for (unsigned index = 0; index < urb->number_of_packets; ++index)
        {
            const struct usb_iso_packet_descriptor *packet = &urb->iso_frame_desc[index];
            if (packet->offset > (unsigned)urb->transfer_buffer_length ||
                packet->length > (unsigned)urb->transfer_buffer_length - packet->offset)
                return -EINVAL;
        }
    }
    return 0;
}

static int prepare_sg(struct urb *urb)
{
    unsigned bytes = (unsigned)urb->transfer_buffer_length;
    void *allocation = lume_kernel_allocate(2u * (bytes + SMP_CACHE_BYTES - 1u));
    if (!allocation)
        return -ENOMEM;
    uintptr_t start =
        ((uintptr_t)allocation + SMP_CACHE_BYTES - 1u) & ~(uintptr_t)(SMP_CACHE_BYTES - 1u);
    uintptr_t offset = start & (LUME_USB_LINEAR_TRANSFER_MAX - 1u);
    /* A controller transfer buffer must stay within one 64 KiB DMA region. */
    if (offset + bytes > LUME_USB_LINEAR_TRANSFER_MAX)
        start += LUME_USB_LINEAR_TRANSFER_MAX - offset;
    urb->sg_allocation = allocation;
    urb->sg_buffer = (void *)start;
    if (usb_pipeout(urb->pipe))
        (void)transfer_sg(urb, urb->sg_buffer, bytes, false);
    return 0;
}

static void release_sg(struct urb *urb)
{
    lume_kernel_free(urb->sg_allocation);
    urb->sg_allocation = NULL;
    urb->sg_buffer = NULL;
}

static void remove_pending(struct urb *urb)
{
    if (!urb->submitted)
        return;
    struct usb_device *device = urb->dev;
    if (urb->pending_previous)
        urb->pending_previous->pending_next = urb->pending_next;
    else
        device->pending = urb->pending_next;
    if (urb->pending_next)
        urb->pending_next->pending_previous = urb->pending_previous;
    else
        device->pending_tail = urb->pending_previous;
    urb->pending_next = NULL;
    urb->pending_previous = NULL;
    urb->submitted = false;
}

int usb_submit_urb(struct urb *urb, gfp_t flags)
{
    if (flags != GFP_KERNEL && flags != GFP_ATOMIC)
        return -EINVAL;
    int validation = validate_urb(urb);
    if (validation)
        return validation;
    if (urb->cancelling)
        return -EPERM;
    struct usb_device *device = urb->dev;
    if (!device->online)
        return -ENODEV;
    if (urb->submitted)
        return -EBUSY;
    if (urb->num_sgs)
    {
        int result = prepare_sg(urb);
        if (result)
            return result;
    }
    urb->actual_length = 0;
    urb->status = 0;
    urb->error_count = 0;
    for (unsigned index = 0; index < urb->number_of_packets; ++index)
    {
        urb->iso_frame_desc[index].actual_length = 0;
        urb->iso_frame_desc[index].status = 0;
    }
    ++urb->references;
    urb->pending_previous = device->pending_tail;
    if (device->pending_tail)
        device->pending_tail->pending_next = urb;
    else
        device->pending = urb;
    device->pending_tail = urb;
    urb->submitted = true;
    if (device->submit)
    {
        ++urb->references;
        int result = device->submit(device->transport_context, urb);
        if (result)
        {
            if (urb->submitted)
            {
                remove_pending(urb);
                if (urb->sg_buffer)
                    release_sg(urb);
                usb_free_urb(urb);
            }
            usb_free_urb(urb);
            return result;
        }
        usb_free_urb(urb);
    }
    return 0;
}

static void fail_iso_completion(struct urb *urb, int status)
{
    urb->error_count = urb->number_of_packets;
    for (unsigned index = 0; index < urb->number_of_packets; ++index)
    {
        urb->iso_frame_desc[index].actual_length = 0;
        urb->iso_frame_desc[index].status = status;
    }
}

static int prepare_iso_completion(struct urb *urb, const void *data, unsigned bytes, int status)
{
    if (status)
    {
        fail_iso_completion(urb, status);
        return status;
    }
    unsigned capacity = 0;
    for (unsigned index = 0; index < urb->number_of_packets; ++index)
    {
        unsigned length = urb->iso_frame_desc[index].length;
        if (UINT32_MAX - capacity < length)
            return -EIO;
        capacity += length;
    }
    if (bytes > capacity || bytes > (unsigned)INT32_MAX ||
        (usb_pipein(urb->pipe) && bytes && !data))
    {
        fail_iso_completion(urb, -EIO);
        return -EIO;
    }
    const unsigned char *source = data;
    unsigned remaining = bytes;
    for (unsigned index = 0; index < urb->number_of_packets; ++index)
    {
        struct usb_iso_packet_descriptor *packet = &urb->iso_frame_desc[index];
        unsigned length = remaining < packet->length ? remaining : packet->length;
        if (usb_pipein(urb->pipe) && length)
        {
            memmove((unsigned char *)urb->transfer_buffer + packet->offset, source, length);
            source += length;
        }
        packet->actual_length = length;
        packet->status = 0;
        remaining -= length;
    }
    return 0;
}

void lume_linux_usb_complete_urb(struct urb *urb, const void *data, unsigned bytes, int status)
{
    if (!urb || !urb->submitted)
        return;
    remove_pending(urb);
    usb_unanchor_urb(urb);
    if (usb_pipetype(urb->pipe) == PIPE_ISOCHRONOUS)
        status = prepare_iso_completion(urb, data, bytes, status);
    else if (!status &&
             (bytes > (unsigned)urb->transfer_buffer_length || bytes > (unsigned)INT32_MAX ||
              (usb_pipein(urb->pipe) && bytes && !data)))
        status = -EIO;
    if (!status && bytes && usb_pipein(urb->pipe) &&
        usb_pipetype(urb->pipe) != PIPE_ISOCHRONOUS)
    {
        if (urb->sg_buffer)
        {
            if (!transfer_sg(urb, (void *)data, bytes, true))
                status = -EIO;
        }
        else
            memmove(urb->transfer_buffer, data, bytes);
    }
    if (urb->sg_buffer)
        release_sg(urb);
    urb->status = status;
    urb->actual_length = status ? 0 : (int)bytes;
    urb->complete(urb);
    usb_free_urb(urb);
}

void lume_linux_usb_finish_pipe(struct usb_device *device, unsigned pipe, const void *data,
                                unsigned bytes, int status)
{
    if (!device)
        return;
    for (struct urb *urb = device->pending; urb; urb = urb->pending_next)
        if (urb->pipe == pipe)
        {
            lume_linux_usb_complete_urb(urb, data, bytes, status);
            return;
        }
}

void lume_linux_usb_finish(struct usb_device *device, const void *data, unsigned bytes, int status)
{
    if (device)
        lume_linux_usb_complete_urb(device->pending, data, bytes, status);
}

void usb_kill_urb(struct urb *urb)
{
    if (!urb || !urb->dev || !urb->submitted)
        return;
    ++urb->references;
    urb->cancelling = true;
    if (urb->dev->unlink)
        urb->dev->unlink(urb->dev->transport_context, urb);
    if (urb->submitted)
        lume_linux_usb_complete_urb(urb, NULL, 0, -ENOENT);
    urb->cancelling = false;
    usb_free_urb(urb);
}

int usb_control_msg(struct usb_device *device, unsigned pipe, u8 request, u8 type, u16 value,
                    u16 index, void *data, u16 size, int timeout)
{
    if (!device || !device->online)
        return -ENODEV;
    if (!device->control)
        return -EOPNOTSUPP;
    unsigned expected = type & USB_DIR_IN ? usb_rcvctrlpipe(device, 0) : usb_sndctrlpipe(device, 0);
    if (pipe != expected || timeout <= 0 || (size && !data))
        return -EINVAL;
    int result = device->control(device->transport_context, type, request, value, index, data, size,
                                 (unsigned)timeout);
    return result > size ? -EIO : result;
}
