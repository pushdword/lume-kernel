#include "lume/q35.h"
#include "lume/acpi.h"
#include "lume/firmware.h"
#if !defined(LUME_ARCH_I686)
#include "lume/apic.h"
#endif
#include <string.h>
static struct lume_acpi acpi;

struct firmware_memory
{
    uint64_t map, bytes;
    uint64_t rsdp, rsdp_bytes;
};

const struct lume_acpi *lume_q35_acpi(void)
{
    return acpi.rsdp ? &acpi : NULL;
}

static uint32_t word(uint64_t address)
{
    const unsigned char *p = (const void *)(uintptr_t)address;
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t wide(uint64_t address)
{
    return word(address) | (uint64_t)word(address + 4) << 32;
}

static bool early(uint64_t address, uint64_t bytes)
{
    return address && address < LUME_Q35_EARLY_LIMIT && bytes <= LUME_Q35_EARLY_LIMIT - address;
}

static bool firmware_read(void *context, uint64_t address, void *out, size_t bytes)
{
    const struct firmware_memory *memory = context;
    bool permitted = memory->rsdp && address >= memory->rsdp &&
                     address - memory->rsdp <= memory->rsdp_bytes &&
                     bytes <= memory->rsdp_bytes - (address - memory->rsdp);
    if (!permitted)
        permitted = lume_q35_firmware_range((const void *)(uintptr_t)memory->map,
                                            (size_t)memory->bytes, address, bytes);
    if (permitted)
        memcpy(out, (const void *)(uintptr_t)address, bytes);
    return permitted;
}

static bool contains_ram(const struct lume_fdt *memory, uint64_t address, uint64_t bytes)
{
    for (unsigned i = 0; i < memory->ram_count; ++i)
        if (address >= memory->ram[i].base &&
            address - memory->ram[i].base <= memory->ram[i].size &&
            bytes <= memory->ram[i].size - (address - memory->ram[i].base))
            return true;
    return false;
}

static bool add_ram(struct lume_fdt *memory, uint64_t base, uint64_t length, uint32_t type)
{
    const uint64_t limit = UINT64_C(0x200000000);
    if (length > UINT64_MAX - base)
        return false;
    if (type != 1 || !length || base >= limit)
        return true;
    if (memory->ram_count == sizeof(memory->ram) / sizeof(memory->ram[0]))
        return false;
    if (length > limit - base)
        length = limit - base;
    memory->ram[memory->ram_count++] = (struct lume_range){base, length};
    return true;
}

#if !defined(LUME_ARCH_I686)
struct e820_record
{
    uint32_t length, base_low, base_high, size_low, size_high, type;
};

static struct lume_pc_framebuffer boot_framebuffer;

const struct lume_pc_framebuffer *lume_q35_boot_framebuffer(void)
{
    return boot_framebuffer.base ? &boot_framebuffer : NULL;
}

static void framebuffer_tag(uint64_t tag, uint32_t size)
{
    const unsigned char *p = (const void *)(uintptr_t)tag;
    if (size < 38 || p[28] != 32 || p[29] != 1 || p[33] != 8 || p[34] != 8 || p[35] != 8 ||
        p[37] != 8 || !((p[32] == 0 && p[36] == 16) || (p[32] == 16 && p[36] == 0)))
        return;
    uint64_t base = wide(tag + 8);
    uint32_t pitch = word(tag + 16), width = word(tag + 20), height = word(tag + 24);
    if (!base || (base & 3) || (pitch & 3) || !width || width > UINT16_MAX || !height ||
        height > UINT16_MAX || pitch / 4 < width || (uint64_t)pitch * height > UINT32_MAX)
        return;
    boot_framebuffer = (struct lume_pc_framebuffer){base, pitch, width, height, p[32] == 0};
}

static uint64_t ram_root_destination(const struct lume_fdt *memory, uint64_t source, uint64_t bytes)
{
    extern unsigned char __image_end[];
    const uint64_t alignment = UINT64_C(0x200000);
    for (unsigned i = 0; i < memory->ram_count; ++i)
    {
        const struct lume_range *range = memory->ram + i;
        uint64_t base = range->base;
        uint64_t limit = (base + range->size) & ~(alignment - 1);
        if (base < (uintptr_t)__image_end)
            base = (uintptr_t)__image_end;
        if (limit > LUME_USER_BASE)
            limit = LUME_USER_BASE;
        base = (base + alignment - 1) & ~(alignment - 1);
        if (source >= base && source < limit && bytes <= limit - source)
            return source;
        while (base < limit && bytes <= limit - base)
        {
            uint64_t next = base;
            for (unsigned j = 0; j < memory->reserved_count; ++j)
            {
                const struct lume_range *reserved = memory->reserved + j;
                uint64_t end = reserved->base + reserved->size;
                if (base < end && reserved->base < base + bytes && end > next)
                    next = end;
            }
            if (next == base)
                return base;
            base = (next + alignment - 1) & ~(alignment - 1);
        }
    }
    return 0;
}

static bool multiboot2_read(uint64_t info, struct lume_fdt *memory)
{
    if ((info & 7) || !early(info, 8))
        return false;
    uint32_t total = word(info);
    if (total < 16 || total > 65536 || (total & 7) || !early(info, total))
        return false;

    *memory = (struct lume_fdt){.cpu_count = 1};
    boot_framebuffer = (struct lume_pc_framebuffer){0};
    memcpy(memory->model, "PCI PC AMD64", sizeof("PCI PC AMD64"));
    memory->reserved[memory->reserved_count++] = (struct lume_range){info, total};
    unsigned records = 0, acpi_tag = 0;
    uint64_t rsdp = 0, rsdp_bytes = 0;
    uint64_t start = 0, root_end = 0;
    struct e820_record map[256];
    bool end = false;
    for (uint64_t at = info + 8; at < info + total;)
    {
        uint32_t type = word(at), size = word(at + 4);
        if (size < 8 || size > info + total - at)
            return false;
        if (type == 0)
        {
            end = size == 8 && at + size == info + total;
            break;
        }
        if (type == 3)
        {
            if (size < 17 || start)
                return false;
            start = word(at + 8);
            root_end = word(at + 12);
            if (!start || root_end <= start)
                return false;
        }
        if (type == 6)
        {
            if (records || size < 16 || word(at + 12) != 0)
                return false;
            uint32_t stride = word(at + 8);
            if (stride < 24 || (size - 16) % stride)
                return false;
            records = (size - 16) / stride;
            if (!records || records > sizeof(map) / sizeof(map[0]))
                return false;
            for (unsigned i = 0; i < records; ++i)
            {
                uint64_t entry = at + 16 + (uint64_t)i * stride;
                uint64_t base = wide(entry), length = wide(entry + 8);
                uint32_t kind = word(entry + 16);
                if (!add_ram(memory, base, length, kind))
                    return false;
                map[i] = (struct e820_record){20,
                                              (uint32_t)base,
                                              (uint32_t)(base >> 32),
                                              (uint32_t)length,
                                              (uint32_t)(length >> 32),
                                              kind};
            }
        }
        if ((type == 14 || type == 15) && type > acpi_tag)
        {
            if (size < (type == 15 ? 44u : 28u))
                return false;
            acpi_tag = type;
            rsdp = at + 8;
            rsdp_bytes = size - 8;
        }
        if (type == 8)
            framebuffer_tag(at, size);
        if (type == 18)
            return false;
        at += ((uint64_t)size + 7) & ~UINT64_C(7);
    }
    extern unsigned char __image_start[], __image_end[];
    if (!end || !records || !start || !contains_ram(memory, start, root_end - start) ||
        (start < info + total && info < root_end) ||
        (start < (uintptr_t)__image_end && (uintptr_t)__image_start < root_end) ||
        !contains_ram(memory, (uintptr_t)__image_start, __image_end - __image_start))
        return false;

    struct firmware_memory firmware = {(uintptr_t)map, records * sizeof(map[0]), rsdp, rsdp_bytes};
    if (rsdp && !lume_acpi_read(&acpi, firmware_read, &firmware, rsdp))
        return false;

    uint64_t bytes = root_end - start;
    uint64_t destination = ram_root_destination(memory, start, bytes);
    if (!destination)
        return false;
    /* Keep the root below the user address window when GRUB places it in high RAM. */
    if (destination != start)
        memmove((void *)(uintptr_t)destination, (const void *)(uintptr_t)start, bytes);
    memory->initrd_start = destination;
    memory->initrd_end = destination + bytes;
    memory->reserved[memory->reserved_count++] = (struct lume_range){destination, bytes};

    if (acpi.cpus > 1)
    {
        if (contains_ram(memory, LUME_AP_TRAMPOLINE, 4096) &&
            (LUME_AP_TRAMPOLINE >= info + total || LUME_AP_TRAMPOLINE + 4096 <= info))
            memory->reserved[memory->reserved_count++] =
                (struct lume_range){LUME_AP_TRAMPOLINE, 4096};
        else
            acpi.cpus = 1;
    }
    return true;
}
#endif

bool lume_q35_boot_read(uint64_t info, uint64_t magic, struct lume_fdt *memory)
{
#if !defined(LUME_ARCH_I686)
    if (magic == LUME_MULTIBOOT2_MAGIC)
        return multiboot2_read(info, memory);
#endif
    if (magic != 0x2badb002 || !early(info, 88))
        return false;
    uint32_t flags = word(info);
    if ((flags & 0x48) != 0x48)
        return false;
    uint64_t map = word(info + 48), bytes = word(info + 44);
    uint64_t modules = word(info + 24), count = word(info + 20);
    if (!bytes || bytes > 65536 || !early(map, bytes) || !count || count > 16 ||
        !early(modules, count * 16))
        return false;
    *memory = (struct lume_fdt){.cpu_count = 1};
#if defined(LUME_ARCH_I686)
    memcpy(memory->model, "PCI PC i686", 12);
#else
    memcpy(memory->model, "QEMU q35 AMD64", 15);
#endif
    memory->reserved[memory->reserved_count++] = (struct lume_range){info, 88};
    memory->reserved[memory->reserved_count++] = (struct lume_range){map, bytes};
    memory->reserved[memory->reserved_count++] = (struct lume_range){modules, count * 16};
    unsigned records = 0;
    for (uint64_t offset = 0; offset < bytes;)
    {
        if (bytes - offset < 4 || ++records > 256)
            return false;
        uint64_t size = (uint64_t)word(map + offset) + 4;
        if (size < 24 || size > bytes - offset)
            return false;
        uint64_t base = wide(map + offset + 4), length = wide(map + offset + 12);
        unsigned type = word(map + offset + 20);
        if (!add_ram(memory, base, length, type))
            return false;
        offset += size;
    }
    struct firmware_memory firmware = {.map = map, .bytes = bytes};
    uint64_t ebda = *(const volatile uint16_t *)(uintptr_t)0x40e;
    (void)lume_acpi_find(&acpi, firmware_read, &firmware, ebda << 4);
    extern unsigned char __image_start[], __image_end[];
    for (unsigned i = 0; i < count; ++i)
    {
        uint64_t start = word(modules + i * 16), end = word(modules + i * 16 + 4);
        if (end <= start || end > LUME_Q35_EARLY_LIMIT ||
            !contains_ram(memory, start, end - start) ||
            (start < (uintptr_t)__image_end && end > (uintptr_t)__image_start))
            return false;
        memory->reserved[memory->reserved_count++] = (struct lume_range){start, end - start};
        if (!i)
        {
            memory->initrd_start = start;
            memory->initrd_end = end;
        }
    }

#if !defined(LUME_ARCH_I686)
    if (acpi.cpus > 1)
    {
        if (!contains_ram(memory, LUME_AP_TRAMPOLINE, 4096))
            return false;
        for (unsigned i = 0; i < memory->reserved_count; ++i)
            if (LUME_AP_TRAMPOLINE < memory->reserved[i].base + memory->reserved[i].size &&
                memory->reserved[i].base < LUME_AP_TRAMPOLINE + 4096)
                return false;
        memory->reserved[memory->reserved_count++] = (struct lume_range){LUME_AP_TRAMPOLINE, 4096};
    }

#endif
    return memory->ram_count &&
           contains_ram(memory, (uintptr_t)__image_start, __image_end - __image_start);
}
