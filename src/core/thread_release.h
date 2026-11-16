#ifndef LUME_THREAD_RELEASE_H
#define LUME_THREAD_RELEASE_H

static void thread_drop_owned(void *context)
{
    unsigned task = *(unsigned *)context;
    vm_fault_wait[task] = vm_fault_ready[task] = false;
    kernel_cancel(task);
    poll_cancel(task);
    service_wait_cancel(task);
    service_bus_release(task);
    unsigned owner = process_slot(task);
#if defined(__aarch64__) || defined(LUME_ARCH_AARCH64) || defined(LUME_ARCH_AMD64) ||              \
    defined(LUME_ARCH_I686)
    if (!thread_pages[task])
        lume_vm_thread_release(owner, task);
    else
#endif
    {
        uint64_t base = LUME_THREAD_BASE + (uint64_t)task * 0x100000 + 4096;
        for (unsigned first = 0; first < thread_pages[task];)
        {
            uint64_t addresses[LUME_MMU_BATCH_PAGES];
            unsigned count = thread_pages[task] - first, mapped = 0;
            if (count > LUME_MMU_BATCH_PAGES)
                count = LUME_MMU_BATCH_PAGES;
            for (unsigned i = first; i < first + count; ++i)
                if (tasks[task].stack[i])
                    addresses[mapped++] = base + (uint64_t)i * 4096;
            if (mapped && !lume_mmu_unmap_user_batch(owner, addresses, mapped))
                lume_fail("thread stack mapping release");
            for (unsigned i = first; i < first + count; ++i)
            {
                if (tasks[task].stack[i] && !lume_page_free(tasks[task].stack[i]))
                    lume_fail("thread stack release");
                tasks[task].stack[i] = 0;
            }
            first += count;
            if (count == LUME_MMU_BATCH_PAGES)
                lume_worker_checkpoint(65536);
        }
    }
    thread_pages[task] = 0;
    signal_thread_reset(task);
    lume_mmu_release_user(task);
    task_state_set(task, 0);
    lume_wait_cancel(pipe_queues[task], task);
    pipe_queues[task] = 0;
    pipe_wait[task] = input_wait[task] = kernel_wait[task] = false;
    observers[task] = false;
    wait_target[task] = 0;
    futex_address[task] = thread_waiting[task] = 0;
    thread_realtime_wait[task] = false;
    thread_exit_pending[task] = false;
}

static bool thread_drop(unsigned task)
{
#if defined(__aarch64__) || defined(LUME_ARCH_AARCH64) || defined(LUME_ARCH_AMD64) ||              \
    defined(LUME_ARCH_I686)
    return lume_mmu_owned(process_slot(task), thread_drop_owned, &task);
#else
    thread_drop_owned(&task);
    return true;
#endif
}

static void thread_drop_required(unsigned task)
{
    if (!thread_drop(task))
        lume_fail("thread retirement requires resource admission");
}

#endif
