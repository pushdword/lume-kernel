#ifndef LUME_IO_REDIRECT_H
#define LUME_IO_REDIRECT_H
#include <stdint.h>

/* Internal continuations are consumed before publishing a syscall result. */
#define LUME_IO_RETRY_NETWORK INT64_MIN
#define LUME_IO_RETRY_STORAGE (INT64_MIN + 1)

#endif
