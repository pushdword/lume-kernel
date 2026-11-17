/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LUME_LINUX_BARRIER_H
#define LUME_LINUX_BARRIER_H
#define smp_store_release(pointer, value) __atomic_store_n(pointer, value, __ATOMIC_RELEASE)
#define smp_load_acquire(pointer) __atomic_load_n(pointer, __ATOMIC_ACQUIRE)
#endif
