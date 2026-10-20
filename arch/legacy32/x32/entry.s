/* Upstream U-Boot go calls an application with argc/argv in EDI/ESI.
 * Preserve those two values while establishing the kernel's private stack
 * and zeroing BSS. Flat ring0/PG0 is the temporary bringup precondition. */
.section .text.entry,"ax",@progbits
.globl _start
_start:
    cli
    cld
    mov E10,EDI
    mov E11,ESI
    mov ESP,__stack_top
    mov EDI,__bss_start
    mov ECX,__bss_end
    sub ECX,EDI
    shr ECX,2
    mov EAX,ZR
    repstos dword
    mov EDI,E10
    mov ESI,E11
    call lume_entry
    halt
