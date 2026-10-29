.text
.global lume_fiber_switch
lume_fiber_switch:
    movl 4(%esp), %eax
    movl 8(%esp), %edx
    movl %ebx, 0(%eax)
    movl %ebp, 4(%eax)
    movl %esi, 8(%eax)
    movl %edi, 12(%eax)
    movl %esp, 16(%eax)
    movl 0(%edx), %ebx
    movl 4(%edx), %ebp
    movl 8(%edx), %esi
    movl 12(%edx), %edi
    movl 16(%edx), %esp
    ret
.section .note.GNU-stack,"",@progbits
