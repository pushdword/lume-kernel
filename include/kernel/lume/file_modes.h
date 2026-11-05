#ifndef LUME_FILE_MODES_H
#define LUME_FILE_MODES_H
#include "vfs.h"
void lume_file_modes_reset(void);
int64_t lume_file_mode_get(const char *, unsigned, bool);
int lume_file_mode_set(const char *, unsigned, bool);
int lume_file_mode_remove(const char *, bool);

struct lume_file_mode_change
{
    const char *path;
    unsigned mode;
};

/* UINT32_MAX removes an override. The batch owns no caller path strings. */
int lume_file_mode_batch(const struct lume_file_mode_change *, unsigned, bool);
int lume_file_mode_override(const char *, unsigned *, bool);
bool lume_file_mode_namespace_busy(const char *);
#endif
