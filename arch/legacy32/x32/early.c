/* X32 instructions stay out of common kernel code. CPUINFO identity is
 * supplied by the CPU; this file never invents a CPU name/capability. */
#include "early.h"
#include "x32_cpuinfo.h"
_Noreturn void lume_early_stop(void) {
    for(;;)__asm__ volatile("cli\nhalt" ::: "memory","cc");
}
void lume_early_cpu(void) {
    lume_early_puts("CPU: ");
    for(unsigned leaf=16;leaf<18;++leaf) {
        struct x32_cpuinfo_result r=x32_cpuinfo(leaf);
        uint32_t words[4]={r.eax,r.ebx,r.ecx,r.edx};
        char name[17];unsigned count=0;
        for(unsigned i=0;i<16;++i) {
            unsigned c=(words[i/4]>>((i%4)*8))&255;
            if(!c)break;name[count++]=(char)c;
        }
        name[count]=0;lume_early_puts(name);
    }
    struct x32_cpuinfo_result caps=x32_cpuinfo(1);
    lume_early_puts(" / FORMS=");lume_early_hex(caps.edx);
    lume_early_puts(" / FEATURES=");lume_early_hex(caps.eax);
    lume_early_puts("\r\n");
}
