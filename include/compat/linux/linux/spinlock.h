/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LUME_LINUX_SPINLOCK_H
#define LUME_LINUX_SPINLOCK_H
#include <linux/types.h>

typedef struct
{
    unsigned locked;
} spinlock_t;

unsigned long lume_linux_irq_save(void);
void lume_linux_irq_restore(unsigned long);
void lume_linux_relax(void);

static inline void spin_lock(spinlock_t *lock)
{
    while (__atomic_exchange_n(&lock->locked, 1u, __ATOMIC_ACQUIRE))
        lume_linux_relax();
}

static inline void spin_unlock(spinlock_t *lock)
{
    __atomic_store_n(&lock->locked, 0u, __ATOMIC_RELEASE);
}

#define spin_lock_irqsave(lock, flags)                                                             \
    do                                                                                             \
    {                                                                                              \
        (flags) = lume_linux_irq_save();                                                           \
        spin_lock(lock);                                                                           \
    } while (0)
#define spin_unlock_irqrestore(lock, flags)                                                        \
    do                                                                                             \
    {                                                                                              \
        spin_unlock(lock);                                                                         \
        lume_linux_irq_restore(flags);                                                             \
    } while (0)
#endif
