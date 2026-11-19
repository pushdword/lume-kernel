/* BIOS disk layout: MBR, eight loader sectors, kernel and FAT application root. */
#ifndef KERNEL_ADDRESS
#define KERNEL_ADDRESS 0x200000
#endif
#ifndef ROOT_ADDRESS
#define ROOT_ADDRESS 0x2000000
#endif
.section .text,"ax"
.code16
.macro serial_helpers
serial_init:
    movw $0x3f9, %dx
    xorb %al, %al
    outb %al, %dx
    movw $0x3fb, %dx
    movb $0x80, %al
    outb %al, %dx
    movw $0x3f8, %dx
    movb $1, %al
    outb %al, %dx
    incw %dx
    xorb %al, %al
    outb %al, %dx
    movw $0x3fb, %dx
    movb $3, %al
    outb %al, %dx
    movw $0x3fa, %dx
    movb $0xc7, %al
    outb %al, %dx
    movw $0x3fc, %dx
    movb $0x0b, %al
    outb %al, %dx
    ret
serial_byte:
    pushw %dx
    pushw %ax
    movw $0x3fd, %dx
1:
    inb %dx, %al
    testb $0x20, %al
    jz 1b
    popw %ax
    movw $0x3f8, %dx
    outb %al, %dx
    popw %dx
    ret
.endm
.global _start
_start:
    ljmp $0, $entry
entry:
    cli
    cld
    xorw %ax, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw $0x7c00, %sp
    sti
    movb %dl, drive
    call serial_init
#if defined(MBR)
    movw $mbr_text, %si
    call print
#if defined(BIOS_DISK_CHS)
    movw $0x8000, %bx
    movw $2, %cx
    xorw %dx, %dx
    movb drive, %dl
    movw $0x0208, %ax
    int $0x13
#else
    movw $packet, %si
    movb drive, %dl
    movb $0x42, %ah
    int $0x13
#endif
    jc error
    movb drive, %dl
    ljmp $0, $0x8000
.balign 4
packet:
    .byte 16, 0
    .word 8
    .word 0x8000, 0
    .quad 1
error:
    movw $error_text, %si
    call print
halt:
    cli
    hlt
    jmp halt
print:
    lodsb
    testb %al, %al
    jz 1f
    call serial_byte
    movb $0x0e, %ah
    movw $7, %bx
    int $0x10
    jmp print
1:
    ret
serial_helpers
mbr_text: .asciz "Lume BIOS MBR\r\n"
error_text: .asciz "Lume BIOS loader read error"
drive: .byte 0
.org 510
    .word 0xaa55
#else
    movw $3, %ax
    int $0x10
    movw $loader_text, %si
    call print
#if defined(BIOS_DISK_CHS)
    movb drive, %dl
    movb $8, %ah
    int $0x13
    jc error
    movzbl %cl, %eax
    andl $63, %eax
    jz error
    movl %eax, geometry_sectors
    movzbl %dh, %eax
    incl %eax
    movl %eax, geometry_heads
    xorw %ax, %ax
    movw %ax, %es
#endif
    inb $0x92, %al
    orb $2, %al
    andb $0xfe, %al
    outb %al, $0x92
    movw $0x5000, %di
    movw $0x1200, %cx
    xorw %ax, %ax
    rep stosb
    xorl %ebx, %ebx
    movw $0x6000, %di
    movw $64, map_remaining
map_next:
    movl $20, (%di)
    addw $4, %di
    movl $0xe820, %eax
    movl $0x534d4150, %edx
    movl $20, %ecx
    int $0x15
    jc error
    cmpl $0x534d4150, %eax
    jne error
    cmpl $20, %ecx
    jb error
    addw $20, %di
    testl %ebx, %ebx
    jz map_done
    decw map_remaining
    jnz map_next
    jmp error
map_done:
    subw $0x6000, %di
    movzwl %di, %edi
    movl %edi, 0x502c
    movl $0x6000, 0x5030
    movl $0x48, 0x5000
    movl $1, 0x5014
    movl $0x5100, 0x5018
    movl $ROOT_ADDRESS, 0x5100
    movl $(ROOT_ADDRESS + ROOT_BYTES), 0x5104
    movw $kernel_text, %si
    call print
    movl $9, packet_lba
    movl $KERNEL_SECTORS, remaining
    movl $KERNEL_ADDRESS, destination
    call load
    movw $root_text, %si
    call print
    movl $ROOT_LBA, packet_lba
    movl $ROOT_SECTORS, remaining
    movl $ROOT_ADDRESS, destination
    call load
    movw $enter_text, %si
    call print
    cli
    lgdt gdtr
    movl %cr0, %eax
    orl $1, %eax
    movl %eax, %cr0
    ljmpl $8, $launch
load:
    movl remaining, %eax
    testl %eax, %eax
    jz loaded
    cmpl $64, %eax
    jbe 1f
    movl $64, %eax
1:
    movw %ax, packet_count
#if defined(BIOS_DISK_CHS)
    movl packet_lba, %eax
    xorl %edx, %edx
    divl geometry_sectors
    movl %edx, %ebx
    movb %bl, %cl
    incb %cl
    movl geometry_sectors, %ebp
    subl %edx, %ebp
    movzwl packet_count, %esi
    cmpl %ebp, %esi
    jbe 2f
    movw %bp, packet_count
2:
    xorl %edx, %edx
    divl geometry_heads
    cmpl $1023, %eax
    ja error
    movb %dl, %dh
    movb %al, %ch
    shrl $2, %eax
    andb $0xc0, %al
    orb %al, %cl
    movb drive, %dl
    movw $0x1000, %ax
    movw %ax, %es
    xorw %bx, %bx
    movb packet_count, %al
    movb $2, %ah
    int $0x13
    jc error
    xorw %ax, %ax
    movw %ax, %es
#else
    movw $packet, %si
    movb drive, %dl
    movb $0x42, %ah
    int $0x13
    jc error
#endif
    cli
    lgdt gdtr
    movl %cr0, %eax
    orl $1, %eax
    movl %eax, %cr0
    ljmpl $8, $copy
loaded:
    ret
.code32
copy:
    movw $16, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movl $0x10000, %esi
    movl destination, %edi
    movzwl packet_count, %ecx
    movl %ecx, %edx
    shll $7, %ecx
    cld
    rep movsl
    movl %edi, destination
    addl %edx, packet_lba
    subl %edx, remaining
    ljmp $24, $return16
.code16
return16:
    movl %cr0, %eax
    andl $0xfffffffe, %eax
    movl %eax, %cr0
    ljmp $0, $real
real:
    xorw %ax, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    sti
    jmp load
.code32
launch:
    movw $16, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movl $0x7c00, %esp
    movl $0x2badb002, %eax
    movl $0x5000, %ebx
    movl $KERNEL_ENTRY, %ecx
    jmp *%ecx
.code16
error:
    movw $error_text, %si
    call print
halt:
    cli
    hlt
    jmp halt
print:
    lodsb
    testb %al, %al
    jz 1f
    call serial_byte
    movb $0x0e, %ah
    movw $7, %bx
    int $0x10
    jmp print
1:
    ret
serial_helpers
.balign 8
gdt:
    .quad 0, 0x00cf9a000000ffff, 0x00cf92000000ffff, 0x00009a000000ffff
gdtr:
    .word 31
    .long gdt
.balign 4
packet:
    .byte 16, 0
packet_count: .word 0
    .word 0, 0x1000
packet_lba: .quad 0
remaining: .long 0
destination: .long 0
#if defined(BIOS_DISK_CHS)
geometry_sectors: .long 0
geometry_heads: .long 0
#endif
map_remaining: .word 0
drive: .byte 0
loader_text: .asciz "Lume BIOS loader\r\n"
enter_text: .asciz "Entering Lume kernel...\r\n"
kernel_text: .asciz "Loading Lume kernel...\r\n"
root_text: .asciz "Loading application root...\r\n"
error_text: .asciz "Lume BIOS loader read or memory map error"
#endif
.section .note.GNU-stack,"",@progbits
