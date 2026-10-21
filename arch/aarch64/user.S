/* The embedded EL0 payload exercises the AArch64 diagnostic ABI without libc or privileged
 * instructions. */
.section .user.text,"ax"
.balign 4096
.global lume_user_entry
lume_user_entry:
    mov x19, x0                    /* Expected PID */
    .irp r,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
    mov x9, #(\r * 256)
    add x9, x19, x9
    dup v\r\().2d, x9
    .endr
    lsl x9, x19, #22
    msr fpcr, x9
    msr fpsr, x19
    mov x20, x1                    /* Private data VA, same VA in both tasks */
    mov x21, x2                    /* Exit after this many timer ticks */
    ldr x23, [x20, #8]             /* Private page canary */
    mov x24, #0
    mov x27, #0x5a5a
    mov x8, #99
    svc #0
    cmn x0, #38                    /* ENOSYS */
    b.ne user_fail
    mov x8, #3
    mov x0, #1
    mov x1, #0x200000
    mov x2, #16
    svc #0
    cmn x0, #14                    /* EFAULT, never read kernel memory */
    b.ne user_fail
    mov x3, #0x200000
.global lume_user_kernel_probe
lume_user_kernel_probe:
    ldr x0, [x3]                   /* Expected EL0 permission fault */
    cmn x0, #14
    b.ne user_fail
    mov x3, #0x40000000
.global lume_user_ro_probe
lume_user_ro_probe:
    str xzr, [x3]                  /* Expected write-to-code permission fault */
    cmn x0, #14
    b.ne user_fail
    mov x8, #3
    mov x0, #1
    add x1, x20, #64
    mov x2, #14
    svc #0
    cmp x0, #14
    b.ne user_fail
user_spin:
    .irp r,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
    mov x9, #(\r * 256)
    add x9, x19, x9
    umov x28, v\r\().d[0]
    cmp x28, x9
    b.ne user_fail
    umov x28, v\r\().d[1]
    cmp x28, x9
    b.ne user_fail
    .endr
    mrs x28, fpcr
    lsl x9, x19, #22
    cmp x28, x9
    b.ne user_fail
    mrs x28, fpsr
    cmp x28, x19
    b.ne user_fail
    add x24, x24, #1
    str x24, [x20]
    tst x24, #0x1fff
    b.ne user_spin
    ldr x25, [x20, #8]
    cmp x25, x23
    b.ne user_fail
    mov x25, #0x5a5a
    cmp x27, x25
    b.ne user_fail
    mov x8, #1
    svc #0
    cmp x0, x19
    b.ne user_fail
    mov x8, #2
    svc #0
    cmp x0, x21
    b.lo user_spin
    mov x0, #0
    b user_exit
user_fail:
    mov x0, #1
user_exit:
    mov x8, #4
    svc #0
    b user_fail
.section .note.GNU-stack,"",%progbits
