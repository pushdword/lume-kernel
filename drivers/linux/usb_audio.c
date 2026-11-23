/* SPDX-License-Identifier: GPL-2.0-only */
#include <linux/err.h>
#include <linux/usb.h>
#include <lume/early.h>
#include <lume/kernel_memory.h>

#define AUDIO_URBS 2u
#define AUDIO_PACKETS_PER_URB 8u

struct lume_linux_usb_audio;

struct audio_transfer
{
    struct lume_linux_usb_audio *audio;
    struct urb *urb;
    unsigned char *buffer;
    unsigned completions;
};

struct lume_linux_usb_audio
{
    struct usb_device *device;
    struct usb_anchor anchor;
    struct audio_transfer transfers[AUDIO_URBS];
    unsigned packet;
    bool failed, passed;
};

static void fail_audio(struct lume_linux_usb_audio *audio, unsigned error)
{
    if (audio->failed || audio->passed)
        return;
    audio->failed = true;
    lume_value("USB_LINUX_AUDIO_ERROR", error);
    usb_kill_anchored_urbs(&audio->anchor);
}

static void complete_audio(struct urb *urb)
{
    struct audio_transfer *transfer = urb->context;
    struct lume_linux_usb_audio *audio = transfer->audio;
    if (urb->status || urb->actual_length != (int)audio->packet ||
        urb->iso_frame_desc[0].status ||
        urb->iso_frame_desc[0].actual_length != audio->packet)
    {
        fail_audio(audio, 1);
        return;
    }
    ++transfer->completions;
    if (transfer->completions < AUDIO_PACKETS_PER_URB)
    {
        usb_anchor_urb(urb, &audio->anchor);
        if (usb_submit_urb(urb, GFP_ATOMIC))
            fail_audio(audio, 2);
        return;
    }
    for (unsigned index = 0; index < AUDIO_URBS; ++index)
        if (audio->transfers[index].completions < AUDIO_PACKETS_PER_URB)
            return;
    audio->passed = true;
    lume_puts("LUME USB LINUX ISOCHRONOUS AUDIO PASS\n");
}

struct lume_linux_usb_audio *lume_linux_usb_audio_attach(struct usb_device *device,
                                                         unsigned endpoint, unsigned packet,
                                                         unsigned interval)
{
    if (!device || !endpoint || !packet || packet > 1024 || !interval)
        return NULL;
    struct lume_linux_usb_audio *audio = lume_kernel_allocate(sizeof(*audio));
    unsigned char *buffers = lume_kernel_allocate((size_t)packet * AUDIO_URBS);
    if (!audio || !buffers)
    {
        lume_kernel_free(buffers);
        lume_kernel_free(audio);
        return NULL;
    }
    *audio = (struct lume_linux_usb_audio){.device = device, .packet = packet};
    memset(buffers, 0, (size_t)packet * AUDIO_URBS);
    init_usb_anchor(&audio->anchor);
    for (unsigned index = 0; index < AUDIO_URBS; ++index)
    {
        struct audio_transfer *transfer = &audio->transfers[index];
        transfer->audio = audio;
        transfer->buffer = buffers + (size_t)packet * index;
        transfer->urb = usb_alloc_urb(1, GFP_KERNEL);
        if (!transfer->urb)
            goto failed;
        struct urb *urb = transfer->urb;
        urb->dev = device;
        urb->pipe = usb_sndisocpipe(device, endpoint);
        urb->transfer_buffer = transfer->buffer;
        urb->transfer_buffer_length = (int)packet;
        urb->complete = complete_audio;
        urb->context = transfer;
        urb->interval = (int)interval;
        urb->transfer_flags = URB_ISO_ASAP;
        urb->iso_frame_desc[0] =
            (struct usb_iso_packet_descriptor){.offset = 0, .length = packet};
        usb_anchor_urb(urb, &audio->anchor);
        if (usb_submit_urb(urb, GFP_KERNEL))
            goto failed;
    }
    return audio;
failed:
    usb_kill_anchored_urbs(&audio->anchor);
    for (unsigned index = 0; index < AUDIO_URBS; ++index)
        usb_free_urb(audio->transfers[index].urb);
    lume_kernel_free(buffers);
    lume_kernel_free(audio);
    return NULL;
}

void lume_linux_usb_audio_detach(struct lume_linux_usb_audio *audio)
{
    if (!audio)
        return;
    usb_kill_anchored_urbs(&audio->anchor);
    unsigned char *buffers = audio->transfers[0].buffer;
    for (unsigned index = 0; index < AUDIO_URBS; ++index)
        usb_free_urb(audio->transfers[index].urb);
    lume_kernel_free(buffers);
    lume_kernel_free(audio);
}
