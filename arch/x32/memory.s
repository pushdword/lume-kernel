/* These freestanding C helpers use E8, EDI, ESI, ECX and EDX as caller-saved registers. Dword
 * string operations require natural alignment. Arbitrary buffers use byte operations. The
 * kernel clears DF before running C code. The source and destination regions passed to memcpy
 * must not overlap. */
.text
.globl memcpy
memcpy:
    mov E8,EDI
    mov ECX,EDX
    mov EAX,EDI
    or EAX,ESI
    and EAX,3
    cmp EAX,0
    jnz near copy_bytes
    shr ECX,2
    repmov dword
    mov ECX,EDX
    and ECX,3
copy_bytes:
    repmov byte
    mov EAX,E8
    ret
.globl memset
memset:
    mov E8,EDI
    mov EAX,ESI
    and EAX,255
    mov ESI,EAX
    shl ESI,8
    or EAX,ESI
    mov ESI,EAX
    shl ESI,16
    or EAX,ESI
    mov ECX,EDI
    and ECX,3
    cmp ECX,0
    mov ECX,EDX
    jnz near fill_bytes
    shr ECX,2
    repstos dword
    mov ECX,EDX
    and ECX,3
fill_bytes:
    repstos byte
    mov EAX,E8
    ret
