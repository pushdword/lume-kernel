#ifndef LUME_VFS_H
#define LUME_VFS_H
#include <lume/abi.h>
#include <stdbool.h>
#include <stddef.h>
#include "fat.h"
#include "storage.h"
#include <fd.h>

struct lume_fd
{
    int object;
    uint32_t flags;
};

struct lume_files
{
    char cwd[LUME_PATH_MAX];
    struct lume_fd fd[LUME_FD_MAX];
};

void lume_vfs_init(const void *shell, size_t bytes);
void lume_vfs_program_buffer(void *, size_t);
void lume_vfs_program_allocator(void *(*)(size_t));
bool lume_vfs_node_buffer(void *, size_t);
void lume_files_init(struct lume_files *);
void lume_files_clone(struct lume_files *, const struct lume_files *);
void lume_files_inherit(struct lume_files *, const struct lume_files *);
void lume_files_release(struct lume_files *);
int64_t lume_vfs_open(struct lume_files *, const char *, uint64_t flags);
int64_t lume_vfs_open_compat(struct lume_files *, const char *, uint64_t flags);
int64_t lume_vfs_remove_compat(struct lume_files *, const char *, bool);
int64_t lume_vfs_stat_inode(uint64_t, struct lume_stat *);
int64_t lume_vfs_close(struct lume_files *, uint64_t fd);
int64_t lume_vfs_read(struct lume_files *, uint64_t fd, void *, uint64_t bytes);
int64_t lume_vfs_write(struct lume_files *, uint64_t fd, const void *, uint64_t bytes);
int64_t lume_vfs_sync(struct lume_files *, uint64_t fd);
int64_t lume_vfs_seek(struct lume_files *, uint64_t fd, int64_t offset, uint64_t whence);
int64_t lume_vfs_stat(struct lume_files *, uint64_t fd, struct lume_stat *);
int64_t lume_vfs_stat_mode(struct lume_files *, uint64_t, struct lume_stat *);
int64_t lume_vfs_stat_path(struct lume_files *, const char *, struct lume_stat *);
int64_t lume_vfs_truncate(struct lume_files *, uint64_t);
int64_t lume_vfs_rename(struct lume_files *, const char *, const char *);
int64_t lume_vfs_getdents(struct lume_files *, uint64_t fd, void *, uint64_t bytes);
int64_t lume_vfs_chdir(struct lume_files *, const char *);
bool lume_vfs_image(const char *path, const void **data, size_t *bytes);
bool lume_vfs_mount_fat(const void *, size_t);
bool lume_vfs_mount_storage(const struct lume_storage_ops *);
int lume_vfs_program(struct lume_files *, const char *, const void **, size_t *);
int64_t lume_vfs_dup2(struct lume_files *, uint64_t, uint64_t);
int64_t lume_vfs_fd_call(struct lume_files *, unsigned, struct lume_fd_request *);
bool lume_vfs_pipe_wait(struct lume_files *, uint64_t);
struct lume_wait_queue;
struct lume_wait_queue *lume_vfs_pipe_queue(struct lume_files *, uint64_t, bool writing);
int64_t lume_vfs_mkdir(struct lume_files *, const char *);
int64_t lume_vfs_remove(struct lume_files *, const char *, bool);
/* The console read hook returns -EAGAIN when no physical RX byte is available. */
int lume_console_getc(void);
bool lume_console_ready(void);
void lume_console_putc(char);
void lume_console_write(const char *, size_t);
#endif
