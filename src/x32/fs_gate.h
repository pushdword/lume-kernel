#ifndef LUME_FS_GATE_H
#define LUME_FS_GATE_H
/* The scheduler lock protects nonblocking ownership of the FAT volume. A false result defers
 * the current trap until the gate is available. */
#ifdef LUME_KERNEL_PERF
int lume_fs_acquire(void);
#else
static inline int lume_fs_acquire(void)
{
    return 1;
}
#endif
#endif
