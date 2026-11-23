#ifndef LUME_FILES_ABI_H
#define LUME_FILES_ABI_H
#include <stdint.h>

#define LUME_FILES_VERSION 1u
#define LUME_FILES_PATH_MAX 1024u
#define LUME_FILES_NAME_MAX 255u
#define LUME_FILES_AT_CWD (-INT64_C(100))

enum
{
    LUME_FILES_OPEN,
    LUME_FILES_STAT,
    LUME_FILES_CHDIR,
    LUME_FILES_GETCWD,
    LUME_FILES_MKDIR,
    LUME_FILES_UNLINK,
    LUME_FILES_RMDIR,
    LUME_FILES_RENAME,
    LUME_FILES_REPLACE,
    LUME_FILES_GETDENTS,
    LUME_FILES_FD_PATH,
    LUME_FILES_CANONICAL,
    LUME_FILES_MODE,
    LUME_FILES_VOLUME,
    LUME_FILES_READLINK,
    LUME_FILES_LINK,
    LUME_FILES_TIMES,
    LUME_FILES_MKFIFO,
    LUME_FILES_SYMLINK,
    LUME_FILES_ACCESS,
    LUME_FILES_FCHDIR
};

enum
{
    LUME_FILES_STAT_NOFOLLOW = 1u,
    LUME_FILES_STAT_EMPTY_PATH = 2u,
    LUME_FILES_GETDENTS_POSIX = 1u,
    LUME_FILES_MODE_NOFOLLOW = 1u,
    LUME_FILES_LINK_FOLLOW = 1u,
    LUME_FILES_TIMES_NOFOLLOW = 1u,
    LUME_FILES_TIMES_FD = 2u,
    LUME_FILES_ACCESS_NOFOLLOW = 1u,
    LUME_FILES_ACCESS_EFFECTIVE = 2u,
    LUME_FILES_OPEN_DIRECTORY = 0x10000u,
    LUME_FILES_OPEN_NOFOLLOW = 0x20000u
};

#define LUME_FILES_DIR_AFTER_DOT UINT64_C(0x40000000)
#define LUME_FILES_DIR_AFTER_DOTDOT (LUME_FILES_DIR_AFTER_DOT + 1)

enum
{
    LUME_FILE_TIME_NOW = 1000000001u,
    LUME_FILE_TIME_OMIT = 1000000002u
};

enum
{
    LUME_FILES_VOLUME_FD = 1u,
    LUME_FILES_VOLUME_QUERY_PERMISSIONS = 2u,
    LUME_FILES_VOLUME_READ_ONLY = 1u,
    LUME_FILES_VOLUME_NATIVE_PERMISSIONS = 2u
};

/* FILES takes an operation, a request address and its exact byte size. FAT filenames
 * use UTF-8. Paths include a terminating zero within LUME_FILES_PATH_MAX. Components contain
 * at most LUME_FILES_NAME_MAX bytes. Relative paths use fd as a directory descriptor or
 * LUME_FILES_AT_CWD. RENAME, REPLACE and LINK use fd2 for their second relative path.
 * Absolute paths ignore the corresponding directory descriptor. FCHDIR selects the working
 * directory through fd. OPEN combines LUME_O flags with the FILES_OPEN flags and takes
 * creation permissions in mode. MKDIR
 * also accepts the sticky bit. Descriptor
 * flags use LUME_FD_CLOEXEC and LUME_FD_NONBLOCK. STAT returns the mode and writes
 * lume_stat, lume_files_stat, lume_files_stat_times or lume_files_stat_allocation,
 * selected by the buffer size.
 * The standard device paths duplicate the corresponding descriptor with shared status and offset.
 * Their metadata describes the open descriptor.
 * STAT_EMPTY_PATH selects
 * fd when path addresses an empty string.
 * GETDENTS returns complete lume_files_dirent records and advances the shared offset.
 * GETDENTS_POSIX returns lume_files_dirent_posix records and includes dot entries.
 * The position field is the seek position immediately following the entry.
 * GETCWD, FD_PATH and CANONICAL write a terminated path and return its byte length
 * excluding the terminator. Their count includes space for the terminator. MODE sets the
 * permissions without changing ownership. Directories accept sticky and set-group-ID bits.
 * Regular files on filesystems with native ownership accept set-ID bits.
 * MODE_NOFOLLOW returns EOPNOTSUPP for a final symbolic link.
 * VOLUME writes a lume_files_volume.
 * VOLUME_FD selects fd and requires path to be zero. Otherwise it resolves path using fd.
 * VOLUME_QUERY_PERMISSIONS requests the native permissions flag for filesystems that persist
 * ownership and permissions in inode metadata and create them with the directory entry.
 * Volume block counts use fragment_bytes as their unit. READLINK returns target bytes
 * without a terminator, truncated to count. It leaves the final link unresolved.
 * SYMLINK creates the link at path and stores path2 verbatim as its target.
 * ACCESS checks the read, write and execute bits in mode using real credentials.
 * ACCESS_EFFECTIVE selects effective credentials. ACCESS_NOFOLLOW checks the final link.
 * STAT_NOFOLLOW leaves the final link unresolved. LINK_FOLLOW resolves the final source link.
 * A stat link count of zero indicates that the provider does not report a count.
 * TIMES reads two lume_file_time values in access and modification order. TIMES_FD
 * selects fd and requires a zero path. TIMES_NOFOLLOW selects the symlink itself.
 * MKFIFO creates a named pipe using mode and the process umask.
 * NOW uses the current time. OMIT preserves the corresponding timestamp.
 * Unused fields are zero. */
struct lume_files_request
{
    uint32_t version, bytes;
    uint64_t path, path2, buffer, count;
    int64_t fd, fd2;
    uint64_t flags;
    uint32_t mode, descriptor_flags;
};

struct lume_files_stat
{
    uint64_t inode, size;
    uint32_t kind, links, uid, gid;
};

struct lume_file_time
{
    int64_t seconds;
    uint32_t nanoseconds, reserved;
};

struct lume_file_times
{
    struct lume_file_time access, modification, change;
};

struct lume_files_stat_times
{
    struct lume_files_stat stat;
    struct lume_file_times times;
};

struct lume_file_allocation
{
    uint64_t sectors;
    /* A zero block size means the filesystem has not supplied allocation data. */
    uint32_t block_bytes, reserved;
};

struct lume_files_stat_allocation
{
    struct lume_files_stat_times metadata;
    struct lume_file_allocation allocation;
};

struct lume_files_dirent
{
    uint64_t inode;
    uint32_t kind, name_bytes;
    char name[LUME_FILES_NAME_MAX + 1];
};

struct lume_files_dirent_posix
{
    uint64_t inode, position;
    uint32_t kind, name_bytes;
    char name[LUME_FILES_NAME_MAX + 1];
};

struct lume_files_volume
{
    uint32_t version, bytes;
    uint64_t block_bytes, fragment_bytes, total_blocks, free_blocks, available_blocks;
    uint64_t filesystem_id;
    uint32_t flags, name_max;
};

_Static_assert(sizeof(struct lume_files_volume) == 64, "volume record ABI");
_Static_assert(sizeof(struct lume_files_request) == 72, "path request ABI");
_Static_assert(sizeof(struct lume_files_stat) == 32, "file metadata ABI");
_Static_assert(sizeof(struct lume_files_dirent_posix) == 280, "POSIX directory entry ABI");
_Static_assert(sizeof(struct lume_file_time) == 16, "file timestamp ABI");
_Static_assert(sizeof(struct lume_files_stat_times) == 80, "file metadata and timestamp ABI");
_Static_assert(sizeof(struct lume_files_stat_allocation) == 96, "file allocation ABI");
_Static_assert(sizeof(struct lume_files_dirent) == 272, "directory record ABI");
#endif
