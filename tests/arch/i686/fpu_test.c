#include "lume/early.h"
#include "lume/context.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static bool unavailable;
static bool fpu_fxsr;

static void fpu_configure(void)
{
}

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

static void expect_vector_zero(void)
{
    _Alignas(16) unsigned char image[512];
    __asm__ volatile("fxsave %0" : "=m"(image) : : "memory");
    for (unsigned i = 160; i < 288; ++i)
        assert(!image[i]);
    uint32_t control;
    memcpy(&control, image + 24, sizeof(control));
    assert(control == 0x1f80);
}

static void vector_context(void)
{
    fpu_fxsr = true;
    context_fpu_start();
    expect_vector_zero();
    _Alignas(16) uint32_t expected[32];
    for (unsigned i = 0; i < 32; ++i)
        expected[i] = UINT32_C(0x10203040) + i;
    const uint32_t control = UINT32_C(0x3f80);
    __asm__ volatile("ldmxcsr %0\nmovdqu 0(%1), %%xmm0\nmovdqu 16(%1), %%xmm1\n"
                     "movdqu 32(%1), %%xmm2\nmovdqu 48(%1), %%xmm3\n"
                     "movdqu 64(%1), %%xmm4\nmovdqu 80(%1), %%xmm5\n"
                     "movdqu 96(%1), %%xmm6\nmovdqu 112(%1), %%xmm7"
                     :
                     : "m"(control), "r"(expected)
                     : "memory");
    struct lume_context frame;
    context_fpu_capture(&frame);
    expect_vector_zero();
    assert(frame.fpu_valid && !memcmp(frame.vector + 160, expected, sizeof(expected)));
    uint32_t saved_control;
    memcpy(&saved_control, frame.vector + 24, sizeof(saved_control));
    assert(saved_control == control);
    context_fpu_prepare(&frame);
    _Alignas(16) unsigned char observed[512];
    __asm__ volatile("fxsave %0" : "=m"(observed) : : "memory");
    assert(!memcmp(observed + 160, expected, sizeof(expected)));
    memcpy(&saved_control, observed + 24, sizeof(saved_control));
    assert(saved_control == control);
    frame.fpu_valid = 0;
    context_fpu_prepare(&frame);
    uint32_t registers[15] = {
        [8] = 7, [10] = LUME_USER_BASE + 4096, [11] = 0x1b, [13] = LUME_USER_LIMIT - 4096};
    assert(unavailable && lume_i686_fpu_activate(registers));
    expect_vector_zero();
}

int main(void)
{
    uint32_t a = 1, b, c, d;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d));
    bool vectors = (d & (UINT32_C(3) << 24)) == (UINT32_C(3) << 24);
    _Alignas(16) unsigned char original[512];
    if (vectors)
        __asm__ volatile("fxsave %0" : "=m"(original) : : "memory");
    const uint64_t expected = UINT64_C(0x3ff8000000000000);
    __asm__ volatile("fninit\nfldl %0" : : "m"(expected) : "memory");
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
    __asm__ volatile("fldl %0" : : "m"(expected) : "memory");
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
    __asm__ volatile("fstpl %0" : "=m"(observed) : : "memory");
    assert(observed == expected);
    context.fpu_valid = 0;
    context_fpu_prepare(&context);
    assert(unavailable && lume_i686_fpu_activate(registers));
    context_fpu_capture(&context);
    for (unsigned i = 28; i < sizeof(context.vector); ++i)
        assert(!context.vector[i]);
    if (vectors)
    {
        vector_context();
        __asm__ volatile("fxrstor %0" : : "m"(original) : "memory");
    }
    puts("PASS_I686_FPU x87 and SSE capture, physical clearing, restore and lazy activation");
    return 0;
}
