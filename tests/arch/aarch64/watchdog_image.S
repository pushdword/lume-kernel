/* Stop with interrupts masked and leave the inherited watchdog unserviced. */
.section .text.boot,"ax"
.balign 8
.global _start
_start:
    b entered
    nop
    .quad 0
    .quad __image_end - _start
    .quad 8
    .quad 0, 0, 0
    .long 0x644d5241
    .long 0
entered:
    msr daifset, #15
    ldr x1, =0x1c00030000
    adr x2, message
next:
    ldrb w3, [x2], #1
    cbz w3, stopped
wait:
    ldr w4, [x1, #0x18]
    tbnz w4, #5, wait
    str w3, [x1]
    b next
stopped:
    dsb sy
1:  wfe
    b 1b
message:
    .asciz "\nLUME WATCHDOG RESET TEST HALTED\n"
    .balign 8
.section .note.GNU-stack,"",%progbits
