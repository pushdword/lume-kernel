/* SPDX-License-Identifier: GPL-2.0-only */
#include <linux/err.h>
#include <linux/usb.h>
#include <lume/block.h>
#include <lume/kernel_memory.h>
#include <lume/work.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

struct lume_linux_usb_storage;
struct lume_linux_usb_storage *lume_linux_usb_storage_attach(struct usb_device *, unsigned,
                                                             unsigned);
void lume_linux_usb_storage_detach(struct lume_linux_usb_storage *);

static unsigned allocations, commands, reads, writes, synchronizations;
static struct lume_block *registered;

struct transport
{
    unsigned char media[4096];
    uint32_t tag, lba;
    unsigned bytes, count;
    unsigned char command;
};

void *lume_kernel_allocate(size_t bytes)
{
    void *memory = malloc(bytes);
    if (memory)
        ++allocations;
    return memory;
}

void lume_kernel_free(void *memory)
{
    if (!memory)
        return;
    assert(allocations);
    --allocations;
    free(memory);
}

void lume_puts(const char *text)
{
    (void)text;
}

void lume_value(const char *name, uint64_t value)
{
    fprintf(stderr, "%s=%llu\n", name, (unsigned long long)value);
    assert(false);
}

uint64_t lume_counter_read(void)
{
    return 1;
}

uint64_t lume_counter_frequency(void)
{
    return 1000000;
}

void lume_worker_copy(void *out, const void *in, size_t bytes)
{
    memcpy(out, in, bytes);
}

bool lume_worker_wait_bits(volatile uint32_t *value, uint32_t mask, uint64_t deadline)
{
    assert(deadline > 1 && (__atomic_load_n(value, __ATOMIC_ACQUIRE) & mask));
    return true;
}

int lume_work_submit(struct lume_work *work)
{
    assert(work && work->run && work->domain == LUME_WORK_USB);
    work->run(work->context);
    return 0;
}

bool lume_work_cancel(struct lume_work *work)
{
    (void)work;
    return false;
}

int lume_devfs_register_block(struct lume_block *block)
{
    assert(!registered && block && block->sectors == 8 && block->sector_bytes == 512 &&
           block->max_sectors == 128 && !block->read_only);
    registered = block;
    return 0;
}

int lume_devfs_unregister_block(struct lume_block *block)
{
    assert(registered == block);
    registered = NULL;
    return 0;
}

static uint32_t get32(const unsigned char *in)
{
    return (uint32_t)in[0] | (uint32_t)in[1] << 8 | (uint32_t)in[2] << 16 |
           (uint32_t)in[3] << 24;
}

static uint32_t get_be32(const unsigned char *in)
{
    return (uint32_t)in[0] << 24 | (uint32_t)in[1] << 16 | (uint32_t)in[2] << 8 |
           (uint32_t)in[3];
}

static void put32(unsigned char *out, uint32_t value)
{
    out[0] = (unsigned char)value;
    out[1] = (unsigned char)(value >> 8);
    out[2] = (unsigned char)(value >> 16);
    out[3] = (unsigned char)(value >> 24);
}

static void complete_input(struct transport *transport, struct urb *urb)
{
    unsigned char response[36] = {0};
    const void *data = response;
    unsigned bytes = (unsigned)urb->transfer_buffer_length;
    if (bytes == 13)
    {
        put32(response, UINT32_C(0x53425355));
        put32(response + 4, transport->tag);
    }
    else if (transport->command == 0x12)
        response[4] = 31;
    else if (transport->command == 0x25)
    {
        response[3] = 7;
        response[6] = 2;
    }
    else
    {
        assert(transport->command == 0x28 && bytes == transport->bytes);
        assert(transport->lba < 8 && transport->count <= 8 - transport->lba);
        data = transport->media + transport->lba * 512u;
        ++reads;
    }
    lume_linux_usb_complete_urb(urb, data, bytes, 0);
}

static int submit(void *context, struct urb *urb)
{
    struct transport *transport = context;
    unsigned bytes = (unsigned)urb->transfer_buffer_length;
    if (usb_pipein(urb->pipe))
    {
        complete_input(transport, urb);
        return 0;
    }
    const unsigned char *data = urb->transfer_buffer;
    if (bytes == 31)
    {
        assert(get32(data) == UINT32_C(0x43425355));
        transport->tag = get32(data + 4);
        transport->bytes = get32(data + 8);
        transport->command = data[15];
        transport->lba = get_be32(data + 17);
        transport->count = (unsigned)data[22] << 8 | data[23];
        ++commands;
        if (transport->command == 0x35)
            ++synchronizations;
    }
    else
    {
        assert(transport->command == 0x2a && bytes == transport->bytes);
        assert(transport->lba < 8 && transport->count <= 8 - transport->lba);
        memcpy(transport->media + transport->lba * 512u, data, bytes);
        ++writes;
    }
    lume_linux_usb_complete_urb(urb, NULL, bytes, 0);
    return 0;
}

int main(void)
{
    struct transport transport = {0};
    for (unsigned index = 0; index < sizeof(transport.media); ++index)
        transport.media[index] = (unsigned char)(index * 29u + 7u);
    struct usb_device device = {.address = 2,
                                .online = true,
                                .transport_context = &transport,
                                .submit = submit};
    struct lume_linux_usb_storage *storage = lume_linux_usb_storage_attach(&device, 1, 2);
    assert(storage && registered && commands == 2);
    unsigned char observed[1024], replacement[1024];
    assert(!registered->ops->read(registered, 2, 2, observed));
    assert(!memcmp(observed, transport.media + 1024, sizeof(observed)));
    for (unsigned index = 0; index < sizeof(replacement); ++index)
        replacement[index] = (unsigned char)(index * 11u + 3u);
    assert(!registered->ops->write(registered, 4, 2, replacement));
    assert(!memcmp(replacement, transport.media + 2048, sizeof(replacement)));
    memset(observed, 0, sizeof(observed));
    assert(!registered->ops->read(registered, 4, 2, observed));
    assert(!memcmp(replacement, observed, sizeof(observed)));
    assert(!registered->ops->sync(registered));
    assert(reads == 2 && writes == 1 && synchronizations == 1 && commands == 6);
    lume_linux_usb_storage_detach(storage);
    assert(!registered && !device.pending && !allocations);
    puts("Linux USB storage PASS: capacity, block read, write, sync and release");
    return 0;
}
