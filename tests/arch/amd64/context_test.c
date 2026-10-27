#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../../arch/amd64/context/context.c"

unsigned char __trap_top[1], __fault_top[1], __secondary_stacks[1];
uintptr_t lume_isr_table[53];
static bool held;
static unsigned locks, unlocks, flushes, eois, pic, activity_calls, ticks, devices, faults;
static struct lume_context replacement;

unsigned lume_cpu_index(void)
{
    return 0;
}

bool lume_smp_owned(void)
{
    return held;
}

void lume_smp_lock(void)
{
    assert(!held);
    held = true;
    ++locks;
}

void lume_smp_unlock(void)
{
    assert(held);
    held = false;
    ++unlocks;
}

void lume_smp_tlb_service(void)
{
    ++flushes;
}

void lume_apic_eoi(void)
{
    ++eois;
}

unsigned lume_activity_enter(unsigned kind)
{
    assert(held && kind == LUME_ACTIVITY_IRQ);
    ++activity_calls;
    return 0;
}

void lume_activity_leave(unsigned previous)
{
    assert(held && !previous);
}

bool lume_irq_dispatch(unsigned vector)
{
    assert(held && vector >= 32 && vector < 48);
    ++pic;
    return true;
}

void lume_task_trap_enter(uint64_t at)
{
    (void)at;
}

void lume_task_account_enter(const struct lume_context *frame)
{
    assert(held && frame);
}

void lume_task_account_leave(const struct lume_context *frame)
{
    assert(held && frame);
}

bool lume_tasks_active(void)
{
    return true;
}

struct lume_context *lume_tasks_sync(struct lume_context *frame, uint64_t cause, uint64_t address)
{
    assert(held && frame && cause == 13 && !address);
    ++faults;
    return &replacement;
}

struct lume_context *lume_tasks_irq(struct lume_context *frame)
{
    assert(held);
    ++ticks;
    return frame;
}

struct lume_context *lume_tasks_device_irq(struct lume_context *frame)
{
    assert(held);
    ++devices;
    return frame;
}

struct lume_context *lume_tasks_deliver_signals(struct lume_context *frame)
{
    assert(held);
    return frame;
}

uint64_t lume_q35_counter_value(uint64_t raw)
{
    return raw;
}

void lume_value(const char *name, uint64_t value)
{
    (void)name;
    (void)value;
    abort();
}

_Noreturn void lume_fail(const char *why)
{
    fprintf(stderr, "Unexpected context failure: %s\n", why);
    abort();
}

static void reset(void)
{
    held = false;
    locks = unlocks = flushes = eois = pic = activity_calls = ticks = devices = faults = 0;
}

int main(void)
{
    const struct lume_context good = {.pc = LUME_USER_BASE + 4096,
                                      .sp = LUME_USER_STACK + 4096,
                                      .status = LUME_CONTEXT_USER_STATUS};
    struct lume_context frame = good;
    reset();
    assert(lume_amd64_exception(&frame, LUME_APIC_TLB, 0) == &frame);
    assert(flushes == 1 && eois == 1 && !locks && !activity_calls && !faults);
    assert(!memcmp(&frame, &good, sizeof(frame)));
    reset();
    frame.status = 1;
    held = true;
    assert(lume_amd64_exception(&frame, LUME_APIC_TLB, 0) == &frame);
    assert(held && flushes == 1 && eois == 1 && !locks && !unlocks && !activity_calls);
    for (unsigned kind = 0; kind < 3; ++kind)
    {
        reset();
        frame = good;
        if (!kind)
            frame.sp = 0;
        else if (kind == 1)
            frame.pc = LUME_USER_LIMIT;
        else
            frame.tls = LUME_USER_LIMIT;
        assert(lume_amd64_exception(&frame, LUME_APIC_TLB, 0) == &replacement);
        assert(flushes == 1 && eois == 1 && faults == 1 && locks == 1 && unlocks == 1);
        assert(!activity_calls);
    }
    const unsigned vectors[] = {32, LUME_APIC_TIMER, LUME_APIC_WAKE};
    for (unsigned i = 0; i < 3; ++i)
    {
        reset();
        frame = good;
        frame.sp = 0;
        assert(lume_amd64_exception(&frame, vectors[i], 0) == &replacement);
        assert(faults == 1 && activity_calls == 1 && locks == 1 && unlocks == 1);
        assert(i ? eois == 1 && !pic : pic == 1 && !eois);
    }
    reset();
    frame = good;
    assert(lume_amd64_exception(&frame, LUME_APIC_TIMER, 0) == &frame);
    assert(ticks == 1 && eois == 1 && activity_calls == 1 && !faults);
    reset();
    assert(lume_amd64_exception(&frame, LUME_APIC_WAKE, 0) == &frame);
    assert(devices == 1 && eois == 1 && activity_calls == 1 && !faults);
    puts(
        "AMD64_CONTEXT_PASS raw TLB acknowledgements, held-lock entry, invalid user contexts and IRQ completion");
    return 0;
}
