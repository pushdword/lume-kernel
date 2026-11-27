/* SPDX-License-Identifier: GPL-2.0-only */
#include <linux/err.h>
#include <linux/scatterlist.h>
#include <linux/usb.h>
#include <lume/kernel_memory.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static unsigned allocations, callbacks, controls;
static bool fail_allocation;

struct transport
{
    unsigned submissions, unlinks;
    bool fail, complete_synchronously;
    bool retain_cancelled_buffer;
    void *cancelled_buffer;
};

struct completion_record
{
    unsigned id;
    int status, actual;
    const char *data;
};

static unsigned completion_sequence[8], completion_count;
static unsigned anchor_completions, anchor_cancellations;

struct anchor_record
{
    struct usb_anchor *anchor;
    bool reanchor;
};

void *lume_kernel_allocate(size_t bytes)
{
    if (fail_allocation)
        return NULL;
    void *memory = malloc(bytes);
    if (memory)
        ++allocations;
    return memory;
}

void lume_kernel_free(void *memory)
{
    assert(allocations-- && memory);
    free(memory);
}

static int control(void *context, u8 type, u8 request, u16 value, u16 index, void *data, u16 bytes,
                   unsigned timeout)
{
    assert(!context && timeout == 500);
    ++controls;
    if (request == 11)
    {
        assert(type == 0x21 && !value && index == 2 && !data && !bytes);
        return 0;
    }
    if (request == USB_REQ_GET_DESCRIPTOR)
    {
        assert(type == USB_DIR_IN && value == USB_DT_DEVICE << 8 && !index && bytes == 8);
        memcpy(data, "desc", 4);
        return 4;
    }
    assert(request == 9 && type == 0x21 && value == 0x200 && index == 2 && bytes == 1);
    assert(*(const unsigned char *)data == 0x02);
    return bytes;
}

static void complete(struct urb *urb)
{
    ++callbacks;
    if (urb->status == -ENOENT)
    {
        assert(!urb->actual_length && usb_submit_urb(urb, GFP_ATOMIC) == -EPERM);
        return;
    }
    assert(!urb->status && urb->actual_length == 3 && !memcmp(urb->transfer_buffer, "abc", 3));
    if (callbacks == 1)
        assert(!usb_submit_urb(urb, GFP_ATOMIC));
    else
        usb_free_urb(urb);
}

static int submit(void *context, struct urb *urb)
{
    struct transport *transport = context;
    assert(urb->submitted);
    ++transport->submissions;
    if (transport->complete_synchronously)
        lume_linux_usb_complete_urb(urb, "sync", 4, 0);
    return transport->fail ? -EIO : 0;
}

static void unlink(void *context, struct urb *urb)
{
    struct transport *transport = context;
    assert(urb->submitted && urb->cancelling);
    ++transport->unlinks;
    if (transport->retain_cancelled_buffer)
    {
        assert(urb->sg_allocation && !transport->cancelled_buffer);
        transport->cancelled_buffer = urb->sg_allocation;
        urb->sg_allocation = NULL;
        urb->sg_buffer = NULL;
    }
}

struct sg_completion
{
    unsigned count;
    int status, bytes;
};

static void complete_sg(struct urb *urb)
{
    struct sg_completion *completion = urb->context;
    assert(!urb->submitted && !urb->sg_buffer && !urb->sg_allocation && !urb->transfer_buffer);
    assert(urb->status == completion->status && urb->actual_length == completion->bytes);
    ++completion->count;
}

static void scatter_gather_urbs(void)
{
    struct transport transport = {0};
    struct usb_device device = {.address = 3,
                                .online = true,
                                .transport_context = &transport,
                                .submit = submit,
                                .unlink = unlink};
    struct sg_completion completion = {.bytes = 5};
    struct urb *urb = usb_alloc_urb(0, GFP_KERNEL);
    assert(urb);
    unsigned baseline = allocations;
    char first[] = "abc", second[] = "defg", third[] = "hijk";
    struct scatterlist head[2], tail[2];
    sg_init_table(head, 2);
    sg_set_buf(head, first, sizeof(first) - 1);
    sg_init_table(tail, 2);
    sg_set_buf(tail, second, sizeof(second) - 1);
    sg_set_buf(&tail[1], third, sizeof(third) - 1);
    sg_chain(head, 2, tail);
    urb->sg = head;
    urb->num_sgs = 3;
    usb_fill_bulk_urb(urb, &device, usb_rcvbulkpipe(&device, 1), NULL, 9, complete_sg, &completion);
    urb->num_sgs = 2;
    assert(usb_submit_urb(urb, GFP_KERNEL) == -EINVAL && !transport.submissions);
    urb->num_sgs = 3;
    fail_allocation = true;
    assert(usb_submit_urb(urb, GFP_KERNEL) == -ENOMEM && !transport.submissions && !device.pending);
    fail_allocation = false;
    transport.fail = true;
    assert(usb_submit_urb(urb, GFP_KERNEL) == -EIO && !urb->submitted && !device.pending &&
           allocations == baseline);
    transport.fail = false;
    assert(!usb_submit_urb(urb, GFP_KERNEL) && allocations == baseline + 1);
    void *linear = lume_linux_usb_transfer_buffer(urb);
    assert(linear && !((uintptr_t)linear & (SMP_CACHE_BYTES - 1u)));
    memcpy(linear, "12345", 5);
    lume_linux_usb_complete_urb(urb, linear, 5, 0);
    assert(!memcmp(first, "123", 4) && !memcmp(second, "45fg", 5) && !memcmp(third, "hijk", 5) &&
           completion.count == 1 && allocations == baseline);

    urb->pipe = usb_sndbulkpipe(&device, 2);
    completion.bytes = 9;
    assert(!usb_submit_urb(urb, GFP_KERNEL));
    assert(!memcmp(lume_linux_usb_transfer_buffer(urb), "12345fghi", 9));
    lume_linux_usb_complete_urb(urb, NULL, 9, 0);
    assert(completion.count == 2 && allocations == baseline);

    urb->pipe = usb_rcvbulkpipe(&device, 1);
    completion.status = -ENOENT;
    completion.bytes = 0;
    transport.retain_cancelled_buffer = true;
    assert(!usb_submit_urb(urb, GFP_KERNEL));
    usb_kill_urb(urb);
    assert(!device.pending && completion.count == 3 && allocations == baseline + 1);
    lume_kernel_free(transport.cancelled_buffer);
    transport.cancelled_buffer = NULL;
    transport.retain_cancelled_buffer = false;

    char *maximum = malloc(LUME_USB_LINEAR_TRANSFER_MAX);
    assert(maximum);
    struct scatterlist single;
    sg_init_one(&single, maximum, LUME_USB_LINEAR_TRANSFER_MAX);
    urb->sg = &single;
    urb->num_sgs = 1;
    urb->transfer_buffer_length = LUME_USB_LINEAR_TRANSFER_MAX;
    assert(!usb_submit_urb(urb, GFP_KERNEL));
    uintptr_t buffer = (uintptr_t)lume_linux_usb_transfer_buffer(urb);
    assert(buffer && !(buffer & (LUME_USB_LINEAR_TRANSFER_MAX - 1u)));
    usb_kill_urb(urb);
    assert(completion.count == 4 && allocations == baseline);
    free(maximum);
    usb_free_urb(urb);
}

static void record_completion(struct urb *urb)
{
    const struct completion_record *record = urb->context;
    assert(completion_count < sizeof(completion_sequence) / sizeof(completion_sequence[0]));
    completion_sequence[completion_count++] = record->id;
    assert(urb->status == record->status && urb->actual_length == record->actual);
    if (record->data)
        assert(!memcmp(urb->transfer_buffer, record->data, (size_t)record->actual));
}

static void record_and_free_completion(struct urb *urb)
{
    record_completion(urb);
    usb_free_urb(urb);
}

static void iso_completion(struct urb *urb)
{
    assert(!urb->status && urb->actual_length == 6 && !urb->error_count);
    assert(urb->number_of_packets == 2);
    assert(urb->iso_frame_desc[0].actual_length == 3 &&
           !urb->iso_frame_desc[0].status);
    assert(urb->iso_frame_desc[1].actual_length == 3 &&
           !urb->iso_frame_desc[1].status);
    const unsigned char *buffer = urb->transfer_buffer;
    assert(!memcmp(buffer + 2, "abc", 3) && !memcmp(buffer + 8, "def", 3));
}

static void iso_error_completion(struct urb *urb)
{
    assert(urb->status == -EIO && !urb->actual_length && urb->error_count == 2);
    for (unsigned index = 0; index < urb->number_of_packets; ++index)
        assert(urb->iso_frame_desc[index].status == -EIO &&
               !urb->iso_frame_desc[index].actual_length);
}

static void anchor_completion(struct urb *urb)
{
    struct anchor_record *record = urb->context;
    ++anchor_completions;
    if (!urb->status)
    {
        assert(urb->actual_length == 4 && !memcmp(urb->transfer_buffer, "data", 4));
        return;
    }
    assert(urb->status == -ENOENT && !urb->actual_length);
    ++anchor_cancellations;
    if (record->reanchor)
    {
        usb_anchor_urb(urb, record->anchor);
        assert(urb->anchor == record->anchor && usb_submit_urb(urb, GFP_ATOMIC) == -EPERM);
    }
}

static void bulk_and_concurrent_urbs(void)
{
    struct transport transport = {0};
    struct usb_device device = {.address = 3,
                                .online = true,
                                .transport_context = &transport,
                                .submit = submit,
                                .unlink = unlink};
    unsigned char first_buffer[8] = {0}, second_buffer[8] = {0};
    unsigned char output[] = "send";
    unsigned char cancelled_buffer[8] = {0}, synchronous_buffer[8] = {0};
    unsigned char rejected_buffer[8] = {0};
    const struct completion_record records[] = {
        {.id = 1, .status = 0, .actual = 5, .data = "first"},
        {.id = 2, .status = 0, .actual = 6, .data = "second"},
        {.id = 3, .status = 0, .actual = 4},
        {.id = 4, .status = -ENOENT, .actual = 0},
        {.id = 5, .status = 0, .actual = 4, .data = "sync"},
        {.id = 6, .status = 0, .actual = 0},
    };
    struct urb *first = usb_alloc_urb(0, GFP_KERNEL);
    struct urb *second = usb_alloc_urb(0, GFP_KERNEL);
    struct urb *out = usb_alloc_urb(0, GFP_KERNEL);
    struct urb *cancelled = usb_alloc_urb(0, GFP_KERNEL);
    struct urb *synchronous = usb_alloc_urb(0, GFP_KERNEL);
    struct urb *rejected = usb_alloc_urb(0, GFP_KERNEL);
    assert(first && second && out && cancelled && synchronous && rejected);
    unsigned input_pipe = usb_rcvbulkpipe(&device, 2);
    unsigned output_pipe = usb_sndbulkpipe(&device, 3);
    assert(usb_pipetype(input_pipe) == PIPE_BULK && usb_pipeendpoint(input_pipe) == 2 &&
           usb_pipein(input_pipe));
    assert(usb_pipetype(output_pipe) == PIPE_BULK && usb_pipeendpoint(output_pipe) == 3 &&
           usb_pipeout(output_pipe));
    usb_fill_bulk_urb(first, &device, input_pipe, first_buffer, sizeof(first_buffer),
                      record_completion, (void *)&records[0]);
    usb_fill_bulk_urb(second, &device, input_pipe, second_buffer, sizeof(second_buffer),
                      record_completion, (void *)&records[1]);
    usb_fill_bulk_urb(out, &device, output_pipe, output, sizeof(output) - 1, record_completion,
                      (void *)&records[2]);
    usb_fill_bulk_urb(cancelled, &device, input_pipe, cancelled_buffer,
                      sizeof(cancelled_buffer), record_completion, (void *)&records[3]);
    usb_fill_bulk_urb(synchronous, &device, input_pipe, synchronous_buffer,
                      sizeof(synchronous_buffer), record_and_free_completion,
                      (void *)&records[4]);
    usb_fill_bulk_urb(rejected, &device, input_pipe, rejected_buffer, sizeof(rejected_buffer),
                      record_completion, (void *)&records[5]);
    transport.fail = true;
    assert(usb_submit_urb(rejected, GFP_KERNEL) == -EIO && !rejected->submitted &&
           !device.pending);
    transport.fail = false;
    assert(!usb_submit_urb(first, GFP_KERNEL));
    assert(!usb_submit_urb(second, GFP_KERNEL));
    assert(!usb_submit_urb(out, GFP_KERNEL));
    assert(!usb_submit_urb(cancelled, GFP_KERNEL));
    assert(device.pending == first && device.pending_tail == cancelled);
    assert(usb_submit_urb(first, GFP_ATOMIC) == -EBUSY);
    lume_linux_usb_finish_pipe(&device, input_pipe, "first", 5, 0);
    lume_linux_usb_finish_pipe(&device, input_pipe, "second", 6, 0);
    lume_linux_usb_complete_urb(out, NULL, sizeof(output) - 1, 0);
    usb_kill_urb(cancelled);
    transport.complete_synchronously = true;
    assert(!usb_submit_urb(synchronous, GFP_KERNEL));
    transport.complete_synchronously = false;
    assert(!device.pending && !device.pending_tail && transport.submissions == 6 &&
           transport.unlinks == 1 && completion_count == 5);
    for (unsigned index = 0; index < completion_count; ++index)
        assert(completion_sequence[index] == index + 1);
    usb_free_urb(first);
    usb_free_urb(second);
    usb_free_urb(out);
    usb_free_urb(cancelled);
    usb_free_urb(rejected);
}

static void isochronous_urbs(void)
{
    struct usb_device device = {.address = 4, .online = true};
    unsigned char buffer[12] = {0};
    struct urb *urb = usb_alloc_urb(2, GFP_KERNEL);
    assert(urb);
    urb->dev = &device;
    urb->pipe = usb_rcvisocpipe(&device, 4);
    urb->transfer_buffer = buffer;
    urb->transfer_buffer_length = sizeof(buffer);
    urb->complete = iso_completion;
    urb->interval = 1;
    urb->transfer_flags = URB_ISO_ASAP;
    urb->iso_frame_desc[0] = (struct usb_iso_packet_descriptor){.offset = 2, .length = 3};
    urb->iso_frame_desc[1] = (struct usb_iso_packet_descriptor){.offset = 8, .length = 4};
    assert(usb_pipetype(urb->pipe) == PIPE_ISOCHRONOUS && !usb_submit_urb(urb, GFP_KERNEL));
    lume_linux_usb_finish_pipe(&device, urb->pipe, "abcdef", 6, 0);
    assert(!device.pending && !device.pending_tail);
    usb_free_urb(urb);

    struct urb *failed = usb_alloc_urb(2, GFP_KERNEL);
    assert(failed);
    failed->dev = &device;
    failed->pipe = usb_rcvisocpipe(&device, 4);
    failed->transfer_buffer = buffer;
    failed->transfer_buffer_length = sizeof(buffer);
    failed->complete = iso_error_completion;
    failed->interval = 1;
    failed->iso_frame_desc[0] =
        (struct usb_iso_packet_descriptor){.offset = 0, .length = 4};
    failed->iso_frame_desc[1] =
        (struct usb_iso_packet_descriptor){.offset = 4, .length = 4};
    assert(!usb_submit_urb(failed, GFP_KERNEL));
    lume_linux_usb_complete_urb(failed, NULL, 1, 0);
    usb_free_urb(failed);

    struct urb *invalid = usb_alloc_urb(1, GFP_KERNEL);
    assert(invalid);
    invalid->dev = &device;
    invalid->pipe = usb_rcvisocpipe(&device, 4);
    invalid->transfer_buffer = buffer;
    invalid->transfer_buffer_length = sizeof(buffer);
    invalid->complete = iso_completion;
    invalid->interval = 1;
    invalid->iso_frame_desc[0] =
        (struct usb_iso_packet_descriptor){.offset = sizeof(buffer) - 1, .length = 2};
    assert(usb_submit_urb(invalid, GFP_KERNEL) == -EINVAL);
    usb_free_urb(invalid);
}

static void anchored_urbs(void)
{
    struct usb_device device = {.address = 5, .online = true};
    struct usb_anchor anchor;
    init_usb_anchor(&anchor);
    assert(usb_anchor_empty(&anchor));
    unsigned char first_buffer[8] = {0}, second_buffer[8] = {0};
    struct anchor_record first_record = {.anchor = &anchor};
    struct anchor_record second_record = {.anchor = &anchor, .reanchor = true};
    struct urb *first = usb_alloc_urb(0, GFP_KERNEL);
    struct urb *second = usb_alloc_urb(0, GFP_KERNEL);
    assert(first && second && usb_get_urb(first) == first);
    usb_put_urb(first);
    unsigned pipe = usb_rcvbulkpipe(&device, 2);
    usb_fill_bulk_urb(first, &device, pipe, first_buffer, sizeof(first_buffer), anchor_completion,
                      &first_record);
    usb_fill_bulk_urb(second, &device, pipe, second_buffer, sizeof(second_buffer),
                      anchor_completion, &second_record);
    usb_anchor_urb(first, &anchor);
    usb_anchor_urb(second, &anchor);
    usb_anchor_urb(first, &anchor);
    assert(anchor.head == first && anchor.tail == second && !usb_anchor_empty(&anchor));
    assert(!usb_submit_urb(first, GFP_KERNEL) && !usb_submit_urb(second, GFP_KERNEL));
    lume_linux_usb_finish_pipe(&device, pipe, "data", 4, 0);
    assert(anchor.head == second && anchor.tail == second && first->anchor == NULL);
    usb_kill_anchored_urbs(&anchor);
    assert(usb_anchor_empty(&anchor) && !second->anchor && !device.pending &&
           !device.pending_tail && anchor_completions == 2 && anchor_cancellations == 1);
    usb_free_urb(first);
    usb_free_urb(second);
}

int main(void)
{
    struct usb_device device = {.vendor = 0x1234,
                                .product = 0xabcd,
                                .address = 2,
                                .interrupt_in = 0x81,
                                .packet = 8,
                                .control = control};
    struct usb_host_interface setting = {.desc = {.bInterfaceNumber = 2,
                                                  .bInterfaceClass = USB_CLASS_HID,
                                                  .bInterfaceSubClass = 1,
                                                  .bInterfaceProtocol = 1}};
    struct usb_interface interface = {.cur_altsetting = &setting, .usb_dev = &device};
    static const struct usb_device_id ids[] = {
        {USB_INTERFACE_INFO(USB_CLASS_HID, 1, 2)},
        {USB_INTERFACE_INFO(USB_CLASS_HID, 1, 1)},
        {0},
    };
    assert(usb_match_id(&interface, ids) == &ids[1]);
    assert(!usb_alloc_urb(-1, GFP_KERNEL) && !usb_alloc_urb(0, GFP_ATOMIC));
    struct urb *urb = usb_alloc_urb(0, GFP_KERNEL);
    unsigned char buffer[8] = {0};
    assert(urb);
    usb_fill_int_urb(urb, &device, usb_rcvintpipe(&device, 1), buffer, sizeof(buffer), complete,
                     NULL, 4);
    assert(usb_submit_urb(urb, GFP_KERNEL) == -ENODEV);
    device.online = true;
    assert(!usb_control_msg(&device, usb_sndctrlpipe(&device, 0), 11, 0x21, 0, 2, NULL, 0, 500));
    unsigned char descriptor[8] = {0, 0, 0, 0, 0xff};
    assert(usb_control_msg(&device, usb_rcvctrlpipe(&device, 0), USB_REQ_GET_DESCRIPTOR, USB_DIR_IN,
                           USB_DT_DEVICE << 8, 0, descriptor, sizeof(descriptor), 500) == 4);
    assert(!memcmp(descriptor, "desc", 4) && descriptor[4] == 0xff);
    unsigned char report = 0x02;
    assert(usb_control_msg(&device, usb_sndctrlpipe(&device, 0), 9, 0x21, 0x200, 2, &report, 1,
                           500) == 1);
    assert(usb_control_msg(&device, usb_sndctrlpipe(&device, 0), USB_REQ_GET_DESCRIPTOR, USB_DIR_IN,
                           USB_DT_DEVICE << 8, 0, descriptor, sizeof(descriptor), 500) == -EINVAL);
    assert(usb_control_msg(&device, usb_rcvctrlpipe(&device, 0), USB_REQ_GET_DESCRIPTOR, USB_DIR_IN,
                           USB_DT_DEVICE << 8, 0, NULL, sizeof(descriptor), 500) == -EINVAL);
    assert(controls == 3 && !usb_submit_urb(urb, GFP_KERNEL));
    assert(usb_submit_urb(urb, GFP_KERNEL) == -EBUSY);
    lume_linux_usb_finish(&device, "abc", 3, 0);
    assert(callbacks == 1 && device.pending == urb && buffer[3] == 0);
    usb_kill_urb(urb);
    assert(callbacks == 2 && !device.pending);
    assert(!usb_submit_urb(urb, GFP_KERNEL));
    lume_linux_usb_finish(&device, "abc", 3, 0);
    assert(callbacks == 3 && !device.pending && !allocations);
    bulk_and_concurrent_urbs();
    isochronous_urbs();
    anchored_urbs();
    scatter_gather_urbs();
    assert(!allocations);
    puts("Linux USB bridge PASS: control, bulk, isochronous, scatter-gather and anchored URBs");
    return 0;
}
