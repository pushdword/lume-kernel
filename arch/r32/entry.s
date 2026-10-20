# R32 trap/context ABI: 32 GPRs, 16 reserved arch bytes, 32-byte common frame.
.text
.globl _start
_start:
csrw status,r0
csrw scratch,r0
li sp,0x13ed8000
la r5,__bss_start
la r6,__bss_end
.Lzero_bss:
bgeu r5,r6,.Lkernel_enter
sw r0,0(r5)
addi r5,r5,4
j .Lzero_bss
.Lkernel_enter:
la r5,vector_entry
csrw tvec,r5
call kernel_main
wfi
.globl secondary_entry
secondary_entry:
break
.globl idle_loop
idle_loop:
wfi
j idle_loop
.globl r32_cas
r32_cas:
lr.w r5,(a0)
bne r5,a1,.Lcas_done
sc.w r6,a0,a2
bne r6,r0,r32_cas
.Lcas_done:
mv a0,r5
ret
.globl vector_entry
vector_entry:
csrrw sp,scratch,sp
bne sp,r0,.Ltrap_have_stack
csrrw sp,scratch,sp
.Ltrap_have_stack:
addi sp,sp,-176
sw r0,0(sp)
sw r1,4(sp)
sw r3,12(sp)
sw r4,16(sp)
sw r5,20(sp)
sw r6,24(sp)
sw r7,28(sp)
sw r8,32(sp)
sw r9,36(sp)
sw r10,40(sp)
sw r11,44(sp)
sw r12,48(sp)
sw r13,52(sp)
sw r14,56(sp)
sw r15,60(sp)
sw r16,64(sp)
sw r17,68(sp)
sw r18,72(sp)
sw r19,76(sp)
sw r20,80(sp)
sw r21,84(sp)
sw r22,88(sp)
sw r23,92(sp)
sw r24,96(sp)
sw r25,100(sp)
sw r26,104(sp)
sw r27,108(sp)
sw r28,112(sp)
sw r29,116(sp)
sw r30,120(sp)
sw r31,124(sp)
csrr r5,status
sw r5,152(sp)
andi r6,r5,4
beq r6,r0,.Ltrap_from_user
addi r6,sp,176
sw r0,164(sp)
j .Ltrap_sp
.Ltrap_from_user:
csrr r6,scratch
li r7,3
sw r7,164(sp)
.Ltrap_sp:
sw r6,8(sp)
sw r6,156(sp)
csrw scratch,r0
sw r0,148(sp)
sw r0,160(sp)
csrr r5,badaddr
sw r5,128(sp)
csrr r5,cause
sw r5,132(sp)
sw r0,136(sp)
sw r0,140(sp)
sw r0,172(sp)
csrr r6,epc
sw r6,144(sp)
li r7,0x80000000
and r7,r5,r7
beq r7,r0,.Lsync_trap
andi r5,r5,31
addi r5,r5,32
j .Lvector_done
.Lsync_trap:
li r7,7
bne r5,r7,.Lnot_syscall
li r5,128
addi r6,r6,4
sw r6,144(sp)
j .Lvector_done
.Lnot_syscall:
li r7,10
bltu r5,r7,.Lnot_page
li r7,13
bgeu r5,r7,.Lnot_page
li r7,10
bne r5,r7,.Lnot_fetch
li r6,4
sw r6,172(sp)
.Lnot_fetch:
li r7,12
bne r5,r7,.Lnot_store
li r6,2
sw r6,172(sp)
.Lnot_store:
li r5,14
j .Lvector_done
.Lnot_page:
li r5,13
.Lvector_done:
sw r5,168(sp)
mv a0,sp
call dispatch
j r32_run_context
.globl r32_run_context
r32_run_context:
mv r31,a0
lw r5,144(r31)
csrw epc,r5
lw r5,152(r31)
csrw status,r5
andi r5,r5,4
bne r5,r0,.Lrestore_kernel
li r5,0x13ed8000
csrw scratch,r5
j .Lrestore_regs
.Lrestore_kernel:
csrw scratch,r0
.Lrestore_regs:
lw r1,4(r31)
lw r3,12(r31)
lw r4,16(r31)
lw r5,20(r31)
lw r6,24(r31)
lw r7,28(r31)
lw r8,32(r31)
lw r9,36(r31)
lw r10,40(r31)
lw r11,44(r31)
lw r12,48(r31)
lw r13,52(r31)
lw r14,56(r31)
lw r15,60(r31)
lw r16,64(r31)
lw r17,68(r31)
lw r18,72(r31)
lw r19,76(r31)
lw r20,80(r31)
lw r21,84(r31)
lw r22,88(r31)
lw r23,92(r31)
lw r24,96(r31)
lw r25,100(r31)
lw r26,104(r31)
lw r27,108(r31)
lw r28,112(r31)
lw r29,116(r31)
lw r30,120(r31)
lw sp,156(r31)
lw r31,124(r31)
sret
.globl r32_kernel_switch
r32_kernel_switch:
addi sp,sp,-64
sw r1,0(sp)
sw r8,4(sp)
sw r9,8(sp)
sw r18,12(sp)
sw r19,16(sp)
sw r20,20(sp)
sw r21,24(sp)
sw r22,28(sp)
sw r23,32(sp)
sw r24,36(sp)
sw r25,40(sp)
sw r26,44(sp)
sw r27,48(sp)
sw sp,0(a0)
lw sp,0(a1)
lw r1,0(sp)
lw r8,4(sp)
lw r9,8(sp)
lw r18,12(sp)
lw r19,16(sp)
lw r20,20(sp)
lw r21,24(sp)
lw r22,28(sp)
lw r23,32(sp)
lw r24,36(sp)
lw r25,40(sp)
lw r26,44(sp)
lw r27,48(sp)
addi sp,sp,64
ret
