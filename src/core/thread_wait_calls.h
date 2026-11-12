/* SPDX-License-Identifier: BSD-3-Clause */
static struct lume_vm_word_key thread_shared_keys[LUME_TASK_LIMIT];

static struct lume_context *thread_word_call(struct lume_context *frame, unsigned task,
                                             unsigned owner, unsigned op,
                                             const struct lume_thread_request *request)
{
    const struct lume_thread_request r = *request;
    int64_t result = -LUME_EINVAL;
    if ((r.address & 3) || !lume_user_buffer(r.address, 4, true))
        return kernel_buffer_failure(frame, -LUME_EFAULT);
    bool shared = op == LUME_THREAD_WAIT_SHARED || op == LUME_THREAD_WAKE_SHARED;
    struct lume_vm_word_key key = {0};
    if (shared)
    {
        result = lume_vm_shared_word_key(owner, r.address, &key);
        if (result)
            goto finished;
        result = -LUME_EINVAL;
    }
    if (op == LUME_THREAD_WAKE || op == LUME_THREAD_WAKE_SHARED)
    {
        result = 0;
        for (unsigned i = 0; i < LUME_TASK_LIMIT && (uint64_t)result < r.value; ++i)
        {
            bool matches = shared ? thread_shared_keys[i].object == key.object &&
                                        thread_shared_keys[i].offset == key.offset
                                  : !thread_shared_keys[i].object &&
                                        futex_address[i] == r.address && process_slot(i) == owner;
            if (states[i] == 4 && futex_address[i] && matches)
            {
                futex_address[i] = 0;
                thread_shared_keys[i] = (struct lume_vm_word_key){0};
                thread_realtime_wait[i] = false;
                states[i] = 1;
                LUME_CONTEXT_REG(&tasks[i].context, 0) = 0;
                sched_wake(&policy, i, unit, 0);
                ++result;
            }
        }
        if (result > 0)
            lume_smp_wake();
    }
    else if (__atomic_load_n((const uint32_t *)(uintptr_t)r.address, __ATOMIC_ACQUIRE) !=
             (uint32_t)r.value)
        result = -LUME_EAGAIN;
    else if (!thread_slots[task].thread && task_signal_pending(task))
        result = -LUME_EINTR;
    else if (op == LUME_THREAD_WAIT_REALTIME)
    {
        uint64_t monotonic = lume_clock_monotonic(), realtime;
        result = lume_clock_realtime_ms(monotonic, &realtime);
        if (result == -LUME_EOVERFLOW || (!result && realtime >= r.timeout_ms))
            result = -LUME_ETIMEDOUT;
        else if (!result)
        {
            futex_address[task] = r.address;
            thread_shared_keys[task] = key;
            thread_realtime_deadline[task] = r.timeout_ms;
            thread_realtime_wait[task] = true;
            tasks[task].context = *frame;
            states[task] = 4;
            wake_at[task] = UINT64_MAX;
            sched_charge(&policy, lume_cpu_index(), now());
            return wait_ready_task();
        }
    }
    else if (!r.timeout_ms)
        result = -LUME_ETIMEDOUT;
    else
    {
        uint64_t monotonic = r.timeout_ms == UINT64_MAX ? 0 : lume_clock_monotonic();
        if (r.timeout_ms == UINT64_MAX || r.timeout_ms < UINT64_MAX - monotonic)
        {
            futex_address[task] = r.address;
            thread_shared_keys[task] = key;
            thread_realtime_wait[task] = false;
            tasks[task].context = *frame;
            states[task] = 4;
            wake_at[task] = r.timeout_ms == UINT64_MAX ? UINT64_MAX : monotonic + r.timeout_ms;
            sched_charge(&policy, lume_cpu_index(), now());
            return wait_ready_task();
        }
    }
finished:
    LUME_CONTEXT_REG(frame, 0) = (uint64_t)result;
    return frame;
}
