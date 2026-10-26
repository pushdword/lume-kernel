#ifndef LUME_Q35_H
#define LUME_Q35_H
#include "lume/early.h"

static inline uint8_t lume_in8(uint16_t port)
{
    uint8_t value;
    __asm__ volatile("inb %w1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline uint16_t lume_in16(uint16_t port)
{
    uint16_t value;
    __asm__ volatile("inw %w1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline uint32_t lume_in32(uint16_t port)
{
    uint32_t value;
    __asm__ volatile("inl %w1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void lume_out8(uint16_t port, uint8_t value)
{
    __asm__ volatile("outb %0, %w1" : : "a"(value), "Nd"(port) : "memory");
}

static inline void lume_out16(uint16_t port, uint16_t value)
{
    __asm__ volatile("outw %0, %w1" : : "a"(value), "Nd"(port) : "memory");
}

static inline void lume_out32(uint16_t port, uint32_t value)
{
    __asm__ volatile("outl %0, %w1" : : "a"(value), "Nd"(port) : "memory");
}

static inline uint64_t lume_rdmsr(uint32_t index)
{
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(index));
    return (uint64_t)hi << 32 | lo;
}

static inline void lume_wrmsr(uint32_t index, uint64_t value)
{
    __asm__ volatile("wrmsr"
                     :
                     : "c"(index), "a"((uint32_t)value), "d"((uint32_t)(value >> 32))
                     : "memory");
}

void lume_q35_console_start(void);
void lume_q35_interrupt_start(void);
void lume_q35_clock_start(void);
unsigned lume_q35_console_geometry(void);
void lume_amd64_context_start(void);
bool lume_q35_boot_read(uint64_t, uint64_t, struct lume_fdt *);
uint32_t lume_q35_pci_read(uint32_t, unsigned);
void lume_q35_pci_write(uint32_t, unsigned, uint32_t);
int lume_q35_pci_find(uint16_t, uint16_t, uint32_t *);
struct lume_resource;
unsigned lume_q35_pci_resources(uint32_t, struct lume_resource *, unsigned);
int lume_q35_hardware_start(void);
#endif
