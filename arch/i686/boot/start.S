.section .multiboot,"a"
.balign 4
    .long 0x1badb002, 3, -(0x1badb002 + 3)
.section .text.boot,"ax"
.code32
.global lume_boot32
lume_boot32:
    cli
    cld
    movl %eax, %esi
    movl %ebx, %ebp
    lgdt boot_gdtr
    ljmp $8, $1f
1:
    movw $16, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movl $__stack_top, %esp
    movl $__bss_start, %edi
    movl $__bss_end, %ecx
    subl %edi, %ecx
    xorl %eax, %eax
    rep stosb
    movl %cr0, %eax
    andl $~0xc, %eax
    orl $0x10003, %eax
    movl %eax, %cr0
    fninit
    pushl $0
    pushl %esi
    pushl $0
    pushl %ebp
    call lume_main
2:
    hlt
    jmp 2b
.section .rodata
.balign 8
boot_gdt:
    .quad 0, 0x00cf9a000000ffff, 0x00cf92000000ffff
boot_gdtr:
    .word 23
    .long boot_gdt
.section .note.GNU-stack,"",@progbits
