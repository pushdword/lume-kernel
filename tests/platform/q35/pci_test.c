#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../../platform/q35/pci.c"

static uint32_t selected, registers[64], masks[64];
static bool probing[64];
#define PCI_ADDRESS 0x1800u

uint32_t lume_in32(uint16_t port)
{
    assert(port == 0xcfc);
    if ((selected & 0x00ffff00) != PCI_ADDRESS)
        return UINT32_MAX;
    unsigned index = (selected & 252) / 4;
    return probing[index] ? masks[index] : registers[index];
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
        if (!probing[index])
            registers[index] = value;
    }
}

uint64_t lume_q35_pci_ecam(uint32_t address, unsigned offset)
{
    (void)address;
    (void)offset;
    return 0;
}

int main(void)
{
    registers[0] = 0x10011af4;
    registers[1] = 7;
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
    puts("Q35_PCI_PASS BAR geometry, 64-bit pairs, capacity and configuration restoration");
    return 0;
}
