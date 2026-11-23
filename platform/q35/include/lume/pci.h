#ifndef LUME_Q35_PCI_H
#define LUME_Q35_PCI_H
#include "lume/resource.h"

enum
{
    LUME_PIC_FIRST_VECTOR = 32,
    LUME_PIC_LINES = 16,
    LUME_PIC_LEVEL_LINES = 0xdef8
};

struct lume_pci_bar
{
    struct lume_resource resource;
    uint32_t low, high;
    unsigned offset;
    bool wide;
};

uint64_t lume_q35_pci_ecam(uint32_t address, unsigned offset);
int lume_q35_pci_config_read(uint32_t address, unsigned offset, unsigned width, uint32_t *);
int lume_q35_pci_config_write(uint32_t address, unsigned offset, unsigned width, uint32_t);
unsigned lume_q35_pci_bars(uint32_t address, struct lume_pci_bar out[6]);
int lume_q35_pci_start(uint32_t parent);
int lume_q35_pci_rescan(void);
int lume_q35_pci_eject(uint32_t id);
void lume_q35_hardware_tick(void);
void lume_q35_hardware_stop(void);
#endif
