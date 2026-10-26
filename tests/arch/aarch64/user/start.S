.section .text.start,"ax"
.global _user_start
_user_start:
    mov x19, x0
    mov x27, #0x5a5a
    bl user_main
    mov x8, #4
    svc #0
1:  b 1b
.section .note.GNU-stack,"",%progbits
