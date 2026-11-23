#ifndef LUME_FILES_CALL_H
#define LUME_FILES_CALL_H
#include <stdint.h>
struct lume_files;
struct lume_fifo_open;
int64_t lume_files_call(struct lume_files *, uint64_t, uint64_t, uint64_t, struct lume_fifo_open *);
#endif
