#ifndef LUME_IO_REDIRECT_H
#define LUME_IO_REDIRECT_H
#include <stdint.h>

/* Internal continuations are consumed before publishing a syscall result. */
#define LUME_IO_RETRY_NETWORK INT64_MIN
#define LUME_IO_RETRY_STORAGE (INT64_MIN + 1)
#define LUME_IO_WAIT_FIFO (INT64_MIN + 2)
#define LUME_IO_PIPE_NO_READERS (INT64_MIN + 3)

#endif
