#ifndef LUME_JIT_CALLS_H
#define LUME_JIT_CALLS_H

static struct lume_context *kernel_jit_scope(struct lume_context *frame, unsigned task)
{
    int64_t result = -LUME_EINVAL;
    if (!LUME_CONTEXT_REG(frame, 1) && !LUME_CONTEXT_REG(frame, 2))
    {
#if defined(__aarch64__) || defined(LUME_ARCH_AARCH64) || defined(LUME_ARCH_AMD64) ||              \
    defined(LUME_ARCH_I686)
        result = lume_mmu_jit_scope(task, LUME_CONTEXT_REG(frame, 0) == LUME_VM_JIT_BEGIN);
        if (result == -LUME_EAGAIN)
        {
            /* Ownership release wakes this task before the saved call is retried.
             * No scope depth changes until the address space is available. */
            tasks[task].context = *frame;
            LUME_CONTEXT_PC(&tasks[task].context) -= LUME_SYSCALL_INSTRUCTION_BYTES;
            task_memory_wait(task);
            sched_charge(&policy, lume_cpu_index(), now());
            return wait_ready_task();
        }
#else
        result = -LUME_ENOSYS;
#endif
    }
    LUME_CONTEXT_REG(frame, 0) = (uint64_t)result;
    return frame;
}

#endif
