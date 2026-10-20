/* X32 contracts and assembly symbols remain unchanged. */
#ifndef LUME_ARCH_X32_CPU_H
#define LUME_ARCH_X32_CPU_H
#define LUME_ARCH_NAME "x32"
#define LUME_ARCH_ELF_MACHINE 0xfe32u
#define arch_compare_exchange32 x32_cas
#define arch_enter_context x32_run_context
static inline __attribute__((always_inline)) void arch_set_page_root(uint32_t root){
    __asm__ volatile("mov CR3,%0"::"r"(root):"memory");
}
/* The established X32 profile always provides KBD1. Keep its exact probe. */
static inline __attribute__((always_inline)) unsigned arch_keyboard_probe(void){
    return *(volatile uint32_t *)0xf0005000u == 0x3144424bu;
}
#endif
