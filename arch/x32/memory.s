/* Freestanding C helpers. Existing ABI, caller-saved E8/EDI/ESI/ECX/EDX.
 * Natural alignment is required for dword strings; arbitrary buffers use byte.
 * Kernel C runs with DF=0. memcpy's regions must not overlap. */
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
