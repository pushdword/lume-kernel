#include "lume/q35.h"
#include "lume/acpi.h"
#include "lume/firmware.h"
#if !defined(LUME_ARCH_I686)
#include "lume/apic.h"
#endif
#include <string.h>
#define EARLY_LIMIT UINT64_C(0x40000000)
static struct lume_acpi acpi;

struct firmware_memory
{
    uint64_t map, bytes;
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
    return address && address < EARLY_LIMIT && bytes <= EARLY_LIMIT - address;
}

static bool firmware_read(void *context, uint64_t address, void *out, size_t bytes)
{
    const struct firmware_memory *memory = context;
    bool permitted = lume_q35_firmware_range((const void *)(uintptr_t)memory->map,
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

bool lume_q35_boot_read(uint64_t info, uint64_t magic, struct lume_fdt *memory)
{
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
        if (length > UINT64_MAX - base)
            return false;
        if (type == 1 && length && base < UINT64_C(0x200000000))
        {
            if (memory->ram_count == 8)
                return false;
            if (length > UINT64_C(0x200000000) - base)
                length = UINT64_C(0x200000000) - base;
            memory->ram[memory->ram_count++] = (struct lume_range){base, length};
        }
        offset += size;
    }
    struct firmware_memory firmware = {map, bytes};
    uint64_t ebda = *(const volatile uint16_t *)(uintptr_t)0x40e;
    (void)lume_acpi_find(&acpi, firmware_read, &firmware, ebda << 4);
    extern unsigned char __image_start[], __image_end[];
    for (unsigned i = 0; i < count; ++i)
    {
        uint64_t start = word(modules + i * 16), end = word(modules + i * 16 + 4);
        if (end <= start || end > EARLY_LIMIT || !contains_ram(memory, start, end - start) ||
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
