#ifndef LUME_OWNER_RETIREMENT_H
#define LUME_OWNER_RETIREMENT_H
#include <stdbool.h>
#include <stdint.h>
#ifndef LUME_TASK_LIMIT
#include "lume/task_limits.h"
#endif

#define LUME_RETIREMENT_WORDS ((LUME_TASK_LIMIT + 31u) / 32u)

struct lume_owner_retirements
{
    uint32_t words[LUME_RETIREMENT_WORDS];
};

/* Producers publish owner retirement without reading domain-owned objects.
 * The domain consumes these requests before admitting a reused owner slot. */
static inline bool lume_owner_retire(struct lume_owner_retirements *requests, unsigned owner)
{
    if (!owner || owner > LUME_TASK_LIMIT)
        return false;
    __atomic_fetch_or(requests->words + (owner - 1) / 32u, UINT32_C(1) << ((owner - 1) % 32u),
                      __ATOMIC_RELEASE);
    return true;
}

static inline bool lume_owner_retired(const struct lume_owner_retirements *requests, unsigned owner)
{
    return owner && owner <= LUME_TASK_LIMIT &&
           (__atomic_load_n(requests->words + (owner - 1) / 32u, __ATOMIC_ACQUIRE) &
            (UINT32_C(1) << ((owner - 1) % 32u)));
}

static inline uint32_t lume_owner_retire_take(struct lume_owner_retirements *requests,
                                              unsigned word)
{
    if (!__atomic_load_n(requests->words + word, __ATOMIC_ACQUIRE))
        return 0;
    return __atomic_exchange_n(requests->words + word, 0, __ATOMIC_ACQ_REL);
}

static inline bool lume_owner_retire_pending(const struct lume_owner_retirements *requests)
{
    for (unsigned word = 0; word < LUME_RETIREMENT_WORDS; ++word)
        if (__atomic_load_n(requests->words + word, __ATOMIC_ACQUIRE))
            return true;
    return false;
}
#endif
