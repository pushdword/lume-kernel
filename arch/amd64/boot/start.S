.section .multiboot,"a"
.balign 4
lume_multiboot_header:
    .long 0x1badb002, 0x00010003, -(0x1badb002 + 0x00010003)
    .long lume_multiboot_header, __image_start, __load_end, __image_end, lume_boot32

.section .text.boot,"ax"
.code32
.global lume_boot32
lume_boot32:
    cli
    cld
    movl %eax, %esi
    movl %ebx, %ebp
    movl $__bss_start, %edi
    movl $__bss_end, %ecx
    subl %edi, %ecx
    xorl %eax, %eax
    rep stosb
    movl %esi, lume_boot_magic
    movl %ebp, lume_boot_info
    movl $__stack_top, %esp
    movl $1, %eax
    cpuid
    andl $0x07000000, %edx
    cmpl $0x07000000, %edx
    jne boot_halt
    movl $0x80000000, %eax
    cpuid
    cmpl $0x80000001, %eax
    jb boot_halt
    movl $0x80000001, %eax
    cpuid
    andl $0x20100000, %edx
    cmpl $0x20100000, %edx
    jne boot_halt
    movl $lume_boot_pdpt, %eax
    orl $3, %eax
    movl %eax, lume_boot_pml4
    movl $lume_boot_pd, %eax
    orl $3, %eax
    movl %eax, lume_boot_pdpt
    movl $lume_boot_pd, %edi
    xorl %eax, %eax
    movl $512, %ecx
1:
    movl %eax, %edx
    orl $0x83, %edx
    movl %edx, (%edi)
    movl $0, 4(%edi)
    addl $0x200000, %eax
    addl $8, %edi
    loop 1b
    lgdt lume_boot_gdtr
    movl $0x620, %eax
    movl %eax, %cr4
    movl $lume_boot_pml4, %eax
    movl %eax, %cr3
    movl $0xc0000080, %ecx
    rdmsr
    orl $0x900, %eax
    wrmsr
    movl %cr0, %eax
    andl $~0xc, %eax
    orl $0x80010003, %eax
    movl %eax, %cr0
    ljmp $8, $lume_boot64
boot_halt:
    hlt
    jmp boot_halt
.code64
lume_boot64:
    movw $16, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    xorw %ax, %ax
    movw %ax, %fs
    movw %ax, %gs
    movq $__stack_top, %rsp
    xorq %rbp, %rbp
    fninit
    movl lume_boot_info(%rip), %edi
    movl lume_boot_magic(%rip), %esi
    call lume_main
    jmp boot_halt
.section .rodata
.balign 8
lume_boot_gdt:
    .quad 0, 0x00af9a000000ffff, 0x00cf92000000ffff
lume_boot_gdtr:
    .word 23
    .long lume_boot_gdt
.section .bss
.balign 4096
.global lume_boot_pml4
lume_boot_pml4: .skip 4096
lume_boot_pdpt: .skip 4096
lume_boot_pd: .skip 4096
lume_boot_info: .skip 4
lume_boot_magic: .skip 4
.section .note.GNU-stack,"",@progbits
