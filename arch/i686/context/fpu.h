#ifndef LUME_I686_CONTEXT_FPU_H
#define LUME_I686_CONTEXT_FPU_H

static void context_fpu_clean(void)
{
    /* Clear every physical register before an empty x87 stack can become lazy. */
    __asm__ volatile("fninit\nfldz\nfldz\nfldz\nfldz\nfldz\nfldz\nfldz\nfldz\nfninit"
                     :
                     :
                     : "memory");
    if (fpu_fxsr)
    {
        const uint32_t control = UINT32_C(0x1f80);
        __asm__ volatile("ldmxcsr %0\nxorps %%xmm0, %%xmm0\nxorps %%xmm1, %%xmm1\n"
                         "xorps %%xmm2, %%xmm2\nxorps %%xmm3, %%xmm3\n"
                         "xorps %%xmm4, %%xmm4\nxorps %%xmm5, %%xmm5\n"
                         "xorps %%xmm6, %%xmm6\nxorps %%xmm7, %%xmm7"
                         :
                         : "m"(control)
                         : "memory");
    }
}

static void context_fpu_start(void)
{
    fpu_configure();
    fpu_enable();
    context_fpu_clean();
}

static void context_fpu_capture(struct lume_context *context)
{
    context->fpu_valid = !fpu_disabled();
    if (context->fpu_valid)
    {
        if (fpu_fxsr)
        {
            __asm__ volatile("fxsave %0" : "=m"(context->vector) : : "memory");
            context->vector[5] = 0;
            context->vector[14] = context->vector[15] = 0;
            context->vector[22] = context->vector[23] = 0;
            for (unsigned i = 0; i < 8; ++i)
                memset(context->vector + 42 + i * 16, 0, 6);
            memset(context->vector + 288, 0, sizeof(context->vector) - 288);
        }
        else
        {
            __asm__ volatile("fnsave %0" : "=m"(context->vector) : : "memory");
            memset(context->vector + 108, 0, sizeof(context->vector) - 108);
        }
        context_fpu_clean();
    }
    else
        memset(context->vector, 0, sizeof(context->vector));
}

static void context_fpu_prepare(const struct lume_context *context)
{
    if (context->fpu_valid)
    {
        fpu_enable();
        if (fpu_fxsr)
            __asm__ volatile("fxrstor %0" : : "m"(context->vector) : "memory");
        else
            __asm__ volatile("frstor %0" : : "m"(context->vector) : "memory");
    }
    else
        fpu_disable();
}

bool lume_i686_fpu_activate(uint32_t *registers)
{
    if (registers[8] != 7 || registers[11] != 0x1b || registers[10] < LUME_USER_BASE ||
        registers[10] >= LUME_USER_LIMIT || registers[13] < LUME_USER_BASE ||
        registers[13] >= LUME_USER_LIMIT || !fpu_disabled())
        return false;
    fpu_enable();
    context_fpu_clean();
    registers[12] = (registers[12] & UINT32_C(0x240dd5)) | 0x202;
    registers[14] = 0x23;
    return true;
}
#endif
