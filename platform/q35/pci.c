#include "lume/q35.h"
#include "lume/resource.h"
#include "lume/pci.h"

uint32_t lume_q35_pci_read(uint32_t device, unsigned offset)
{
    if ((device & ~UINT32_C(0x00ffff00)) || (offset & 3) || offset >= 4096)
        return UINT32_MAX;
    uint64_t ecam = lume_q35_pci_ecam(device, offset);
    if (ecam)
        return *(const volatile uint32_t *)(uintptr_t)ecam;
    if (offset >= 256)
        return UINT32_MAX;
    lume_out32(0xcf8, 0x80000000u | device | (offset & 252));
    return lume_in32(0xcfc);
}

void lume_q35_pci_write(uint32_t device, unsigned offset, uint32_t value)
{
    if ((device & ~UINT32_C(0x00ffff00)) || (offset & 3) || offset >= 4096)
        return;
    uint64_t ecam = lume_q35_pci_ecam(device, offset);
    if (ecam)
    {
        *(volatile uint32_t *)(uintptr_t)ecam = value;
        return;
    }
    if (offset >= 256)
        return;
    lume_out32(0xcf8, 0x80000000u | device | (offset & 252));
    lume_out32(0xcfc, value);
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
    uint32_t command = lume_q35_pci_read(address, 4) & 0xffff;
    unsigned count = 0;
    lume_q35_pci_write(address, 4, command & ~3u);
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
    lume_q35_pci_write(address, 4, command);
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
