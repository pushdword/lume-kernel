/* AHCI command tables use one synchronous slot and a private DMA bounce buffer. */
#include "lume/q35.h"
#include "lume/q35_module.h"
#include "lume/platform_driver.h"
#include "lume/devfs.h"
#include "lume/storage.h"
#include "lume/pc_disk.h"
#include "lume/counter.h"
#include "lume/cpu.h"
#include "lume/sd.h"
#include "lume/worker.h"
#include <string.h>

#define AHCI_PORTS 32u
#define AHCI_DMA_PAGES 5u
#define AHCI_BOUNCE_BYTES ((AHCI_DMA_PAGES - 1u) * 4096u)
#define AHCI_STORAGE_SECTORS (AHCI_BOUNCE_BYTES / 512u)
#define AHCI_ERRORS UINT32_C(0x78000000)

struct ahci_disk
{
    struct lume_pc_disk raw;
    volatile uint32_t *registers;
    uint64_t dma, sectors;
    unsigned sector_bytes;
    char name[5];
    bool initialized, failed, flush;
};

static struct ahci_disk disks[AHCI_PORTS];
static volatile uint32_t *host;
static uint32_t pci_address;
static struct ahci_disk *storage_disk;
static struct lume_volume data_volume;
static struct lume_volume journal_volume;
static bool write_enabled;

static bool wait_clear(volatile uint32_t *value, uint32_t mask)
{
    uint64_t start = lume_counter_read();
    uint64_t timeout = lume_counter_frequency() * 5;
    for (unsigned spins = 0; spins < 100000000; ++spins)
    {
        if (!(*value & mask))
            return true;
        if (lume_counter_read() - start >= timeout)
            break;
        lume_cpu_relax();
    }
    return false;
}

static bool stop_port(struct ahci_disk *disk)
{
    volatile uint32_t *port = disk->registers;
    port[6] &= ~1u;
    if (!wait_clear(port + 6, 1u << 15))
        return false;
    port[6] &= ~(1u << 4);
    return wait_clear(port + 6, 1u << 14);
}

static bool command(struct ahci_disk *disk, unsigned operation, uint64_t sector, unsigned count,
                    void *buffer, bool writing)
{
    bool data = operation != 0xe7;
    unsigned sector_bytes = operation == 0xec ? 512 : disk->sector_bytes;
    if (disk->failed || !disk->initialized ||
        (data &&
         (!buffer || !count || !sector_bytes || count > AHCI_BOUNCE_BYTES / sector_bytes)) ||
        (operation != 0xec && data && (sector >= disk->sectors || count > disk->sectors - sector)))
        return false;
    volatile uint32_t *port = disk->registers;
    if (!wait_clear(port + 8, 0x88) || port[14] || port[13])
    {
        disk->failed = true;
        return false;
    }
    unsigned bytes = data ? count * sector_bytes : 0;
    uint32_t *header = (void *)(uintptr_t)disk->dma;
    unsigned char *table = (void *)(uintptr_t)(disk->dma + 1280);
    unsigned char *bounce = (void *)(uintptr_t)(disk->dma + 4096);
    memset(header, 0, 32);
    memset(table, 0, 144);
    header[0] = 5 | (writing ? 1u << 6 : 0) | (data ? 1u << 16 : 0);
    header[2] = (uint32_t)(disk->dma + 1280);
    header[3] = (uint32_t)((disk->dma + 1280) >> 32);
    table[0] = 0x27;
    table[1] = 0x80;
    table[2] = (unsigned char)operation;
    table[4] = (unsigned char)sector;
    table[5] = (unsigned char)(sector >> 8);
    table[6] = (unsigned char)(sector >> 16);
    table[7] = 0x40;
    table[8] = (unsigned char)(sector >> 24);
    table[9] = (unsigned char)(sector >> 32);
    table[10] = (unsigned char)(sector >> 40);
    if (operation != 0xec)
    {
        table[12] = (unsigned char)count;
        table[13] = (unsigned char)(count >> 8);
    }
    if (data)
    {
        uint32_t *region = (void *)(table + 128);
        region[0] = (uint32_t)(disk->dma + 4096);
        region[1] = (uint32_t)((disk->dma + 4096) >> 32);
        region[3] = bytes - 1;
        if (writing)
            memcpy(bounce, buffer, bytes);
    }
    port[4] = UINT32_MAX;
    port[12] = UINT32_MAX;
    lume_dma_write_complete();
    port[14] = 1;
    lume_memory_complete();
    if (!wait_clear(port + 14, 1) || (port[4] & AHCI_ERRORS) || (port[8] & 0x21))
    {
        disk->failed = true;
        return false;
    }
    lume_dma_read_barrier();
    if (header[1] != bytes)
    {
        disk->failed = true;
        return false;
    }
    if (data && !writing)
        memcpy(buffer, bounce, bytes);
    return true;
}

static int raw_read(struct lume_block *block, uint64_t sector, unsigned count, void *buffer)
{
    struct ahci_disk *disk = block->context;
    if (!command(disk, 0x25, sector, count, buffer, false))
        return -LUME_EIO;
    lume_worker_checkpoint(count * block->sector_bytes);
    return 0;
}

static bool flush(struct ahci_disk *disk)
{
    return !disk->failed && (!disk->flush || command(disk, 0xe7, 0, 0, NULL, false));
}

static int raw_write(struct lume_block *block, uint64_t sector, unsigned count, const void *buffer)
{
    if (!lume_storage_is_installer())
        return -LUME_EROFS;
    struct ahci_disk *disk = block->context;
    if (!command(disk, 0x35, sector, count, (void *)buffer, true))
        return -LUME_EIO;
    lume_worker_checkpoint(count * block->sector_bytes);
    return 0;
}

static int raw_sync(struct lume_block *block)
{
    return flush(block->context) ? 0 : -LUME_EIO;
}

static const struct lume_block_ops raw_operations = {
    .read = raw_read, .write = raw_write, .sync = raw_sync};

static bool read_blocks(uint64_t sector, unsigned count, void *buffer)
{
    if (!storage_disk || !count || sector >= storage_disk->sectors ||
        count > storage_disk->sectors - sector)
        return false;
    unsigned char *target = buffer;
    while (count)
    {
        unsigned chunk = count > AHCI_STORAGE_SECTORS ? AHCI_STORAGE_SECTORS : count;
        if (raw_read(&storage_disk->raw.block, sector, chunk, target))
            return false;
        target += chunk * 512u;
        sector += chunk;
        count -= chunk;
    }
    return true;
}

static bool write_deferred(uint64_t sector, unsigned count, const void *buffer)
{
    if (!write_enabled || (!lume_volume_contains(&data_volume, sector, count) &&
                           !lume_volume_contains(&journal_volume, sector, count)))
        return false;
    const unsigned char *source = buffer;
    while (count)
    {
        unsigned chunk = count > AHCI_STORAGE_SECTORS ? AHCI_STORAGE_SECTORS : count;
        if (!command(storage_disk, 0x35, sector, chunk, (void *)source, true))
            return false;
        lume_worker_checkpoint(chunk * 512u);
        source += chunk * 512u;
        sector += chunk;
        count -= chunk;
    }
    return true;
}

static bool write_blocks(uint64_t sector, unsigned count, const void *buffer)
{
    return write_deferred(sector, count, buffer) && flush(storage_disk);
}

static bool read_sector(uint64_t sector, void *buffer)
{
    return read_blocks(sector, 1, buffer);
}

static bool write_sector(uint64_t sector, const void *buffer)
{
    return write_blocks(sector, 1, buffer);
}

static bool start(uint64_t frequency)
{
    (void)frequency;
    return storage_disk && !storage_disk->failed;
}

static bool configure(uint64_t base, uint64_t size, uint32_t irq)
{
    (void)base;
    (void)size;
    (void)irq;
    return false;
}

static bool stop(void)
{
    return storage_disk && flush(storage_disk);
}

static uint64_t sectors(void)
{
    return storage_disk ? storage_disk->sectors : 0;
}

static bool allow(uint64_t first, uint64_t count)
{
    if (!storage_disk || storage_disk->failed || write_enabled ||
        first != data_volume.first || count != data_volume.count)
        return false;
    write_enabled = true;
    return true;
}

static bool allow_boot(uint64_t first, uint64_t count)
{
    (void)first;
    (void)count;
    return false;
}

static bool allow_journal(uint64_t first, uint64_t count)
{
    unsigned char mbr[512];
    struct lume_volume allowed;
    if (!write_enabled || journal_volume.count || !read_sector(0, mbr) ||
        !lume_volume_journal_select(mbr, sectors(), &allowed) || first != allowed.first ||
        count != allowed.count)
        return false;
    journal_volume = allowed;
    return true;
}

static void stats(void)
{
    lume_value("AHCI_STORAGE_SECTORS", sectors());
}

static const struct lume_platform_sd_ops operations = {
    .lume_sd_start = start,
    .lume_sd_configure = configure,
    .lume_sd_stop = stop,
    .lume_sd_read = read_sector,
    .lume_sd_write = write_sector,
    .lume_sd_sectors = sectors,
    .lume_sd_allow_volume = allow,
    .lume_sd_allow_boot_volume = allow_boot,
    .lume_sd_stats = stats,
    .lume_sd_read_blocks = read_blocks,
    .lume_sd_write_blocks = write_blocks,
    .lume_sd_write_deferred = write_deferred,
    .lume_sd_sync = stop,
    .lume_sd_allow_journal_volume = allow_journal,
};

static bool initialize(struct ahci_disk *disk, unsigned index, bool wide)
{
    disk->registers = host + 64 + index * 32;
    volatile uint32_t *port = disk->registers;
    if ((port[10] & 0xf0f) != 0x103 || port[9] != 0x101 || !stop_port(disk))
        return false;
    disk->dma = lume_pages_alloc_span(AHCI_DMA_PAGES);
    if (!disk->dma)
        return false;
    if (!wide && disk->dma + AHCI_DMA_PAGES * 4096 > UINT64_C(0x100000000))
    {
        for (unsigned page = 0; page < AHCI_DMA_PAGES; ++page)
            (void)lume_page_free(disk->dma + page * 4096);
        disk->dma = 0;
        return false;
    }
    memset((void *)(uintptr_t)disk->dma, 0, AHCI_DMA_PAGES * 4096);
    port[0] = (uint32_t)disk->dma;
    port[1] = (uint32_t)(disk->dma >> 32);
    port[2] = (uint32_t)(disk->dma + 1024);
    port[3] = (uint32_t)((disk->dma + 1024) >> 32);
    port[5] = 0;
    port[4] = UINT32_MAX;
    port[12] = UINT32_MAX;
    lume_dma_write_complete();
    port[6] |= (1u << 4) | 1u;
    disk->initialized = true;
    uint16_t identify[256];
    if (!command(disk, 0xec, 0, 1, identify, false) || (identify[83] & 0xc400) != 0x4400)
        return false;
    disk->sector_bytes = 512;
    if ((identify[106] & 0xd000) == 0x5000)
    {
        uint32_t words = identify[117] | (uint32_t)identify[118] << 16;
        if (words != 512 && words != 1024 && words != 2048)
            return false;
        disk->sector_bytes = words * 2;
    }
    if (disk->sector_bytes != 512)
        return false;
    disk->flush = (identify[83] & (1u << 12)) != 0;
    if (!disk->flush && (identify[82] & (1u << 5)))
        return false;
    disk->sectors = (uint64_t)identify[100] | (uint64_t)identify[101] << 16 |
                    (uint64_t)identify[102] << 32 | (uint64_t)identify[103] << 48;
    if (!disk->sectors || disk->sectors > UINT64_C(0x1000000000000))
        return false;
    disk->name[0] = 's';
    disk->name[1] = 'd';
    unsigned letter = 2;
    if (index >= 26)
        disk->name[letter++] = 'a';
    disk->name[letter++] = (char)('a' + index % 26);
    disk->name[letter] = 0;
    return true;
}

static int select_storage_disk(struct ahci_disk *disk)
{
    unsigned char mbr[512];
    uint32_t identity;
    struct lume_volume volume;
    if (!command(disk, 0x25, 0, 1, mbr, false))
        return 0;
    memcpy(&identity, mbr + 440, sizeof(identity));
    if ((identity != LUME_Q35_EFI_DISK_ID && identity != LUME_INSTALL_DISK_ID) ||
        !lume_volume_select(mbr, disk->sectors, &volume))
        return 0;
    if (storage_disk)
        return -LUME_EBUSY;
    storage_disk = disk;
    data_volume = volume;
    return 0;
}

static int module_start(struct lume_hardware *hardware, void *context)
{
    struct q35_binding *binding = context;
    struct lume_property address;
    int result = lume_hardware_property(hardware, "pci-address", LUME_PROPERTY_U32, 0, &address);
    if (result)
        return result;
    pci_address = (uint32_t)address.values[0];
    uint64_t base = lume_q35_pci_read(pci_address, 36) & ~UINT32_C(15);
    const struct lume_resource *resource;
    unsigned index = 0;
    while ((resource = lume_hardware_resource(hardware, index++)) != NULL)
        if (resource->kind == LUME_RESOURCE_MMIO && resource->base == base &&
            resource->size >= 0x180)
            break;
    if (!resource || !base || base >= UINT64_C(0x100000000) ||
        (lume_q35_pci_read(pci_address, 8) >> 8) != 0x010601)
        return -LUME_EINVAL;
    uint32_t pci_command = lume_q35_pci_read(pci_address, 4) & 65535;
    lume_q35_pci_write(pci_address, 4, pci_command | 6u);
    host = (void *)(uintptr_t)base;
    if (host[9] & 1)
    {
        host[10] |= 2u;
        if (!wait_clear(host + 10, 0x11))
            return -LUME_EIO;
    }
    host[1] = (host[1] | (1u << 31)) & ~2u;
    unsigned ports = AHCI_PORTS;
    if ((resource->size - 0x100) / 0x80 < ports)
        ports = (unsigned)((resource->size - 0x100) / 0x80);
    bool installer = lume_storage_is_installer();
    bool found = false;
    for (unsigned i = 0; i < ports; ++i)
    {
        struct ahci_disk *disk = disks + i;
        if (!(host[3] & (1u << i)) || !initialize(disk, i, (host[0] >> 31) != 0))
            continue;
        found = true;
        if (installer)
            result = lume_pc_disk_register(&disk->raw, disk->name, disk->sectors,
                                           AHCI_STORAGE_SECTORS, &raw_operations, disk, false);
        else
            result = select_storage_disk(disk);
        if (result)
            return result;
        lume_value("AHCI_DISK_PORT", i);
        lume_value("AHCI_DISK_SECTORS", disk->sectors);
    }
    if (installer)
        return found ? 0 : -LUME_ENOENT;
    if (!storage_disk)
        return -LUME_ENOENT;
    result = lume_pc_disk_register(&storage_disk->raw, storage_disk->name, storage_disk->sectors,
                                   AHCI_STORAGE_SECTORS, &raw_operations, storage_disk, true);
    if (result)
        return result;
    result = lume_platform_driver_register(LUME_PLATFORM_SD, &operations, sizeof(operations));
    binding->registered = !result;
    return result;
}

static int module_stop(struct lume_hardware *hardware, void *context)
{
    (void)hardware;
    struct q35_binding *binding = context;
    if (binding->registered)
    {
        int result = lume_platform_driver_can_stop(LUME_PLATFORM_SD, &operations);
        if (result)
            return result;
    }
    for (unsigned i = 0; i < AHCI_PORTS; ++i)
    {
        int result = lume_pc_disk_unregister(&disks[i].raw);
        if (result)
            return result;
    }
    for (unsigned i = 0; i < AHCI_PORTS; ++i)
        if (disks[i].initialized && !stop_port(disks + i))
            return -LUME_EIO;
    if (binding->registered)
    {
        int result = lume_platform_driver_unregister(LUME_PLATFORM_SD, &operations);
        if (result)
            return result;
        binding->registered = false;
    }
    for (unsigned i = 0; i < AHCI_PORTS; ++i)
    {
        if (disks[i].dma)
            for (unsigned page = 0; page < AHCI_DMA_PAGES; ++page)
                (void)lume_page_free(disks[i].dma + page * 4096);
        memset(disks + i, 0, sizeof(disks[i]));
    }
    storage_disk = NULL;
    data_volume = (struct lume_volume){0};
    journal_volume = (struct lume_volume){0};
    write_enabled = false;
    return 0;
}

Q35_MODULE("q35-ahci", LUME_HARDWARE_ID_PCI, "pci-class,010601", 1u << LUME_RESOURCE_MMIO);
