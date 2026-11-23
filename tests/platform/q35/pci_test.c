#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../../platform/q35/pci.c"

static uint32_t selected, registers[64], masks[64];
static bool probing[64];
static bool ecam_enabled;
static _Alignas(4) unsigned char ecam_page[4096];
#define PCI_ADDRESS 0x1800u

uint64_t lume_irq_save(void)
{
    return 0;
}

void lume_irq_restore(uint64_t flags)
{
    assert(!flags);
}

uint32_t lume_in32(uint16_t port)
{
    assert(port == 0xcfc);
    if ((selected & 0x00ffff00) != PCI_ADDRESS)
        return UINT32_MAX;
    unsigned index = (selected & 252) / 4;
    return probing[index] ? masks[index] : registers[index];
}

uint8_t lume_in8(uint16_t port)
{
    assert(port >= 0xcfc && port <= 0xcff);
    return (uint8_t)(lume_in32(0xcfc) >> (8 * (port - 0xcfc)));
}

uint16_t lume_in16(uint16_t port)
{
    assert(port == 0xcfc || port == 0xcfe);
    return (uint16_t)(lume_in32(0xcfc) >> (8 * (port - 0xcfc)));
}

void lume_out32(uint16_t port, uint32_t value)
{
    if (port == 0xcf8)
        selected = value;
    else
    {
        assert(port == 0xcfc && (selected & 0x00ffff00) == PCI_ADDRESS);
        unsigned index = (selected & 252) / 4;
        probing[index] = index >= 4 && index < 10 && value == UINT32_MAX;
        if (index == 1)
            registers[index] = (registers[index] & ~value & 0xffff0000u) | (value & 0xffffu);
        else if (!probing[index])
            registers[index] = value;
    }
}

static void write_lane(uint16_t port, uint32_t mask, uint32_t value)
{
    assert((selected & 0x00ffff00) == PCI_ADDRESS);
    unsigned index = (selected & 252) / 4;
    unsigned shift = 8 * (port - 0xcfc);
    uint32_t bits = mask << shift;
    if (index == 1 && shift >= 16)
        registers[index] &= ~(value << shift);
    else
        registers[index] = (registers[index] & ~bits) | (value << shift);
}

void lume_out8(uint16_t port, uint8_t value)
{
    assert(port >= 0xcfc && port <= 0xcff);
    write_lane(port, UINT8_MAX, value);
}

void lume_out16(uint16_t port, uint16_t value)
{
    assert(port == 0xcfc || port == 0xcfe);
    write_lane(port, UINT16_MAX, value);
}

uint64_t lume_q35_pci_ecam(uint32_t address, unsigned offset)
{
    return ecam_enabled && address == PCI_ADDRESS ? (uintptr_t)(ecam_page + offset) : 0;
}

static void check_config_widths(void)
{
    uint32_t value = 0;
    assert(!lume_q35_pci_config_read(PCI_ADDRESS, 1, 1, &value) && value == 0x1a);
    assert(!lume_q35_pci_config_read(PCI_ADDRESS, 2, 2, &value) && value == 0x1001);
    assert(lume_q35_pci_config_read(PCI_ADDRESS, 1, 2, &value) == -LUME_EINVAL);
    assert(lume_q35_pci_config_read(PCI_ADDRESS, 0, 3, &value) == -LUME_EINVAL);
    assert(lume_q35_pci_config_read(PCI_ADDRESS | 4, 0, 4, &value) == -LUME_EINVAL);
    assert(lume_q35_pci_config_read(PCI_ADDRESS, 256, 1, &value) == -LUME_EOPNOTSUPP);
    assert(lume_q35_pci_config_write(PCI_ADDRESS, 256, 1, 0) == -LUME_EOPNOTSUPP);
    assert(lume_q35_pci_config_write(PCI_ADDRESS, 4096, 4, 0) == -LUME_EINVAL);
    registers[1] = 0xa5e70007;
    assert(!lume_q35_pci_config_write(PCI_ADDRESS, 4, 2, 0x0410));
    assert(registers[1] == 0xa5e70410);
    assert(!lume_q35_pci_config_write(PCI_ADDRESS, 5, 1, 0x09));
    assert(registers[1] == 0xa5e70910);
    assert(!lume_q35_pci_config_write(PCI_ADDRESS, 6, 1, 0x20));
    assert(registers[1] == 0xa5c70910);
    registers[1] = 7;

    ecam_enabled = true;
    memset(ecam_page + 4092, 0xa5, 4);
    assert(!lume_q35_pci_config_write(PCI_ADDRESS, 4092, 2, 0x1234));
    assert(!lume_q35_pci_config_write(PCI_ADDRESS, 4095, 1, 0x67));
    assert(!lume_q35_pci_config_read(PCI_ADDRESS, 4092, 4, &value) && value == 0x67a51234);
    assert(lume_q35_pci_config_read(PCI_ADDRESS, 4095, 2, &value) == -LUME_EINVAL);
    ecam_enabled = false;
}

int main(void)
{
    registers[0] = 0x10011af4;
    registers[1] = 7;
    check_config_widths();
    registers[4] = 0xc001;
    masks[4] = 0xffffffe1;
    registers[5] = 0xe0000008;
    masks[5] = 0xff000008;
    registers[6] = 0xd0000004;
    masks[6] = 0xffe00004;
    masks[7] = UINT32_MAX;
    uint32_t original[64];
    memcpy(original, registers, sizeof(original));
    uint32_t found;
    assert(!lume_q35_pci_find(0x1af4, 0x1001, &found) && found == PCI_ADDRESS);
    assert(lume_q35_pci_find(0x1af4, 0x1005, &found));
    struct lume_resource resources[6];
    assert(lume_q35_pci_resources(PCI_ADDRESS, resources, 6) == 3);
    assert(resources[0].kind == LUME_RESOURCE_IOPORT && resources[0].base == 0xc000 &&
           resources[0].size == 32);
    assert(resources[1].kind == LUME_RESOURCE_MMIO && resources[1].base == 0xe0000000 &&
           resources[1].size == 0x1000000);
    assert(resources[2].kind == LUME_RESOURCE_MMIO && resources[2].base == 0xd0000000 &&
           resources[2].size == 0x200000);
    assert(!memcmp(original, registers, sizeof(original)));
    for (unsigned i = 0; i < 64; ++i)
        assert(!probing[i]);
    assert(lume_q35_pci_resources(PCI_ADDRESS, resources, 2) == 2);
    assert(!memcmp(original, registers, sizeof(original)));
    registers[7] = 1;
    assert(lume_q35_pci_resources(PCI_ADDRESS, resources, 6) == 3);
    assert(resources[2].base == UINT64_C(0x1d0000000));
    puts("Q35_PCI_PASS configuration widths, status preservation, BAR geometry and restoration");
    return 0;
}
