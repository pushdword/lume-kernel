#ifndef LUME_USER_IO_H
#define LUME_USER_IO_H
#include <stdbool.h>
#include <stdint.h>
struct lume_files;

/* Each operation invokes VFS once with a bounded kernel buffer. A failed read
 * copyout does not undo consumed input or the cursor advanced by that invocation. */
/* Transfers up to 512 bytes retain a private stack buffer across suspension.
 * Larger transfers borrow one of four fixed staging buffers. Nested transfers
 * beyond that pool allocate at most LUME_FILE_IO_MAX bytes until completion. */
int64_t lume_user_io_call(struct lume_files *, unsigned, uint64_t, uint64_t, uint64_t, bool, bool,
                          int64_t);
int64_t lume_user_io_vector_call(struct lume_files *, unsigned, uint64_t, uint64_t, unsigned, bool);
/* Native pipe operations use resident physical aliases while holding the lock.
 * A false result leaves the pipe and user buffer untouched. */
bool lume_user_io_pipe_try(struct lume_files *, unsigned, uint64_t, uint64_t, uint64_t, bool,
                           int64_t *);
#endif
