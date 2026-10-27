#ifndef LUME_TICKET_LOCK_H
#define LUME_TICKET_LOCK_H
#include <stdbool.h>
#include <stdint.h>

/* Every reserved ticket must be served. The outstanding ticket count must stay
 * below UINT32_MAX so unsigned subtraction remains valid across wrap. */
struct lume_ticket_lock
{
    _Alignas(64) uint32_t next;
    _Alignas(64) uint32_t serving;
};

static inline uint32_t lume_ticket_reserve(struct lume_ticket_lock *lock)
{
    return __atomic_fetch_add(&lock->next, 1, __ATOMIC_RELAXED);
}

static inline bool lume_ticket_ready(const struct lume_ticket_lock *lock, uint32_t ticket)
{
    return __atomic_load_n(&lock->serving, __ATOMIC_ACQUIRE) == ticket;
}

static inline void lume_ticket_release(struct lume_ticket_lock *lock, uint32_t ticket)
{
    __atomic_store_n(&lock->serving, ticket + UINT32_C(1), __ATOMIC_RELEASE);
}

/* The caller owns the serving ticket while testing for queued successors. */
static inline bool lume_ticket_contended(const struct lume_ticket_lock *lock)
{
    uint32_t next = __atomic_load_n(&lock->next, __ATOMIC_RELAXED);
    uint32_t serving = __atomic_load_n(&lock->serving, __ATOMIC_RELAXED);
    return (uint32_t)(next - serving) > 1;
}
#endif
