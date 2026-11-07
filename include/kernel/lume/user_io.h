#ifndef LUME_USER_IO_H
#define LUME_USER_IO_H
#include <stdbool.h>
#include <stdint.h>
struct lume_files;

/* Each operation invokes VFS once with a bounded kernel buffer. A failed read
 * copyout does not undo consumed input or the cursor advanced by that invocation. */
int64_t lume_user_io_call(struct lume_files *, unsigned, uint64_t, uint64_t, uint64_t, bool, bool,
                          int64_t);
#endif
