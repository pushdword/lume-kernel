#ifndef LUME_AMD64_APIC_H
#define LUME_AMD64_APIC_H
#include <stdbool.h>
#include <stdint.h>

#define LUME_APIC_TIMER 48u
#define LUME_APIC_WAKE 49u
#define LUME_APIC_TLB 50u
#define LUME_AP_TRAMPOLINE UINT64_C(0x8000)
#define LUME_AP_STACK_BYTES UINT64_C(0x50000)

void lume_apic_start(void);
void lume_apic_secondary(void);
void lume_apic_send(unsigned id, uint32_t command);
void lume_apic_eoi(void);
void lume_apic_timer_start(void);
void lume_apic_timer_stop(void);
bool lume_apic_available(void);
#endif
