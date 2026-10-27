.section .rodata.ap_trampoline,"a"
.balign 16
.global lume_ap_trampoline, lume_ap_trampoline_end, lume_ap_parameters
.set AP_BASE, 0x8000
lume_ap_trampoline:
.code16
    cli
    cld
    xorw %ax, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    lgdtl AP_BASE + (ap_gdtr - lume_ap_trampoline)
    movl %cr0, %eax
    orl $1, %eax
    movl %eax, %cr0
    ljmpl $8, $AP_BASE + (ap_protected - lume_ap_trampoline)
.code32
ap_protected:
    movw $16, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movl $0x620, %eax
    movl %eax, %cr4
    movl AP_BASE + (lume_ap_parameters - lume_ap_trampoline), %eax
    movl %eax, %cr3
    movl $0xc0000080, %ecx
    rdmsr
    orl $0x900, %eax
    wrmsr
    movl %cr0, %eax
    andl $~0xc, %eax
    orl $0x80010003, %eax
    movl %eax, %cr0
    ljmpl $24, $AP_BASE + (ap_long - lume_ap_trampoline)
.code64
ap_long:
    movabsq $AP_BASE + (lume_ap_parameters - lume_ap_trampoline), %rax
    movq 8(%rax), %rdx
    movq 16(%rax), %rsp
    movq 32(%rax), %rdi
    movq 24(%rax), %rax
    jmp *%rax
.balign 8
ap_gdt:
    .quad 0, 0x00cf9a000000ffff, 0x00cf92000000ffff, 0x00af9a000000ffff
ap_gdtr:
    .word 31
    .long AP_BASE + (ap_gdt - lume_ap_trampoline)
.balign 8
lume_ap_parameters:
    .quad 0, 0, 0, 0, 0
lume_ap_trampoline_end:
.text
.code64
.global lume_secondary_entry
lume_secondary_entry:
    movq %rdx, %cr3
    xorq %rbp, %rbp
    fninit
    call lume_amd64_secondary_main
    ud2
.section .note.GNU-stack,"",@progbits
