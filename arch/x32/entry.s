.section .text.entry,"ax",@progbits
.globl _start
_start:
cli
cld
mov ESP,0x13ed8000
mov EDI,__bss_start
mov ECX,__bss_end
sub ECX,EDI
shr ECX,2
mov EAX,ZR
repstos dword
call kernel_main
halt
.globl secondary_entry
secondary_entry:
cli
cld
mov ESP,0x13edc000
mov EDI,1
call cpu_start
halt
.globl x32_cas
x32_cas:
mov EAX,ESI
cas32 EAX,[EDI],EDX
ret
.globl idle_loop
idle_loop:
stihalt
jmp near idle_loop
.globl vector_entry
vector_entry:
sub ESP,144
mov dword [ESP+0],ZR
mov dword [ESP+4],EAX
mov dword [ESP+8],ECX
mov dword [ESP+12],EDX
mov dword [ESP+16],EBX
mov dword [ESP+24],EBP
mov dword [ESP+28],ESI
mov dword [ESP+32],EDI
mov dword [ESP+36],E8
mov dword [ESP+40],E9
mov dword [ESP+44],E10
mov dword [ESP+48],E11
mov dword [ESP+52],E12
mov dword [ESP+56],E13
mov dword [ESP+60],E14
mov dword [ESP+64],E15
mov dword [ESP+68],E16
mov dword [ESP+72],E17
mov dword [ESP+76],E18
mov dword [ESP+80],E19
mov dword [ESP+84],E20
mov dword [ESP+88],E21
mov dword [ESP+92],E22
mov dword [ESP+96],E23
mov dword [ESP+100],E24
mov dword [ESP+104],E25
mov dword [ESP+108],E26
mov dword [ESP+112],E27
mov dword [ESP+116],E28
mov dword [ESP+120],E29
mov dword [ESP+124],E30
mov EAX,dword [ESP+156]
mov dword [ESP+20],EAX
mov EAX,DS
mov dword [ESP+128],EAX
mov EAX,ES
mov dword [ESP+132],EAX
mov EAX,FS
mov dword [ESP+136],EAX
mov EAX,GS
mov dword [ESP+140],EAX
mov EAX,16
mov DS,EAX
mov ES,EAX
mov FS,EAX
mov GS,EAX
cld
mov EDI,ESP
call dispatch
mov ESP,EAX
jmp near restore_context
.globl x32_run_context
x32_run_context:
cli
mov EAX,16
mov SS,EAX
mov ESP,EDI
restore_context:
mov EAX,dword [ESP+128]
mov DS,EAX
mov EAX,dword [ESP+132]
mov ES,EAX
mov EAX,dword [ESP+136]
mov FS,EAX
mov EAX,dword [ESP+140]
mov GS,EAX
mov ECX,dword [ESP+8]
mov EDX,dword [ESP+12]
mov EBX,dword [ESP+16]
mov EBP,dword [ESP+24]
mov ESI,dword [ESP+28]
mov EDI,dword [ESP+32]
mov E8,dword [ESP+36]
mov E9,dword [ESP+40]
mov E10,dword [ESP+44]
mov E11,dword [ESP+48]
mov E12,dword [ESP+52]
mov E13,dword [ESP+56]
mov E14,dword [ESP+60]
mov E15,dword [ESP+64]
mov E16,dword [ESP+68]
mov E17,dword [ESP+72]
mov E18,dword [ESP+76]
mov E19,dword [ESP+80]
mov E20,dword [ESP+84]
mov E21,dword [ESP+88]
mov E22,dword [ESP+92]
mov E23,dword [ESP+96]
mov E24,dword [ESP+100]
mov E25,dword [ESP+104]
mov E26,dword [ESP+108]
mov E27,dword [ESP+112]
mov E28,dword [ESP+116]
mov E29,dword [ESP+120]
mov E30,dword [ESP+124]
mov EAX,dword [ESP+4]
add ESP,144
iret
