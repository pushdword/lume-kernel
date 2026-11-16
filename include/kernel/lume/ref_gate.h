#ifndef LUME_REF_GATE_H
#define LUME_REF_GATE_H
#include <stdbool.h>
#include <stdint.h>

#define LUME_REF_CLOSED (UINT32_C(1) << 31)
#define LUME_REF_GATE_INITIALIZER {LUME_REF_CLOSED}

struct lume_ref_gate
{
    uint32_t state;
};

/* Closed storage remains resident. Publication opens the gate after initializing
 * the payload. Retirement closes only a gate with no outstanding references. */
static inline bool lume_ref_get(struct lume_ref_gate *gate)
{
    uint32_t state = __atomic_load_n(&gate->state, __ATOMIC_ACQUIRE);
    do
    {
        if (state >= LUME_REF_CLOSED - 1)
            return false;
    } while (!__atomic_compare_exchange_n(&gate->state, &state, state + 1, true, __ATOMIC_ACQUIRE,
                                          __ATOMIC_RELAXED));
    return true;
}

static inline bool lume_ref_put(struct lume_ref_gate *gate)
{
    uint32_t state = __atomic_load_n(&gate->state, __ATOMIC_RELAXED);
    do
    {
        if (!(state & ~LUME_REF_CLOSED))
            return false;
    } while (!__atomic_compare_exchange_n(&gate->state, &state, state - 1, true, __ATOMIC_RELEASE,
                                          __ATOMIC_RELAXED));
    return true;
}

static inline bool lume_ref_close(struct lume_ref_gate *gate)
{
    uint32_t expected = 0;
    return __atomic_compare_exchange_n(&gate->state, &expected, LUME_REF_CLOSED, false,
                                       __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
}

static inline bool lume_ref_open(struct lume_ref_gate *gate)
{
    uint32_t expected = LUME_REF_CLOSED;
    return __atomic_compare_exchange_n(&gate->state, &expected, 0, false, __ATOMIC_RELEASE,
                                       __ATOMIC_RELAXED);
}

static inline bool lume_ref_closed(const struct lume_ref_gate *gate)
{
    return (__atomic_load_n(&gate->state, __ATOMIC_ACQUIRE) & LUME_REF_CLOSED) != 0;
}

/* Blocking admission preserves existing references. Payload retirement still
 * requires a closed gate with a zero reference count. */
static inline unsigned lume_ref_block_count(struct lume_ref_gate *gate)
{
    return __atomic_fetch_or(&gate->state, LUME_REF_CLOSED, __ATOMIC_ACQ_REL) & ~LUME_REF_CLOSED;
}

static inline void lume_ref_block(struct lume_ref_gate *gate)
{
    (void)lume_ref_block_count(gate);
}

static inline void lume_ref_reopen(struct lume_ref_gate *gate)
{
    __atomic_fetch_and(&gate->state, ~LUME_REF_CLOSED, __ATOMIC_RELEASE);
}

static inline unsigned lume_ref_count(const struct lume_ref_gate *gate)
{
    return __atomic_load_n(&gate->state, __ATOMIC_ACQUIRE) & ~LUME_REF_CLOSED;
}
#endif
