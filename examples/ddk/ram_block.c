#include <lume/block.h>
#include <lume/abi.h>

static unsigned char storage[8][512];
static struct lume_device device;
struct lume_block lume_example_ram_block;

static int read_blocks(struct lume_block *b, uint64_t first, unsigned count, void *data)
{
    unsigned char *out = data;
    const unsigned char *in = (const void *)b->context;
    for (size_t i = 0; i < (size_t)count * 512; ++i)
        out[i] = in[first * 512 + i];
    return 0;
}

static int write_blocks(struct lume_block *b, uint64_t first, unsigned count, const void *data)
{
    const unsigned char *in = data;
    unsigned char *out = b->context;
    for (size_t i = 0; i < (size_t)count * 512; ++i)
        out[first * 512 + i] = in[i];
    return 0;
}

static int sync_blocks(struct lume_block *b)
{
    (void)b;
    return 0;
}
static const struct lume_block_ops block_ops = {read_blocks, write_blocks, sync_blocks};

static int start(struct lume_device *d)
{
    lume_example_ram_block = (struct lume_block){.device = d,
                                                 .ops = &block_ops,
                                                 .context = storage,
                                                 .sectors = 8,
                                                 .sector_bytes = 512,
                                                 .max_sectors = 8};
    return 0;
}

static int stop(struct lume_device *d)
{
    (void)d;
    return 0;
}
static const struct lume_driver_ops driver_ops = {.start = start, .stop = stop};
static struct lume_device device = {.version = LUME_DDK_VERSION,
                                    .class_id = LUME_DEVICE_BLOCK,
                                    .name = "ramblk0",
                                    .ops = &driver_ops};
LUME_DRIVER_ENTRY(ram_block_entry, device);

int lume_example_ram_block_start(void)
{
    return lume_device_attach(&device);
}

int lume_example_ram_block_stop(void)
{
    return lume_device_detach(&device);
}
