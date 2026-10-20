# R32 soft ABI leaf memory routines.
.text
.globl memcpy
memcpy:
 mv t0,a0
.Lcopy:
 beq a2,zero,.Lcopy_done
 lbu t1,0(a1)
 sb t1,0(a0)
 addi a0,a0,1
 addi a1,a1,1
 addi a2,a2,-1
 j .Lcopy
.Lcopy_done:
 mv a0,t0
 ret
.globl memset
memset:
 mv t0,a0
.Lfill:
 beq a2,zero,.Lfill_done
 sb a1,0(a0)
 addi a0,a0,1
 addi a2,a2,-1
 j .Lfill
.Lfill_done:
 mv a0,t0
 ret
.globl memmove
memmove:
 bltu a0,a1,memcpy
 beq a0,a1,.Lmove_equal
 add t0,a0,a2
 add t1,a1,a2
.Lmove_back:
 beq a2,zero,.Lmove_equal
 addi t0,t0,-1
 addi t1,t1,-1
 lbu t2,0(t1)
 sb t2,0(t0)
 addi a2,a2,-1
 j .Lmove_back
.Lmove_equal:
 ret
