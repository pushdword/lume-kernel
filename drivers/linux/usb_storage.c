/* SPDX-License-Identifier: GPL-2.0-only */
#include <linux/err.h>
#include <linux/usb.h>
#include <lume/block.h>
#include <lume/counter.h>
#include <lume/devfs.h>
#include <lume/early.h>
#include <lume/kernel_memory.h>
#include <lume/worker.h>

#define STORAGE_BYTES (64u * 1024u)
#define STORAGE_SECTORS (STORAGE_BYTES / 512u)
#define STORAGE_SLOTS 4u

enum storage_operation
{
    STORAGE_INQUIRY = 1,
    STORAGE_CAPACITY,
    STORAGE_READ,
    STORAGE_WRITE,
    STORAGE_SYNC
};

struct lume_linux_usb_storage
{
    struct usb_device *device;
    struct usb_anchor anchor;
    struct urb *command, *data, *status;
    struct lume_device block_device;
    struct lume_block block;
    struct lume_work registration;
    unsigned char command_bytes[31], inquiry_bytes[36], capacity_bytes[8], status_bytes[13];
    unsigned char *transfer;
    uint64_t sectors;
    unsigned input_endpoint, output_endpoint, operation, transfer_bytes, tag, slot;
    volatile uint32_t completed, busy;
    int result;
    bool input, active, registered, disconnected, failed;
};

static bool slots[STORAGE_SLOTS];
static const char *const names[STORAGE_SLOTS] = {
    "usb-storage0", "usb-storage1", "usb-storage2", "usb-storage3"};

static void put32(unsigned char *out, u32 value)
{
    out[0] = (u8)value;
    out[1] = (u8)(value >> 8);
    out[2] = (u8)(value >> 16);
    out[3] = (u8)(value >> 24);
}

static u32 get32(const unsigned char *in)
{
    return (u32)in[0] | (u32)in[1] << 8 | (u32)in[2] << 16 | (u32)in[3] << 24;
}

static void put_be16(unsigned char *out, u16 value)
{
    out[0] = (u8)(value >> 8);
    out[1] = (u8)value;
}

static void put_be32(unsigned char *out, u32 value)
{
    out[0] = (u8)(value >> 24);
    out[1] = (u8)(value >> 16);
    out[2] = (u8)(value >> 8);
    out[3] = (u8)value;
}

static u32 get_be32(const unsigned char *in)
{
    return (u32)in[0] << 24 | (u32)in[1] << 16 | (u32)in[2] << 8 | (u32)in[3];
}

static void release_storage(struct lume_linux_usb_storage *storage)
{
    slots[storage->slot] = false;
    usb_free_urb(storage->command);
    usb_free_urb(storage->data);
    usb_free_urb(storage->status);
    lume_kernel_free(storage->transfer);
    lume_kernel_free(storage);
}

static void fail_probe(struct lume_linux_usb_storage *storage, unsigned error)
{
    if (storage->failed)
        return;
    storage->failed = true;
    storage->active = false;
    __atomic_store_n(&storage->busy, 0, __ATOMIC_RELEASE);
    lume_value("USB_LINUX_BULK_ERROR", error);
    usb_kill_anchored_urbs(&storage->anchor);
}

static int start_transaction(struct lume_linux_usb_storage *storage, unsigned operation,
                             const unsigned char *cdb, unsigned cdb_bytes, void *buffer,
                             unsigned bytes, bool input);

static int raw_read(struct lume_block *, uint64_t, unsigned, void *);
static int raw_write(struct lume_block *, uint64_t, unsigned, const void *);
static int raw_sync(struct lume_block *);

static const struct lume_block_ops block_operations = {
    .read = raw_read, .write = raw_write, .sync = raw_sync};

static void register_block(void *context)
{
    struct lume_linux_usb_storage *storage = context;
    if (storage->disconnected || storage->failed)
        return;
    storage->block_device = (struct lume_device){.version = LUME_DDK_VERSION,
                                                 .class_id = LUME_DEVICE_BLOCK,
                                                 .name = names[storage->slot],
                                                 .state = LUME_DEVICE_READY};
    storage->block = (struct lume_block){.device = &storage->block_device,
                                         .ops = &block_operations,
                                         .context = storage,
                                         .sectors = storage->sectors,
                                         .sector_bytes = 512,
                                         .max_sectors = STORAGE_SECTORS};
    int result = lume_devfs_register_block(&storage->block);
    if (result)
    {
        fail_probe(storage, 9);
        return;
    }
    storage->registered = true;
    lume_puts("LUME USB LINUX BLOCK STORAGE READY\n");
}

static void finish_transaction(struct lume_linux_usb_storage *storage, int result)
{
    if (!storage->active)
        return;
    unsigned operation = storage->operation;
    storage->active = false;
    if (result)
    {
        if (operation == STORAGE_INQUIRY || operation == STORAGE_CAPACITY)
            fail_probe(storage, 6);
        else
        {
            storage->result = result;
            __atomic_store_n(&storage->completed, 1, __ATOMIC_RELEASE);
        }
        return;
    }
    if (operation == STORAGE_INQUIRY)
    {
        if ((storage->inquiry_bytes[0] & 0x1f) != 0 || storage->inquiry_bytes[4] < 31)
        {
            fail_probe(storage, 2);
            return;
        }
        lume_puts("LUME USB LINUX BULK STORAGE INQUIRY PASS\n");
        unsigned char cdb[10] = {0x25};
        if (start_transaction(storage, STORAGE_CAPACITY, cdb, sizeof(cdb),
                              storage->capacity_bytes, sizeof(storage->capacity_bytes), true))
            fail_probe(storage, 7);
        return;
    }
    if (operation == STORAGE_CAPACITY)
    {
        uint32_t last = get_be32(storage->capacity_bytes);
        uint32_t bytes = get_be32(storage->capacity_bytes + 4);
        if (last == UINT32_MAX || bytes != 512)
        {
            fail_probe(storage, 8);
            return;
        }
        storage->sectors = (uint64_t)last + 1;
        __atomic_store_n(&storage->busy, 0, __ATOMIC_RELEASE);
        storage->registration = (struct lume_work){.run = register_block,
                                                   .context = storage,
                                                   .domain = LUME_WORK_USB};
        if (lume_work_submit(&storage->registration))
            fail_probe(storage, 10);
        return;
    }
    storage->result = 0;
    __atomic_store_n(&storage->completed, 1, __ATOMIC_RELEASE);
}

static int submit_urb(struct lume_linux_usb_storage *storage, struct urb *urb)
{
    usb_anchor_urb(urb, &storage->anchor);
    int result = usb_submit_urb(urb, GFP_ATOMIC);
    if (result)
        usb_unanchor_urb(urb);
    return result;
}

static void status_complete(struct urb *urb)
{
    struct lume_linux_usb_storage *storage = urb->context;
    int result = urb->status;
    if (!result &&
        (urb->actual_length != (int)sizeof(storage->status_bytes) ||
         get32(storage->status_bytes) != UINT32_C(0x53425355) ||
         get32(storage->status_bytes + 4) != storage->tag ||
         get32(storage->status_bytes + 8) || storage->status_bytes[12]))
        result = -EIO;
    finish_transaction(storage, result);
}

static void data_complete(struct urb *urb)
{
    struct lume_linux_usb_storage *storage = urb->context;
    if (urb->status || urb->actual_length != (int)storage->transfer_bytes)
    {
        finish_transaction(storage, urb->status ? urb->status : -EIO);
        return;
    }
    if (submit_urb(storage, storage->status))
        finish_transaction(storage, -EIO);
}

static void command_complete(struct urb *urb)
{
    struct lume_linux_usb_storage *storage = urb->context;
    if (urb->status || urb->actual_length != (int)sizeof(storage->command_bytes))
    {
        finish_transaction(storage, urb->status ? urb->status : -EIO);
        return;
    }
    struct urb *next = storage->transfer_bytes ? storage->data : storage->status;
    if (submit_urb(storage, next))
        finish_transaction(storage, -EIO);
}

static int start_transaction(struct lume_linux_usb_storage *storage, unsigned operation,
                             const unsigned char *cdb, unsigned cdb_bytes, void *buffer,
                             unsigned bytes, bool input)
{
    if (!storage || storage->active || storage->disconnected || storage->failed || !cdb ||
        !cdb_bytes || cdb_bytes > 16 || bytes > STORAGE_BYTES || (bytes && !buffer))
        return -EINVAL;
    storage->operation = operation;
    storage->transfer_bytes = bytes;
    storage->input = input;
    storage->result = -EIO;
    ++storage->tag;
    if (!storage->tag)
        ++storage->tag;
    memset(storage->command_bytes, 0, sizeof(storage->command_bytes));
    put32(storage->command_bytes, UINT32_C(0x43425355));
    put32(storage->command_bytes + 4, storage->tag);
    put32(storage->command_bytes + 8, bytes);
    storage->command_bytes[12] = input ? 0x80 : 0;
    storage->command_bytes[14] = (u8)cdb_bytes;
    memcpy(storage->command_bytes + 15, cdb, cdb_bytes);
    memset(storage->status_bytes, 0, sizeof(storage->status_bytes));
    usb_fill_bulk_urb(storage->command, storage->device,
                      usb_sndbulkpipe(storage->device, storage->output_endpoint),
                      storage->command_bytes, sizeof(storage->command_bytes), command_complete,
                      storage);
    if (bytes)
        usb_fill_bulk_urb(storage->data, storage->device,
                          input ? usb_rcvbulkpipe(storage->device, storage->input_endpoint)
                                : usb_sndbulkpipe(storage->device, storage->output_endpoint),
                          buffer, (int)bytes, data_complete, storage);
    usb_fill_bulk_urb(storage->status, storage->device,
                      usb_rcvbulkpipe(storage->device, storage->input_endpoint),
                      storage->status_bytes, sizeof(storage->status_bytes), status_complete,
                      storage);
    storage->active = true;
    int result = submit_urb(storage, storage->command);
    if (result)
        storage->active = false;
    return result;
}

static int block_request(struct lume_linux_usb_storage *storage, unsigned operation,
                         uint64_t sector, unsigned count, void *buffer)
{
    if (!storage || storage->disconnected || storage->failed || !storage->registered ||
        (operation != STORAGE_SYNC && (!buffer || !count || count > STORAGE_SECTORS ||
                                       sector > UINT32_MAX || sector >= storage->block.sectors ||
                                       count > storage->block.sectors - sector)))
        return -EIO;
    if (__atomic_exchange_n(&storage->busy, 1, __ATOMIC_ACQUIRE))
        return -EBUSY;
    unsigned char cdb[10] = {0};
    unsigned bytes = count * 512u;
    bool input = operation == STORAGE_READ;
    cdb[0] = operation == STORAGE_READ    ? 0x28
             : operation == STORAGE_WRITE ? 0x2a
                                          : 0x35;
    if (operation != STORAGE_SYNC)
    {
        put_be32(cdb + 2, (u32)sector);
        put_be16(cdb + 7, (u16)count);
        if (operation == STORAGE_WRITE)
            lume_worker_copy(storage->transfer, buffer, bytes);
    }
    __atomic_store_n(&storage->completed, 0, __ATOMIC_RELEASE);
    int result = start_transaction(storage, operation, cdb, sizeof(cdb), storage->transfer, bytes,
                                   input);
    if (!result)
    {
        uint64_t deadline = lume_counter_read() + lume_counter_frequency() * 5;
        if (!lume_worker_wait_bits(&storage->completed, 1, deadline) ||
            !__atomic_load_n(&storage->completed, __ATOMIC_ACQUIRE))
        {
            storage->failed = true;
            storage->active = false;
            usb_kill_anchored_urbs(&storage->anchor);
            result = -ETIMEDOUT;
        }
        else
            result = storage->result;
    }
    if (!result && operation == STORAGE_READ)
        lume_worker_copy(buffer, storage->transfer, bytes);
    __atomic_store_n(&storage->busy, 0, __ATOMIC_RELEASE);
    return result;
}

static int raw_read(struct lume_block *block, uint64_t sector, unsigned count, void *buffer)
{
    return block_request(block->context, STORAGE_READ, sector, count, buffer);
}

static int raw_write(struct lume_block *block, uint64_t sector, unsigned count, const void *buffer)
{
    return block_request(block->context, STORAGE_WRITE, sector, count, (void *)buffer);
}

static int raw_sync(struct lume_block *block)
{
    return block_request(block->context, STORAGE_SYNC, 0, 0, NULL);
}

struct lume_linux_usb_storage *lume_linux_usb_storage_attach(struct usb_device *device,
                                                             unsigned input_endpoint,
                                                             unsigned output_endpoint)
{
    if (!device || !input_endpoint || !output_endpoint)
        return NULL;
    unsigned slot = 0;
    while (slot < STORAGE_SLOTS && slots[slot])
        ++slot;
    if (slot == STORAGE_SLOTS)
        return NULL;
    slots[slot] = true;
    struct lume_linux_usb_storage *storage = lume_kernel_allocate(sizeof(*storage));
    unsigned char *transfer = lume_kernel_allocate(STORAGE_BYTES);
    if (!storage || !transfer)
    {
        lume_kernel_free(transfer);
        lume_kernel_free(storage);
        slots[slot] = false;
        return NULL;
    }
    *storage = (struct lume_linux_usb_storage){.device = device,
                                               .transfer = transfer,
                                               .input_endpoint = input_endpoint,
                                               .output_endpoint = output_endpoint,
                                               .slot = slot,
                                               .busy = 1};
    init_usb_anchor(&storage->anchor);
    storage->command = usb_alloc_urb(0, GFP_KERNEL);
    storage->data = usb_alloc_urb(0, GFP_KERNEL);
    storage->status = usb_alloc_urb(0, GFP_KERNEL);
    if (!storage->command || !storage->data || !storage->status)
        goto failed;
    unsigned char cdb[6] = {0x12, 0, 0, 0, sizeof(storage->inquiry_bytes), 0};
    if (!start_transaction(storage, STORAGE_INQUIRY, cdb, sizeof(cdb), storage->inquiry_bytes,
                           sizeof(storage->inquiry_bytes), true))
        return storage;
failed:
    usb_kill_anchored_urbs(&storage->anchor);
    release_storage(storage);
    return NULL;
}

void lume_linux_usb_storage_detach(struct lume_linux_usb_storage *storage)
{
    if (!storage)
        return;
    storage->disconnected = true;
    storage->active = false;
    usb_kill_anchored_urbs(&storage->anchor);
    (void)lume_work_cancel(&storage->registration);
    if (storage->registered)
    {
        int result = lume_devfs_unregister_block(&storage->block);
        if (result)
        {
            storage->failed = true;
            storage->block_device.state = LUME_DEVICE_FAILED;
            lume_value("USB_LINUX_BLOCK_DETACH_ERROR", (unsigned)-result);
            return;
        }
        storage->registered = false;
    }
    release_storage(storage);
}
