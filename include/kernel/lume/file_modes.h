#ifndef LUME_FILE_MODES_H
#define LUME_FILE_MODES_H
#include "vfs.h"
void lume_file_modes_reset(void);
int64_t lume_file_mode_get(const char *, unsigned, bool);
int lume_file_mode_set(const char *, unsigned, bool);
int lume_file_mode_remove(const char *, bool);
#endif
