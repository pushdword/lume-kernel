#include "lume/context.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    struct lume_context frame = {
        .pc = 0x40001002, .syscall_pc = 0x40001002, .raw = {[7] = 0x5ffff000, [5] = 0x12345678}};
    frame.reg[0] = UINT64_C(0x1122334455667788);
    lume_context_finish_syscall(&frame);
    assert(frame.raw[7] == 0x55667788 && frame.raw[5] == 0x11223344);
    assert(!frame.syscall_pc);
    frame.raw[7] = 0x5ffff000;
    frame.raw[5] = 0x12345678;
    frame.syscall_pc = (uint32_t)frame.pc;
    frame.pc -= LUME_SYSCALL_INSTRUCTION_BYTES;
    frame.reg[0] = 0;
    lume_context_finish_syscall(&frame);
    assert(frame.raw[7] == 0x5ffff000 && frame.raw[5] == 0x12345678);
    assert(!frame.syscall_pc);
    frame.pc += LUME_SYSCALL_INSTRUCTION_BYTES;
    frame.syscall_pc = (uint32_t)frame.pc;
    frame.reg[0] = (uint64_t)-14;
    lume_context_finish_syscall(&frame);
    assert(frame.raw[7] == UINT32_C(0xfffffff2) && frame.raw[5] == UINT32_MAX);
    puts("PASS i686 64-bit syscall result, signed errors and blocked-call replay request");
    return 0;
}
