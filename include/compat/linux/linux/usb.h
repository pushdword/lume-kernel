/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LUME_LINUX_USB_H
#define LUME_LINUX_USB_H
#include <linux/types.h>

#define USB_DIR_IN 0x80u
#define USB_REQ_GET_DESCRIPTOR 6u
#define USB_DT_DEVICE 1u
#define USB_DT_DEVICE_SIZE 18u
#define USB_ENDPOINT_XFER_ISOC 1u
#define USB_ENDPOINT_XFER_BULK 2u
#define USB_ENDPOINT_XFER_INT 3u
#define USB_ENDPOINT_XFERTYPE_MASK 3u
#define USB_ENDPOINT_NUMBER_MASK 0x0fu
#define USB_TYPE_CLASS 0x20u
#define USB_RECIP_INTERFACE 1u
#define USB_CLASS_HID 3u
#define USB_DEVICE_ID_MATCH_VENDOR 0x0001u
#define USB_DEVICE_ID_MATCH_PRODUCT 0x0002u
#define USB_DEVICE_ID_MATCH_INT_CLASS 0x0080u
#define USB_DEVICE_ID_MATCH_INT_SUBCLASS 0x0100u
#define USB_DEVICE_ID_MATCH_INT_PROTOCOL 0x0200u
#define PIPE_ISOCHRONOUS 0u
#define PIPE_INTERRUPT 1u
#define PIPE_CONTROL 2u
#define PIPE_BULK 3u
#define URB_ISO_ASAP 0x0002u
#define LUME_USB_LINEAR_TRANSFER_MAX 65536u
#define USB_INTERFACE_INFO(cl, sub, proto)                                                         \
    .match_flags = USB_DEVICE_ID_MATCH_INT_CLASS | USB_DEVICE_ID_MATCH_INT_SUBCLASS |              \
                   USB_DEVICE_ID_MATCH_INT_PROTOCOL,                                               \
    .bInterfaceClass = (cl), .bInterfaceSubClass = (sub), .bInterfaceProtocol = (proto)

struct usb_device_id
{
    u16 match_flags, idVendor, idProduct;
    u8 bInterfaceClass, bInterfaceSubClass, bInterfaceProtocol;
    unsigned long driver_info;
};

struct usb_endpoint_descriptor
{
    u8 bEndpointAddress, bmAttributes;
    u16 wMaxPacketSize;
    u8 bInterval;
};

struct usb_host_endpoint
{
    struct usb_endpoint_descriptor desc;
};

struct usb_interface_descriptor
{
    u8 bInterfaceNumber, bAlternateSetting, bNumEndpoints;
    u8 bInterfaceClass, bInterfaceSubClass, bInterfaceProtocol;
};

struct usb_host_interface
{
    struct usb_interface_descriptor desc;
    struct usb_host_endpoint *endpoint;
};

struct urb;
struct usb_anchor;
struct scatterlist;

struct usb_device
{
    u16 vendor, product;
    u8 address, interrupt_in;
    u16 packet;
    bool online;
    void *transport_context;
    int (*control)(void *, u8, u8, u16, u16, void *, u16, unsigned timeout_ms);
    int (*submit)(void *, struct urb *);
    void (*unlink)(void *, struct urb *);
    struct urb *pending;
    struct urb *pending_tail;
};

struct usb_interface
{
    struct usb_host_interface *cur_altsetting;
    struct usb_device *usb_dev;
    void *data;
};

struct usb_driver
{
    const char *name;
    int (*probe)(struct usb_interface *, const struct usb_device_id *);
    void (*disconnect)(struct usb_interface *);
    const struct usb_device_id *id_table;
};

struct usb_iso_packet_descriptor
{
    unsigned offset, length, actual_length;
    int status;
};

struct urb
{
    struct usb_device *dev;
    unsigned pipe;
    void *transfer_buffer, *context;
    int transfer_buffer_length, actual_length, status, interval;
    void (*complete)(struct urb *);
    unsigned references, transfer_flags, start_frame, number_of_packets, error_count;
    struct urb *pending_next, *pending_previous;
    struct usb_anchor *anchor;
    struct urb *anchor_next, *anchor_previous;
    bool cancelling, submitted;
    struct scatterlist *sg;
    int num_sgs;
    void *sg_allocation, *sg_buffer;
    struct usb_iso_packet_descriptor iso_frame_desc[];
};

struct usb_anchor
{
    struct urb *head, *tail;
};

static inline struct usb_device *interface_to_usbdev(struct usb_interface *interface)
{
    return interface->usb_dev;
}

static inline void usb_set_intfdata(struct usb_interface *interface, void *data)
{
    interface->data = data;
}

static inline void *usb_get_intfdata(struct usb_interface *interface)
{
    return interface->data;
}

static inline unsigned usb_rcvintpipe(const struct usb_device *device, unsigned endpoint)
{
    return (PIPE_INTERRUPT << 30) | ((unsigned)device->address << 8) |
           ((endpoint & USB_ENDPOINT_NUMBER_MASK) << 15) | USB_DIR_IN;
}

static inline unsigned usb_sndctrlpipe(const struct usb_device *device, unsigned endpoint)
{
    return (PIPE_CONTROL << 30) | ((unsigned)device->address << 8) |
           ((endpoint & USB_ENDPOINT_NUMBER_MASK) << 15);
}

static inline unsigned usb_rcvctrlpipe(const struct usb_device *device, unsigned endpoint)
{
    return usb_sndctrlpipe(device, endpoint) | USB_DIR_IN;
}

static inline unsigned usb_sndbulkpipe(const struct usb_device *device, unsigned endpoint)
{
    return (PIPE_BULK << 30) | ((unsigned)device->address << 8) |
           ((endpoint & USB_ENDPOINT_NUMBER_MASK) << 15);
}

static inline unsigned usb_rcvbulkpipe(const struct usb_device *device, unsigned endpoint)
{
    return usb_sndbulkpipe(device, endpoint) | USB_DIR_IN;
}

static inline unsigned usb_sndisocpipe(const struct usb_device *device, unsigned endpoint)
{
    return ((unsigned)device->address << 8) |
           ((endpoint & USB_ENDPOINT_NUMBER_MASK) << 15);
}

static inline unsigned usb_rcvisocpipe(const struct usb_device *device, unsigned endpoint)
{
    return usb_sndisocpipe(device, endpoint) | USB_DIR_IN;
}

static inline unsigned usb_pipeendpoint(unsigned pipe)
{
    return (pipe >> 15) & USB_ENDPOINT_NUMBER_MASK;
}

static inline unsigned usb_pipetype(unsigned pipe)
{
    return pipe >> 30;
}

static inline bool usb_pipein(unsigned pipe)
{
    return pipe & USB_DIR_IN;
}

static inline bool usb_pipeout(unsigned pipe)
{
    return !usb_pipein(pipe);
}

static inline void *lume_linux_usb_transfer_buffer(const struct urb *urb)
{
    return urb->sg_buffer ? urb->sg_buffer : urb->transfer_buffer;
}

const struct usb_device_id *usb_match_id(struct usb_interface *, const struct usb_device_id *);
struct urb *usb_alloc_urb(int iso_packets, gfp_t);
struct urb *usb_get_urb(struct urb *);
void usb_free_urb(struct urb *);
static inline void usb_put_urb(struct urb *urb)
{
    usb_free_urb(urb);
}
void init_usb_anchor(struct usb_anchor *);
void usb_anchor_urb(struct urb *, struct usb_anchor *);
void usb_unanchor_urb(struct urb *);
void usb_kill_anchored_urbs(struct usb_anchor *);
bool usb_anchor_empty(const struct usb_anchor *);
void usb_fill_int_urb(struct urb *, struct usb_device *, unsigned, void *, int,
                      void (*)(struct urb *), void *, int);
void usb_fill_bulk_urb(struct urb *, struct usb_device *, unsigned, void *, int,
                       void (*)(struct urb *), void *);
int usb_submit_urb(struct urb *, gfp_t);
void usb_kill_urb(struct urb *);
int usb_control_msg(struct usb_device *, unsigned pipe, u8 request, u8 type, u16 value, u16 index,
                    void *data, u16 size, int timeout);

/* Complete and disconnect in the same serialized context as interrupt submission. */
void lume_linux_usb_finish(struct usb_device *, const void *, unsigned bytes, int status);
void lume_linux_usb_finish_pipe(struct usb_device *, unsigned pipe, const void *, unsigned bytes,
                                int status);
void lume_linux_usb_complete_urb(struct urb *, const void *, unsigned bytes, int status);
#endif
