/* Firmware Image entry: x0 is the physical FDT address. No runtime or Linux. */
.section .text.boot, "ax"
.balign 8
.global _start
_start:
    b boot_primary
    nop
    .quad 0                         /* text_offset from a 2 MiB aligned base */
    .quad __image_end - _start      /* Includes BSS and the early stack */
    .quad 8                         /* Little endian, arbitrary aligned base */
    .quad 0, 0, 0
    .long 0x644d5241                /* AArch64 Image header, not a Linux kernel */
    .long 0
boot_primary:
    msr daifset, #15
    mov x20, x0
    mrs x21, CurrentEL
    lsr x21, x21, #2
    /* Pi5 Cortex-A76 core number is Aff1. Park every nonzero affinity. */
    mrs x1, mpidr_el1
    mov x2, #0xffffff
    movk x2, #0xff, lsl #32
    tst x1, x2
    b.ne lume_stop
    cmp x21, #1
    b.eq boot_stack
    cmp x21, #2
    b.ne lume_stop
boot_stack:
    msr spsel, #1
    ldr x1, =__stack_top
    mov sp, x1
    ldr x1, =lume_vectors
    cmp x21, #1
    b.eq 4f
    msr vbar_el2, x1
    b 5f
4:  msr vbar_el1, x1
5:  isb
    adr x0, boot_banner
    bl lume_puts
    mov x0, x21
    bl lume_hex
    mov x0, #10
    bl lume_putc
    cmp x21, #1
    b.eq boot_el1
    /* Permit EL1 physical counter access; establish a non-virtualized EL1. */
    mrs x1, cnthctl_el2
    orr x1, x1, #3
    msr cnthctl_el2, x1
    msr cntvoff_el2, xzr
    mov x1, #0x80000000
    msr hcr_el2, x1
    mov x1, #0x33ff
    msr cptr_el2, x1
    msr hstr_el2, xzr
    mov x1, #0x0800
    movk x1, #0x30d0, lsl #16
    msr sctlr_el1, x1
    ldr x1, =__stack_top
    msr sp_el1, x1
    ldr x1, =boot_el1
    msr elr_el2, x1
    mov x1, #0x3c5                 /* Masked DAIF, EL1h */
    msr spsr_el2, x1
    isb
    eret
boot_el1:
    msr spsel, #1
    ldr x1, =__stack_top
    mov sp, x1
    ldr x1, =lume_vectors
    msr vbar_el1, x1
    isb
    ldr x1, =__bss_start
    ldr x2, =__bss_end
1:  cmp x1, x2
    b.hs 2f
    stp xzr, xzr, [x1], #16
    b 1b
2:  mov x0, x20
    mov x1, x21
    bl lume_main
.global lume_stop
lume_stop:
    msr daifset, #15
3:  wfe
    b 3b
boot_banner:
    .asciz "\nLUME AARCH64 ENTRY EL="
    .balign 4

.global lume_psci_call
.type lume_psci_call, %function
lume_psci_call:
    /* SMC32 PSCI calls may clobber x0-x17; AAPCS caller-saved agrees. */
    stp x29, x30, [sp, #-16]!
    mov x1, #0
    mov x2, #0
    mov x3, #0
    smc #0
    ldp x29, x30, [sp], #16
    ret

.balign 2048
.global lume_vectors
lume_vectors:
.macro VECTOR n
    stp x0, x1, [sp, #-288]!
    mov x0, #\n
    b vector_common
    .balign 128
.endm
    VECTOR 0
    VECTOR 1
    VECTOR 2
    VECTOR 3
    VECTOR 4
    VECTOR 5
    VECTOR 6
    VECTOR 7
    VECTOR 8
    VECTOR 9
    VECTOR 10
    VECTOR 11
    VECTOR 12
    VECTOR 13
    VECTOR 14
    VECTOR 15
vector_common:
    stp x2, x3, [sp, #16]
    stp x4, x5, [sp, #32]
    stp x6, x7, [sp, #48]
    stp x8, x9, [sp, #64]
    stp x10, x11, [sp, #80]
    stp x12, x13, [sp, #96]
    stp x14, x15, [sp, #112]
    stp x16, x17, [sp, #128]
    stp x18, x19, [sp, #144]
    stp x20, x21, [sp, #160]
    stp x22, x23, [sp, #176]
    stp x24, x25, [sp, #192]
    stp x26, x27, [sp, #208]
    stp x28, x29, [sp, #224]
    str x30, [sp, #240]
    mrs x1, sp_el0
    str x1, [sp, #248]
    mrs x1, CurrentEL
    cmp x1, #8
    b.eq 6f
    mrs x1, elr_el1
    mrs x2, spsr_el1
    b 7f
6:  mrs x1, elr_el2
    mrs x2, spsr_el2
7:  stp x1, x2, [sp, #256]
    mrs x1, tpidr_el0
    str x1, [sp, #272]
    str xzr, [sp, #280]
    mov x1, x0
    mov x0, sp
    bl lume_exception
    cbz x0, user_finished
    add sp, sp, #288
restore_context:
    mov x30, x0
    ldr x1, [x30, #248]
    msr sp_el0, x1
    ldp x1, x2, [x30, #256]
    mrs x3, CurrentEL
    cmp x3, #8
    b.eq 8f
    msr elr_el1, x1
    msr spsr_el1, x2
    b 9f
8:  msr elr_el2, x1
    msr spsr_el2, x2
9:  ldr x1, [x30, #272]
    msr tpidr_el0, x1
    ldp x0, x1, [x30, #0]
    ldp x2, x3, [x30, #16]
    ldp x4, x5, [x30, #32]
    ldp x6, x7, [x30, #48]
    ldp x8, x9, [x30, #64]
    ldp x10, x11, [x30, #80]
    ldp x12, x13, [x30, #96]
    ldp x14, x15, [x30, #112]
    ldp x16, x17, [x30, #128]
    ldp x18, x19, [x30, #144]
    ldp x20, x21, [x30, #160]
    ldp x22, x23, [x30, #176]
    ldp x24, x25, [x30, #192]
    ldp x26, x27, [x30, #208]
    ldp x28, x29, [x30, #224]
    ldr x30, [x30, #240]
    eret

.global lume_run_user
lume_run_user:
    stp x19, x20, [sp, #-16]!
    stp x21, x22, [sp, #-16]!
    stp x23, x24, [sp, #-16]!
    stp x25, x26, [sp, #-16]!
    stp x27, x28, [sp, #-16]!
    stp x29, x30, [sp, #-16]!
    ldr x1, =saved_kernel_sp
    mov x2, sp
    str x2, [x1]
    b restore_context
user_finished:
    ldr x1, =saved_kernel_sp
    ldr x1, [x1]
    mov sp, x1
    ldp x29, x30, [sp], #16
    ldp x27, x28, [sp], #16
    ldp x25, x26, [sp], #16
    ldp x23, x24, [sp], #16
    ldp x21, x22, [sp], #16
    ldp x19, x20, [sp], #16
    ret
.bss
.balign 8
saved_kernel_sp:
    .skip 8
.section .note.GNU-stack,"",%progbits
