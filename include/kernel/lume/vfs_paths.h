#ifndef LUME_VFS_PATHS_H
#define LUME_VFS_PATHS_H
#include "lume/files.h"
#include <stdint.h>

struct lume_files;
struct lume_file_mode_request;
const char *lume_files_cwd(const struct lume_files *);
int lume_files_copy_cwd(struct lume_files *, const struct lume_files *);
int lume_vfs_canonical_long(struct lume_files *, const char *, char[LUME_FILES_PATH_MAX]);
int lume_vfs_canonical_at(struct lume_files *, int64_t, const char *, char[LUME_FILES_PATH_MAX]);
int lume_vfs_fd_path(struct lume_files *, int64_t, char[LUME_FILES_PATH_MAX]);
int64_t lume_vfs_getdents_long(struct lume_files *, uint64_t, void *, uint64_t);
int64_t lume_vfs_file_mode_long(struct lume_files *, unsigned, struct lume_file_mode_request *,
                                const char *);
#endif
