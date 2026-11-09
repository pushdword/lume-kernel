#ifndef LUME_I686_CONTEXT_FPU_H
#define LUME_I686_CONTEXT_FPU_H

static void context_fpu_clean(void)
{
    /* Clear every physical register before an empty x87 stack can become lazy. */
    __asm__ volatile("fninit\nfldz\nfldz\nfldz\nfldz\nfldz\nfldz\nfldz\nfldz\nfninit"
                     :
                     :
                     : "memory");
}

static void context_fpu_start(void)
{
    fpu_enable();
    context_fpu_clean();
}

static void context_fpu_capture(struct lume_context *context)
{
    context->fpu_valid = !fpu_disabled();
    if (context->fpu_valid)
    {
        memset(context->vector + 108, 0, sizeof(context->vector) - 108);
        __asm__ volatile("fnsave %0" : "=m"(context->vector) : : "memory");
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
