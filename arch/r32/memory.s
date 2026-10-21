# R32 soft ABI leaf memory routines. Word accesses require natural alignment.
.text
.globl memcpy
memcpy:
    mv t0,a0
.Lcopy_align:
    beq a2,zero,.Lcopy_done
    andi t1,a0,3
    beq t1,zero,.Lcopy_bulk
    lbu t2,0(a1)
    sb t2,0(a0)
    addi a0,a0,1
    addi a1,a1,1
    addi a2,a2,-1
    j .Lcopy_align
.Lcopy_bulk:
    andi t1,a1,3
    bne t1,zero,.Lcopy_packed_start
    li t1,16
.Lcopy_blocks:
    bltu a2,t1,.Lcopy_words_start
    lw t2,0(a1)
    lw t3,4(a1)
    lw t4,8(a1)
    lw t5,12(a1)
    sw t2,0(a0)
    sw t3,4(a0)
    sw t4,8(a0)
    sw t5,12(a0)
    addi a0,a0,16
    addi a1,a1,16
    addi a2,a2,-16
    j .Lcopy_blocks
.Lcopy_words_start:
    li t1,4
.Lcopy_words:
    bltu a2,t1,.Lcopy_bytes
    lw t2,0(a1)
    sw t2,0(a0)
    addi a0,a0,4
    addi a1,a1,4
    addi a2,a2,-4
    j .Lcopy_words
.Lcopy_packed_start:
    li a6,0
.Lcopy_packed:
    li t1,4
    bltu a2,t1,.Lcopy_bytes
    lbu t2,0(a1)
    lbu t3,1(a1)
    slli t3,t3,8
    or t2,t2,t3
    lbu t3,2(a1)
    slli t3,t3,16
    or t2,t2,t3
    lbu t3,3(a1)
    slli t3,t3,24
    or t2,t2,t3
    sw t2,0(a0)
    addi a0,a0,4
    addi a1,a1,4
    addi a2,a2,-4
    bne a6,zero,.Lcopy_bytes
    andi a3,a1,3
    slli a3,a3,3
    li a4,32
    sub a4,a4,a3
    srli a5,a1,2
    slli a5,a5,2
    li t1,8
    bltu a2,t1,.Lcopy_merge_done
    lw t4,0(a5)
.Lcopy_merge:
    lw t5,4(a5)
    srl t2,t4,a3
    sll t3,t5,a4
    or t2,t2,t3
    sw t2,0(a0)
    mv t4,t5
    addi a5,a5,4
    addi a0,a0,4
    addi a1,a1,4
    addi a2,a2,-4
    bgeu a2,t1,.Lcopy_merge
.Lcopy_merge_done:
    li a6,1
    j .Lcopy_packed
.Lcopy_bytes:
    beq a2,zero,.Lcopy_done
    lbu t2,0(a1)
    sb t2,0(a0)
    addi a0,a0,1
    addi a1,a1,1
    addi a2,a2,-1
    j .Lcopy_bytes
.Lcopy_done:
    mv a0,t0
    ret
.globl memset
memset:
    mv t0,a0
    andi a1,a1,255
    slli t1,a1,8
    or a1,a1,t1
    slli t1,a1,16
    or a1,a1,t1
.Lfill_align:
    beq a2,zero,.Lfill_done
    andi t1,a0,3
    beq t1,zero,.Lfill_bulk
    sb a1,0(a0)
    addi a0,a0,1
    addi a2,a2,-1
    j .Lfill_align
.Lfill_bulk:
    li t1,16
.Lfill_blocks:
    bltu a2,t1,.Lfill_words_start
    sw a1,0(a0)
    sw a1,4(a0)
    sw a1,8(a0)
    sw a1,12(a0)
    addi a0,a0,16
    addi a2,a2,-16
    j .Lfill_blocks
.Lfill_words_start:
    li t1,4
.Lfill_words:
    bltu a2,t1,.Lfill_bytes
    sw a1,0(a0)
    addi a0,a0,4
    addi a2,a2,-4
    j .Lfill_words
.Lfill_bytes:
    beq a2,zero,.Lfill_done
    sb a1,0(a0)
    addi a0,a0,1
    addi a2,a2,-1
    j .Lfill_bytes
.Lfill_done:
    mv a0,t0
    ret
.globl memmove
memmove:
    bltu a0,a1,memcpy
    sub t1,a0,a1
    bgeu t1,a2,memcpy
    beq a0,a1,.Lmove_done
    add t1,a0,a2
    add t2,a1,a2
.Lmove_align:
    beq a2,zero,.Lmove_done
    andi t3,t1,3
    beq t3,zero,.Lmove_bulk
    addi t1,t1,-1
    addi t2,t2,-1
    lbu t4,0(t2)
    sb t4,0(t1)
    addi a2,a2,-1
    j .Lmove_align
.Lmove_bulk:
    andi t3,t2,3
    bne t3,zero,.Lmove_packed_start
    li t3,16
.Lmove_blocks:
    bltu a2,t3,.Lmove_words_start
    addi t1,t1,-16
    addi t2,t2,-16
    lw t4,0(t2)
    lw t5,4(t2)
    lw t6,8(t2)
    lw a3,12(t2)
    sw t4,0(t1)
    sw t5,4(t1)
    sw t6,8(t1)
    sw a3,12(t1)
    addi a2,a2,-16
    j .Lmove_blocks
.Lmove_words_start:
    li t3,4
.Lmove_words:
    bltu a2,t3,.Lmove_bytes
    addi t1,t1,-4
    addi t2,t2,-4
    lw t4,0(t2)
    sw t4,0(t1)
    addi a2,a2,-4
    j .Lmove_words
.Lmove_packed_start:
    li a6,0
.Lmove_packed:
    li t3,4
    bltu a2,t3,.Lmove_bytes
    addi t1,t1,-4
    addi t2,t2,-4
    lbu t4,0(t2)
    lbu t5,1(t2)
    slli t5,t5,8
    or t4,t4,t5
    lbu t5,2(t2)
    slli t5,t5,16
    or t4,t4,t5
    lbu t5,3(t2)
    slli t5,t5,24
    or t4,t4,t5
    sw t4,0(t1)
    addi a2,a2,-4
    bne a6,zero,.Lmove_bytes
    andi a3,t2,3
    slli a3,a3,3
    li a4,32
    sub a4,a4,a3
    srli a5,t2,2
    slli a5,a5,2
    li t3,8
    bltu a2,t3,.Lmove_merge_done
    lw t6,0(a5)
.Lmove_merge:
    lw a7,-4(a5)
    srl t4,a7,a3
    sll t5,t6,a4
    or t4,t4,t5
    addi t1,t1,-4
    sw t4,0(t1)
    mv t6,a7
    addi a5,a5,-4
    addi t2,t2,-4
    addi a2,a2,-4
    bgeu a2,t3,.Lmove_merge
.Lmove_merge_done:
    li a6,1
    j .Lmove_packed
.Lmove_bytes:
    beq a2,zero,.Lmove_done
    addi t1,t1,-1
    addi t2,t2,-1
    lbu t4,0(t2)
    sb t4,0(t1)
    addi a2,a2,-1
    j .Lmove_bytes
.Lmove_done:
    ret
.globl memcmp
memcmp:
.Lcompare_align:
    beq a2,zero,.Lcompare_equal
    andi t0,a0,3
    beq t0,zero,.Lcompare_bulk
    lbu t1,0(a0)
    lbu t2,0(a1)
    bne t1,t2,.Lcompare_different
    addi a0,a0,1
    addi a1,a1,1
    addi a2,a2,-1
    j .Lcompare_align
.Lcompare_bulk:
    li t0,4
    andi t1,a1,3
    bne t1,zero,.Lcompare_packed_start
.Lcompare_words:
    bltu a2,t0,.Lcompare_bytes
    lw a3,0(a0)
    lw a4,0(a1)
    bne a3,a4,.Lcompare_word_different
    addi a0,a0,4
    addi a1,a1,4
    addi a2,a2,-4
    j .Lcompare_words
.Lcompare_packed_start:
    li a7,0
.Lcompare_packed:
    li t0,4
    bltu a2,t0,.Lcompare_bytes
    lw a3,0(a0)
    lbu a4,0(a1)
    lbu t1,1(a1)
    slli t1,t1,8
    or a4,a4,t1
    lbu t1,2(a1)
    slli t1,t1,16
    or a4,a4,t1
    lbu t1,3(a1)
    slli t1,t1,24
    or a4,a4,t1
    bne a3,a4,.Lcompare_word_different
    addi a0,a0,4
    addi a1,a1,4
    addi a2,a2,-4
    bne a7,zero,.Lcompare_bytes
    andi t3,a1,3
    slli t3,t3,3
    li t4,32
    sub t4,t4,t3
    srli t5,a1,2
    slli t5,t5,2
    li t0,8
    bltu a2,t0,.Lcompare_merge_done
    lw t1,0(t5)
.Lcompare_merge:
    lw a3,0(a0)
    lw t2,4(t5)
    srl a4,t1,t3
    sll t6,t2,t4
    or a4,a4,t6
    bne a3,a4,.Lcompare_word_different
    mv t1,t2
    addi t5,t5,4
    addi a0,a0,4
    addi a1,a1,4
    addi a2,a2,-4
    bgeu a2,t0,.Lcompare_merge
.Lcompare_merge_done:
    li a7,1
    j .Lcompare_packed
.Lcompare_word_different:
    andi t1,a3,255
    andi t2,a4,255
    bne t1,t2,.Lcompare_different
    srli a3,a3,8
    srli a4,a4,8
    j .Lcompare_word_different
.Lcompare_bytes:
    beq a2,zero,.Lcompare_equal
    lbu t1,0(a0)
    lbu t2,0(a1)
    bne t1,t2,.Lcompare_different
    addi a0,a0,1
    addi a1,a1,1
    addi a2,a2,-1
    j .Lcompare_bytes
.Lcompare_different:
    sub a0,t1,t2
    ret
.Lcompare_equal:
    li a0,0
    ret
