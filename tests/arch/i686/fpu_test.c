#include "lume/early.h"
#include "lume/context.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static bool unavailable;

static bool fpu_disabled(void)
{
    return unavailable;
}

static void fpu_enable(void)
{
    unavailable = false;
}

static void fpu_disable(void)
{
    unavailable = true;
}

#include "../../../arch/i686/context/fpu.h"

static void expect_physical_zero(void)
{
    _Alignas(16) unsigned char image[108];
    __asm__ volatile("fnsave %0" : "=m"(image) : : "memory");
    for (unsigned i = 28; i < sizeof(image); ++i)
        assert(!image[i]);
}

int main(void)
{
    const uint64_t expected = UINT64_C(0x3ff8000000000000);
    __asm__ volatile("fninit\nfldl %0" : : "m"(expected) : "st");
    unavailable = true;
    context_fpu_start();
    assert(!unavailable);
    expect_physical_zero();
    struct lume_context context;
    memset(&context, 0xa5, sizeof(context));
    unavailable = true;
    context_fpu_capture(&context);
    assert(!context.fpu_valid);
    for (unsigned i = 0; i < sizeof(context.vector); ++i)
        assert(!context.vector[i]);
    context_fpu_prepare(&context);
    assert(unavailable);
    uint32_t registers[15] = {[8] = 7,
                              [10] = LUME_USER_BASE + 4096,
                              [11] = 0x1b,
                              [12] = UINT32_MAX,
                              [13] = LUME_USER_LIMIT - 4096};
    registers[11] = 8;
    assert(!lume_i686_fpu_activate(registers) && unavailable);
    registers[11] = 0x1b;
    assert(lume_i686_fpu_activate(registers) && !unavailable);
    assert(registers[12] == ((UINT32_MAX & UINT32_C(0x240dd5)) | 0x202));
    assert(registers[14] == 0x23);
    assert(!lume_i686_fpu_activate(registers));
    context_fpu_capture(&context);
    assert(context.fpu_valid);
    uint16_t control, tags;
    memcpy(&control, context.vector, 2);
    memcpy(&tags, context.vector + 8, 2);
    assert(control == 0x37f && tags == UINT16_MAX);
    for (unsigned i = 28; i < sizeof(context.vector); ++i)
        assert(!context.vector[i]);
    __asm__ volatile("fldl %0" : : "m"(expected) : "st");
    context_fpu_capture(&context);
    assert(context.fpu_valid);
    unsigned char saved[112];
    memcpy(saved, context.vector, sizeof(saved));
    expect_physical_zero();
    assert(!memcmp(saved, context.vector, sizeof(saved)));
    unavailable = true;
    context_fpu_prepare(&context);
    assert(!unavailable);
    uint64_t observed;
    __asm__ volatile("fstpl %0" : "=m"(observed) : : "st");
    assert(observed == expected);
    context.fpu_valid = 0;
    context_fpu_prepare(&context);
    assert(unavailable && lume_i686_fpu_activate(registers));
    context_fpu_capture(&context);
    for (unsigned i = 28; i < sizeof(context.vector); ++i)
        assert(!context.vector[i]);
    puts("PASS_I686_FPU startup and post-save physical clearing, unused capture and x87 restore");
    return 0;
}
