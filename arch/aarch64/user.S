/* Embedded AArch64 EL0 diagnostic payload; only the private bring-up ABI.
 * No X32 binary ABI, libc or privileged instructions are used here.
 */
.section .user.text,"ax"
.balign 4096
.global lume_user_entry
lume_user_entry:
    mov x19, x0                    /* Expected PID */
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
    mov x3, #0x4000000000
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
