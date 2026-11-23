/* SPDX-License-Identifier: GPL-2.0-only */
#include <linux/err.h>
#include <linux/usb.h>
#include <lume/early.h>
#include <lume/kernel_memory.h>
#include <lume/linux_usb_hid.h>

enum
{
    HID_SET_REPORT = 9,
    HID_REPORT_OUTPUT = 2,
    HID_LED_CAPS_LOCK = 2,
};

struct lume_linux_usb_hid
{
    struct usb_device device;
    struct usb_interface interface;
    struct usb_host_interface setting;
    struct usb_host_endpoint endpoint;
    struct lume_hid *hid;
    uint32_t milliseconds;
    unsigned char report[64];
    bool reported;
    bool caps_sent;
};

static void complete(struct urb *urb)
{
    if (urb->status)
        return;
    struct lume_linux_usb_hid *port = urb->context;
    bool keyboard = port->setting.desc.bInterfaceProtocol == 1;
    if (keyboard)
        lume_hid_report(port->hid, port->report, (unsigned)urb->actual_length, port->milliseconds);
    else
    {
        ++port->hid->reports;
        lume_hid_mouse_report(port->report, (unsigned)urb->actual_length, port->milliseconds);
    }
    if (urb->actual_length && !port->reported)
    {
        port->reported = true;
        lume_puts(keyboard ? "LUME USB LINUX HID KEYBOARD ACTIVE\n"
                           : "LUME USB LINUX HID MOUSE ACTIVE\n");
    }
    (void)usb_submit_urb(urb, GFP_ATOMIC);
}

static int probe(struct usb_interface *interface, const struct usb_device_id *id)
{
    (void)id;
    struct lume_linux_usb_hid *port = usb_get_intfdata(interface);
    struct usb_device *device = interface_to_usbdev(interface);
    unsigned index = interface->cur_altsetting->desc.bInterfaceNumber;
    int result = usb_control_msg(device, usb_sndctrlpipe(device, 0), 11,
                                 USB_TYPE_CLASS | USB_RECIP_INTERFACE, 0, (u16)index, NULL, 0, 500);
    if (result)
        return result;
    struct urb *urb = usb_alloc_urb(0, GFP_KERNEL);
    if (!urb)
        return -ENOMEM;
    usb_fill_int_urb(urb, device, usb_rcvintpipe(device, device->interrupt_in), port->report,
                     device->packet, complete, port, port->endpoint.desc.bInterval);
    result = usb_submit_urb(urb, GFP_KERNEL);
    if (result)
        usb_free_urb(urb);
    else
        usb_set_intfdata(interface, urb);
    return result;
}

static void disconnect(struct usb_interface *interface)
{
    struct urb *urb = usb_get_intfdata(interface);
    usb_kill_urb(urb);
    usb_free_urb(urb);
    usb_set_intfdata(interface, NULL);
}

static const struct usb_device_id ids[] = {
    {USB_INTERFACE_INFO(USB_CLASS_HID, 1, 1)},
    {USB_INTERFACE_INFO(USB_CLASS_HID, 1, 2)},
    {0},
};

static const struct usb_driver driver = {
    .name = "lume-usb-boot-hid", .probe = probe, .disconnect = disconnect, .id_table = ids};

bool lume_linux_usb_device_descriptor(uint8_t address, void *context,
                                      lume_linux_usb_control control, unsigned char *buffer,
                                      uint16_t bytes)
{
    struct usb_device device = {
        .address = address, .online = true, .transport_context = context, .control = control};
    int length = usb_control_msg(&device, usb_rcvctrlpipe(&device, 0), USB_REQ_GET_DESCRIPTOR,
                                 USB_DIR_IN, USB_DT_DEVICE << 8, 0, buffer, bytes, 500);
    return length == USB_DT_DEVICE_SIZE && buffer[0] == USB_DT_DEVICE_SIZE &&
           buffer[1] == USB_DT_DEVICE;
}

struct lume_linux_usb_hid *lume_linux_usb_hid_attach(const struct lume_usb_interface *descriptor,
                                                     uint16_t vendor, uint16_t product,
                                                     uint8_t address, struct lume_hid *hid,
                                                     void *context, lume_linux_usb_control control)
{
    struct lume_linux_usb_hid *port = lume_kernel_allocate(sizeof(*port));
    if (!port)
        return NULL;
    *port = (struct lume_linux_usb_hid){0};
    if (descriptor->packet > sizeof(port->report))
    {
        lume_kernel_free(port);
        return NULL;
    }
    port->hid = hid;
    port->device = (struct usb_device){.vendor = vendor,
                                       .product = product,
                                       .address = address,
                                       .interrupt_in = descriptor->endpoint,
                                       .packet = descriptor->packet,
                                       .online = true,
                                       .transport_context = context,
                                       .control = control};
    port->endpoint.desc = (struct usb_endpoint_descriptor){.bEndpointAddress = descriptor->endpoint,
                                                           .bmAttributes = USB_ENDPOINT_XFER_INT,
                                                           .wMaxPacketSize = descriptor->packet,
                                                           .bInterval = descriptor->interval};
    port->setting.desc =
        (struct usb_interface_descriptor){.bInterfaceNumber = descriptor->interface,
                                          .bNumEndpoints = 1,
                                          .bInterfaceClass = USB_CLASS_HID,
                                          .bInterfaceSubClass = 1,
                                          .bInterfaceProtocol = descriptor->keyboard ? 1 : 2};
    port->setting.endpoint = &port->endpoint;
    port->interface = (struct usb_interface){
        .cur_altsetting = &port->setting, .usb_dev = &port->device, .data = port};
    const struct usb_device_id *match = usb_match_id(&port->interface, driver.id_table);
    if (!match || driver.probe(&port->interface, match))
    {
        lume_kernel_free(port);
        return NULL;
    }
    return port;
}

bool lume_linux_usb_hid_pending(const struct lume_linux_usb_hid *port)
{
    return port && port->device.pending;
}

void lume_linux_usb_hid_sync_caps(struct lume_linux_usb_hid *port)
{
    if (!port || port->caps_sent == port->hid->caps)
        return;
    bool caps = port->hid->caps;
    u8 report = caps ? HID_LED_CAPS_LOCK : 0;
    struct usb_device *device = &port->device;
    u16 index = port->setting.desc.bInterfaceNumber;
    int bytes = usb_control_msg(device, usb_sndctrlpipe(device, 0), HID_SET_REPORT,
                                USB_TYPE_CLASS | USB_RECIP_INTERFACE, HID_REPORT_OUTPUT << 8, index,
                                &report, sizeof(report), 100);
    if (bytes == sizeof(report))
        port->caps_sent = caps;
}

void lume_linux_usb_hid_complete(struct lume_linux_usb_hid *port, const void *data, unsigned bytes,
                                 int status, uint32_t milliseconds)
{
    if (port)
    {
        port->milliseconds = milliseconds;
        lume_linux_usb_finish(&port->device, data, bytes, status);
    }
}

void lume_linux_usb_hid_detach(struct lume_linux_usb_hid *port)
{
    if (port)
    {
        port->device.online = false;
        driver.disconnect(&port->interface);
        lume_kernel_free(port);
    }
}
