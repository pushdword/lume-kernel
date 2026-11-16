#ifndef LUME_FD_PUBLICATION_H
#define LUME_FD_PUBLICATION_H
#define FD_PUBLICATION_BUCKETS 64u

static struct fd_lock
{
    _Alignas(64) uint32_t next;
    uint32_t serving;
} fd_publication[FD_PUBLICATION_BUCKETS];

struct fd_guard
{
    struct fd_lock *lock;
    uint32_t ticket;
};

/* Publication and reference acquisition cannot suspend or enter scheduler services
 * while holding this lock. The caller retains the descriptor table's lifetime. */
static struct fd_guard fd_enter(const struct lume_files *files)
{
    uintptr_t identity = (uintptr_t)files;
    unsigned bucket = (unsigned)((identity >> 6) ^ (identity >> 14)) % FD_PUBLICATION_BUCKETS;
    struct fd_guard guard = {.lock = fd_publication + bucket};
    guard.ticket = __atomic_fetch_add(&guard.lock->next, 1, __ATOMIC_RELAXED);
    while (__atomic_load_n(&guard.lock->serving, __ATOMIC_ACQUIRE) != guard.ticket)
        __atomic_signal_fence(__ATOMIC_ACQUIRE);
    return guard;
}

static void fd_leave(struct fd_guard guard)
{
    __atomic_store_n(&guard.lock->serving, guard.ticket + 1, __ATOMIC_RELEASE);
}
#endif
