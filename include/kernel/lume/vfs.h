#ifndef LUME_VFS_H
#define LUME_VFS_H
#include <lume/abi.h>
#include <stdbool.h>
#include <stddef.h>
#include "fat.h"
#include "storage.h"
#include "poll.h"
#include <fd.h>
#include "lume/credential.h"

struct lume_identity
{
    uint32_t uid, euid, suid, gid, egid, sgid, groups_count;
    uint32_t groups[LUME_GROUP_MAX], umask;
    bool jailed;
    unsigned sandbox_profile;

    struct
    {
        uint64_t soft, hard;
    } limit[LUME_LIMIT_COUNT];
};

struct lume_fd
{
    int object;
    uint32_t flags;
    uint64_t generation;
    struct lume_poll_source changed;
};

#if defined(__aarch64__) || defined(LUME_ARCH_AARCH64) || defined(LUME_ARCH_AMD64)
#define LUME_FILES_DYNAMIC 1
#else
#define LUME_FILES_DYNAMIC 0
#endif

struct lume_directory_authorities;

struct lume_file_identity
{
    uint64_t key, version, bytes;
};

struct lume_files
{
    char cwd[LUME_PATH_MAX];
#if LUME_FILES_DYNAMIC
    struct lume_fd *fd;
#else
    struct lume_fd fd[LUME_FD_MAX];
#endif
    struct lume_identity identity;
    unsigned cwd_node, cwd_generation;
    struct lume_directory_authorities *directory_authorities;
};

void lume_vfs_init(const void *shell, size_t bytes);
/* Enable deferred final-reference cleanup after resource workers are initialized. */
void lume_vfs_reap_start(void);
bool lume_vfs_reap_pending(void);
/* STORAGE owns handle and node reclamation until this bounded batch completes. */
unsigned lume_vfs_reap(unsigned);
void lume_vfs_program_buffer(void *, size_t);
void lume_vfs_program_allocator(void *(*)(size_t));
bool lume_vfs_node_buffer(void *, size_t);
/* Initialization and cloning require an uninitialized or released destination. */
bool lume_files_init(struct lume_files *);
bool lume_files_clone(struct lume_files *, const struct lume_files *);
bool lume_files_inherit(struct lume_files *, const struct lume_files *);
/* Move consumes its source and requires a released destination. */
void lume_files_move(struct lume_files *, struct lume_files *);
bool lume_vfs_exec_callbacks(const struct lume_files *);
void lume_files_release(struct lume_files *);
uint64_t lume_files_pages(const struct lume_files *);
int64_t lume_vfs_open(struct lume_files *, const char *, uint64_t flags);
int lume_vfs_sandbox_access(struct lume_files *, uint64_t, bool);
int lume_vfs_directory_authority(struct lume_files *, unsigned, uint64_t);
bool lume_vfs_sandbox_directory(struct lume_files *, const char *, bool);
int lume_vfs_x11_authority(struct lume_files *, uint64_t);
bool lume_vfs_sandbox_x11_authority(struct lume_files *, const char *);
void lume_vfs_terminal_open(int (*)(struct lume_files *, const char *, uint64_t));
int64_t lume_vfs_open_compat(struct lume_files *, const char *, uint64_t flags);
int64_t lume_vfs_remove_compat(struct lume_files *, const char *, bool);
int64_t lume_vfs_stat_inode(uint64_t, struct lume_stat *);
int64_t lume_vfs_close(struct lume_files *, uint64_t fd);
int64_t lume_vfs_read(struct lume_files *, uint64_t fd, void *, uint64_t bytes);
int64_t lume_vfs_write(struct lume_files *, uint64_t fd, const void *, uint64_t bytes);
int64_t lume_vfs_pread(struct lume_files *, uint64_t fd, void *, uint64_t bytes, int64_t offset);
int64_t lume_vfs_pwrite(struct lume_files *, uint64_t fd, const void *, uint64_t bytes,
                        int64_t offset);
int64_t lume_vfs_sync(struct lume_files *, uint64_t fd);
int64_t lume_vfs_seek(struct lume_files *, uint64_t fd, int64_t offset, uint64_t whence);
struct lume_file_lock_request;
int64_t lume_vfs_ofd_lock(struct lume_files *, bool, struct lume_file_lock_request *);
int64_t lume_vfs_flock(struct lume_files *, struct lume_file_lock_request *);
int64_t lume_vfs_posix_lock(struct lume_files *, unsigned, bool, struct lume_file_lock_request *);
int64_t lume_vfs_process_close(struct lume_files *, uint64_t, unsigned);
int64_t lume_vfs_process_dup2(struct lume_files *, uint64_t, uint64_t, unsigned);
int64_t lume_vfs_process_fd_call(struct lume_files *, unsigned, struct lume_fd_request *, unsigned);
void lume_vfs_process_exec(struct lume_files *, unsigned);
void lume_vfs_process_release(unsigned);
int64_t lume_vfs_stat(struct lume_files *, uint64_t fd, struct lume_stat *);
int64_t lume_vfs_stat_mode(struct lume_files *, uint64_t, struct lume_stat *);
int64_t lume_vfs_stat_path(struct lume_files *, const char *, struct lume_stat *);
int lume_vfs_volume_path(struct lume_files *, const char *, struct lume_files_volume *);
int lume_vfs_volume_fd(struct lume_files *, uint64_t, struct lume_files_volume *);
int64_t lume_vfs_truncate(struct lume_files *, uint64_t);
int64_t lume_vfs_resize(struct lume_files *, uint64_t, uint64_t);
int64_t lume_vfs_rename(struct lume_files *, const char *, const char *);
int64_t lume_vfs_replace(struct lume_files *, const char *, const char *);
int64_t lume_vfs_getdents(struct lume_files *, uint64_t fd, void *, uint64_t bytes);
int64_t lume_vfs_chdir(struct lume_files *, const char *);
int64_t lume_vfs_chroot(struct lume_files *, const char *);
struct lume_file_mode_request;
int64_t lume_vfs_file_mode(struct lume_files *, unsigned, struct lume_file_mode_request *,
                           const char *);
bool lume_vfs_image(const char *path, const void **data, size_t *bytes);
bool lume_vfs_mount_fat(const void *, size_t);
bool lume_vfs_mount_storage(const struct lume_storage_ops *);
int lume_vfs_program(struct lume_files *, const char *, const void **, size_t *);
/* A program reference owns an open description without consuming a user descriptor. */
int lume_vfs_program_hold(struct lume_files *, const char *, size_t *);
int lume_vfs_object_identity(int, struct lume_file_identity *);
int lume_vfs_object_retain(int);
int lume_vfs_exec_pin(int);
void lume_vfs_exec_unpin(int);
void lume_vfs_mapping_notify(void (*)(uint64_t, uint64_t, uint64_t));
int lume_vfs_canonical(struct lume_files *, const char *, char[LUME_PATH_MAX]);
int64_t lume_vfs_dup2(struct lume_files *, uint64_t, uint64_t);
int64_t lume_vfs_fd_call(struct lume_files *, unsigned, struct lume_fd_request *);
bool lume_vfs_pipe_wait(struct lume_files *, uint64_t);
bool lume_vfs_pipe_io(struct lume_files *, uint64_t);
bool lume_vfs_memory_io(struct lume_files *, uint64_t);
bool lume_vfs_console_io(struct lume_files *, uint64_t);
bool lume_vfs_storage_io(struct lume_files *, uint64_t);
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
