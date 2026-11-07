#ifndef LUME_FILE_IO_CALL_H
#define LUME_FILE_IO_CALL_H
#include <stdint.h>
struct lume_files;
int64_t lume_file_io_call(struct lume_files *, unsigned, uint64_t, uint64_t, uint64_t);
int64_t lume_file_lock_call(struct lume_files *, uint64_t, uint64_t, uint64_t);
#endif
