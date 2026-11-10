#include "lume/amd64_query.h"
.text
.macro TRAP n, error=0
.global lume_isr_\n
lume_isr_\n:
.if !\error
    pushq $0
.endif
    pushq $\n
    jmp lume_trap_common
.endm
.irp n,0,1,2,3,4,5,6,7,9,15,16,18,19,20,22,23,24,25,26,27,28,31
TRAP \n
.endr
.irp n,8,10,11,12,13,14,17,21,29,30
TRAP \n,1
.endr
.irp n,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,128,255
TRAP \n
.endr
.global lume_syscall_entry
lume_syscall_entry:
    swapgs
    movq %rsp, %gs:LUME_AMD64_ENTRY_USER_SP
    movq %gs:LUME_AMD64_ENTRY_STACK, %rsp
    pushq $0x1b
    pushq %gs:LUME_AMD64_ENTRY_USER_SP
    pushq %r11
    pushq $0x23
    pushq %rcx
    pushq %gs:LUME_AMD64_ENTRY_CPU
    pushq $128
    swapgs
    jmp lume_trap_common
lume_trap_common:
    cld
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rbp
    pushq %rsi
    pushq %rdi
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq %rsp, %r12
    cmpq $128, 120(%r12)
    jne lume_trap_capture
    cmpq $0x23, 144(%r12)
    jne lume_trap_capture
    cmpq $LUME_AMD64_QUERY_SYSCALL_PID, 56(%r12)
    je lume_trap_query
    cmpq $LUME_AMD64_QUERY_SYSCALL_THREAD, 56(%r12)
    je lume_trap_query
    cmpq $LUME_AMD64_QUERY_SYSCALL_READ, 56(%r12)
    je lume_trap_query
    cmpq $LUME_AMD64_QUERY_SYSCALL_WRITE, 56(%r12)
    je lume_trap_query
    cmpq $LUME_AMD64_QUERY_SYSCALL_TTY, 56(%r12)
    jne lume_trap_capture
lume_trap_query:
    andq $-16, %rsp
    movq %r12, %rdi
    call lume_amd64_query_try
    movq %r12, %rsp
    testb %al, %al
    jz lume_trap_capture
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rdi
    popq %rsi
    popq %rbp
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax
    cmpq $0, 8(%rsp)
    jne lume_query_sysret
    addq $16, %rsp
    iretq
lume_query_sysret:
    movq 16(%rsp), %rcx
    movq 32(%rsp), %r11
    movq 40(%rsp), %rsp
    sysretq
lume_trap_capture:
    subq $704, %rsp
    andq $-16, %rsp
    fxsave64 176(%rsp)
    movq %rsp, %rdi
    movq %r12, %rsi
    call lume_amd64_context_capture
    movq %rsp, %rdi
    movq 120(%r12), %rsi
    movq 128(%r12), %rdx
    cmpq $128, %rsi
    jne 1f
    xorl %edx, %edx
1:
    call lume_amd64_exception
    testq %rax, %rax
    jz lume_user_finished
    movq %rax, %rdi
    jmp lume_context_restore
.global lume_run_user
lume_run_user:
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    subq $8, %rsp
    movq %rdi, %r12
    call lume_cpu_index
    leaq lume_kernel_return_sp(%rip), %rcx
    movq %rsp, (%rcx,%rax,8)
    movq %r12, %rdi
lume_context_restore:
    call lume_amd64_context_prepare
    movq %rax, %rdi
    fxrstor64 176(%rdi)
    movq $16, %rax
    movq $8, %rcx
    cmpq $0, 144(%rdi)
    jne 1f
    movq $0x1b, %rax
    movq $0x23, %rcx
1:
    pushq %rax
    pushq 128(%rdi)
    pushq 160(%rdi)
    pushq %rcx
    pushq 136(%rdi)
    movq 8(%rdi), %rsi
    movq 16(%rdi), %rdx
    movq 24(%rdi), %rcx
    movq 32(%rdi), %rax
    movq 40(%rdi), %rbx
    movq 48(%rdi), %rbp
    movq 56(%rdi), %r9
    movq 64(%rdi), %r8
    movq 72(%rdi), %r10
    movq 80(%rdi), %r11
    movq 88(%rdi), %r12
    movq 96(%rdi), %r13
    movq 104(%rdi), %r14
    movq 112(%rdi), %r15
    movq 0(%rdi), %rdi
    iretq
lume_user_finished:
    call lume_cpu_index
    leaq lume_kernel_return_sp(%rip), %rcx
    movq (%rcx,%rax,8), %rsp
    addq $8, %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp
    ret
.bss
.balign 8
lume_kernel_return_sp: .skip 32
.section .rodata
.global lume_isr_table
lume_isr_table:
.irp n,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,128,255
    .quad lume_isr_\n
.endr
.section .note.GNU-stack,"",@progbits
