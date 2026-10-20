#ifndef LUME_FS_GATE_H
#define LUME_FS_GATE_H
/* Scheduler-lock protected, nonblocking ownership of the single FAT volume.
 * A false result defers the current trap; it is not a filesystem error. */
#ifdef LUME_KERNEL_PERF
int lume_fs_acquire(void);
#else
static inline int lume_fs_acquire(void)
{
    return 1;
}
#endif
#endif
