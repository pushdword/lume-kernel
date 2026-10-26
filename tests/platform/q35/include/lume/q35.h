#ifndef LUME_Q35_H
#define LUME_Q35_H
#include "lume/early.h"
uint8_t lume_in8(uint16_t);
uint16_t lume_in16(uint16_t);
uint32_t lume_in32(uint16_t);
void lume_out8(uint16_t, uint8_t);
void lume_out16(uint16_t, uint16_t);
void lume_out32(uint16_t, uint32_t);
uint32_t lume_q35_pci_read(uint32_t, unsigned);
void lume_q35_pci_write(uint32_t, unsigned, uint32_t);
int lume_q35_pci_find(uint16_t, uint16_t, uint32_t *);
#endif
