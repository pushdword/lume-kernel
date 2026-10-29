.text
.macro TRAP n, error=0
.global lume_isr_\n
lume_isr_\n:
.if !\error
    pushl $0
.endif
    pushl $\n
    jmp lume_trap_common
.endm
.irp n,0,1,2,3,4,5,6,7,9,15,16,18,19,20,22,23,24,25,26,27,28,31
TRAP \n
.endr
.irp n,8,10,11,12,13,14,17,21,29,30
TRAP \n,1
.endr
.irp n,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,128,255
TRAP \n
.endr
lume_trap_common:
    cld
    pushal
    movl %esp, %ebx
    movw $16, %ax
    movw %ax, %ds
    movw %ax, %es
    subl $464, %esp
    andl $-16, %esp
    movl %esp, %edi
    fnsave 336(%edi)
    subl $8, %esp
    pushl %ebx
    pushl %edi
    call lume_i686_context_capture
    addl $16, %esp
    subl $4, %esp
    pushl 36(%ebx)
    pushl 32(%ebx)
    pushl %edi
    call lume_i686_exception
    addl $16, %esp
    testl %eax, %eax
    jz lume_user_finished
    jmp lume_context_restore
.global lume_run_user
lume_run_user:
    pushl %ebp
    pushl %ebx
    pushl %esi
    pushl %edi
    movl %esp, lume_kernel_return_sp
    movl 20(%esp), %eax
lume_context_restore:
    subl $12, %esp
    pushl %eax
    call lume_i686_context_prepare
    movl %eax, %esp
    movw $0x23, %ax
    movw %ax, %ds
    movw %ax, %es
    movw $0x33, %ax
    movw %ax, %gs
    popal
    iretl
lume_user_finished:
    movl lume_kernel_return_sp, %esp
    popl %edi
    popl %esi
    popl %ebx
    popl %ebp
    ret
.bss
.balign 4
lume_kernel_return_sp: .skip 4
.section .rodata
.global lume_isr_table
lume_isr_table:
.irp n,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,128,255
    .long lume_isr_\n
.endr
.section .note.GNU-stack,"",@progbits
