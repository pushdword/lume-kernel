#include "lume/q35.h"
#include "lume/resource.h"
#include "lume/pci.h"
#include "lume/abi.h"
#include "lume/irq.h"
#include "lume/ticket_lock.h"

static struct lume_ticket_lock configuration_lock;

static uint32_t configuration_enter(void)
{
    uint32_t ticket = lume_ticket_reserve(&configuration_lock);
    while (!lume_ticket_ready(&configuration_lock, ticket))
        __atomic_signal_fence(__ATOMIC_ACQUIRE);
    return ticket;
}

static bool valid_config_access(uint32_t device, unsigned offset, unsigned width)
{
    return (width == 1 || width == 2 || width == 4) && !(device & ~UINT32_C(0x00ffff00)) &&
           !(offset & (width - 1)) && offset <= 4096 - width;
}

int lume_q35_pci_config_read(uint32_t device, unsigned offset, unsigned width, uint32_t *value)
{
    if (!value || !valid_config_access(device, offset, width))
        return -LUME_EINVAL;
    uint64_t ecam = lume_q35_pci_ecam(device, offset);
    if (ecam)
    {
        uintptr_t address = (uintptr_t)ecam;
        *value = width == 1   ? *(const volatile uint8_t *)address
                 : width == 2 ? *(const volatile uint16_t *)address
                              : *(const volatile uint32_t *)address;
        return 0;
    }
    if (offset >= 256)
        return -LUME_EOPNOTSUPP;
    uint64_t flags = lume_irq_save();
    uint32_t ticket = configuration_enter();
    lume_out32(0xcf8, 0x80000000u | device | (offset & 252));
    uint16_t port = 0xcfc + (uint16_t)(offset & 3);
    *value = width == 1 ? lume_in8(port) : width == 2 ? lume_in16(port) : lume_in32(port);
    lume_ticket_release(&configuration_lock, ticket);
    lume_irq_restore(flags);
    return 0;
}

int lume_q35_pci_config_write(uint32_t device, unsigned offset, unsigned width, uint32_t value)
{
    if (!valid_config_access(device, offset, width))
        return -LUME_EINVAL;
    /* Use the requested width to preserve adjacent status fields with write-one-to-clear bits. */
    uint64_t ecam = lume_q35_pci_ecam(device, offset);
    if (ecam)
    {
        uintptr_t address = (uintptr_t)ecam;
        if (width == 1)
            *(volatile uint8_t *)address = (uint8_t)value;
        else if (width == 2)
            *(volatile uint16_t *)address = (uint16_t)value;
        else
            *(volatile uint32_t *)address = value;
        return 0;
    }
    if (offset >= 256)
        return -LUME_EOPNOTSUPP;
    uint64_t flags = lume_irq_save();
    uint32_t ticket = configuration_enter();
    lume_out32(0xcf8, 0x80000000u | device | (offset & 252));
    uint16_t port = 0xcfc + (uint16_t)(offset & 3);
    if (width == 1)
        lume_out8(port, (uint8_t)value);
    else if (width == 2)
        lume_out16(port, (uint16_t)value);
    else
        lume_out32(port, value);
    lume_ticket_release(&configuration_lock, ticket);
    lume_irq_restore(flags);
    return 0;
}

uint32_t lume_q35_pci_read(uint32_t device, unsigned offset)
{
    uint32_t value;
    return lume_q35_pci_config_read(device, offset, 4, &value) ? UINT32_MAX : value;
}

void lume_q35_pci_write(uint32_t device, unsigned offset, uint32_t value)
{
    (void)lume_q35_pci_config_write(device, offset, 4, value);
}

int lume_q35_pci_find(uint16_t vendor, uint16_t product, uint32_t *device)
{
    for (unsigned bus = 0; bus < 256; ++bus)
        for (unsigned slot = 0; slot < 32; ++slot)
            for (unsigned function = 0; function < 8; ++function)
            {
                uint32_t address = bus << 16 | slot << 11 | function << 8;
                uint32_t identity = lume_q35_pci_read(address, 0);
                if (identity == ((uint32_t)product << 16 | vendor))
                {
                    *device = address;
                    return 0;
                }
                if (!function &&
                    (identity == UINT32_MAX || !(lume_q35_pci_read(address, 12) & 0x00800000)))
                    break;
            }
    return -1;
}

unsigned lume_q35_pci_bars(uint32_t address, struct lume_pci_bar out[6])
{
    unsigned type = (lume_q35_pci_read(address, 12) >> 16) & 127;
    unsigned bars = type == 0 ? 6 : type == 1 ? 2 : 0;
    uint32_t command;
    if (lume_q35_pci_config_read(address, 4, 2, &command))
        return 0;
    unsigned count = 0;
    (void)lume_q35_pci_config_write(address, 4, 2, command & ~3u);
    for (unsigned bar = 0; bar < bars; ++bar)
    {
        unsigned offset = 16 + bar * 4;
        uint32_t value = lume_q35_pci_read(address, offset);
        bool io = value & 1;
        bool wide = !io && (value & 6) == 4 && bar + 1 < bars;
        uint32_t high = wide ? lume_q35_pci_read(address, offset + 4) : 0;
        lume_q35_pci_write(address, offset, UINT32_MAX);
        if (wide)
            lume_q35_pci_write(address, offset + 4, UINT32_MAX);
        uint64_t mask = lume_q35_pci_read(address, offset) & (io ? ~3u : ~15u);
        if (wide)
            mask |= (uint64_t)lume_q35_pci_read(address, offset + 4) << 32;
        lume_q35_pci_write(address, offset, value);
        if (wide)
            lume_q35_pci_write(address, offset + 4, high);
        uint64_t base = (value & (io ? ~3u : ~15u)) | (uint64_t)high << 32;
        uint64_t size = wide ? ~mask + 1 : (uint32_t)(~(uint32_t)mask + 1);
        if (mask && size && !(size & (size - 1)))
            out[count++] = (struct lume_pci_bar){
                .resource = {.base = base,
                             .size = size,
                             .kind = io ? LUME_RESOURCE_IOPORT : LUME_RESOURCE_MMIO},
                .low = value,
                .high = high,
                .offset = offset,
                .wide = wide};
        if (wide)
            ++bar;
    }
    (void)lume_q35_pci_config_write(address, 4, 2, command);
    return count;
}

unsigned lume_q35_pci_resources(uint32_t address, struct lume_resource *out, unsigned capacity)
{
    struct lume_pci_bar bars[6];
    unsigned total = lume_q35_pci_bars(address, bars), count = 0;
    for (unsigned i = 0; i < total && count < capacity; ++i)
        if (bars[i].resource.base)
            out[count++] = bars[i].resource;
    return count;
}
