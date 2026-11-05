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
    LUME_FILES_VOLUME
};

enum
{
    LUME_FILES_STAT_NOFOLLOW = 1u,
    LUME_FILES_STAT_EMPTY_PATH = 2u,
    LUME_FILES_OPEN_DIRECTORY = 0x10000u,
    LUME_FILES_OPEN_NOFOLLOW = 0x20000u
};

enum
{
    LUME_FILES_VOLUME_FD = 1u,
    LUME_FILES_VOLUME_READ_ONLY = 1u
};

/* FILES takes an operation, a request address and its exact byte size. FAT filenames
 * use UTF-8. Paths include a terminating zero within LUME_FILES_PATH_MAX. Components contain
 * at most LUME_FILES_NAME_MAX bytes. Relative paths use fd as a directory descriptor or
 * LUME_FILES_AT_CWD. RENAME and REPLACE use fd2 for their second relative path.
 * Absolute paths ignore the corresponding directory descriptor. OPEN combines LUME_O
 * flags with the FILES_OPEN flags and takes creation permissions in mode. Descriptor
 * flags use LUME_FD_CLOEXEC and LUME_FD_NONBLOCK. STAT writes a lume_stat and returns its
 * file mode. STAT_EMPTY_PATH selects fd when path addresses an empty string.
 * GETDENTS returns complete lume_files_dirent records and advances the shared offset.
 * GETCWD, FD_PATH and CANONICAL write a terminated path and return its byte length
 * excluding the terminator. Their count includes space for the terminator. MODE sets
 * the low permission bits without changing ownership. VOLUME writes a lume_files_volume.
 * VOLUME_FD selects fd and requires path to be zero. Otherwise it resolves path using fd.
 * Volume block counts use fragment_bytes as their unit. Unused fields are zero. */
struct lume_files_request
{
    uint32_t version, bytes;
    uint64_t path, path2, buffer, count;
    int64_t fd, fd2;
    uint64_t flags;
    uint32_t mode, descriptor_flags;
};

struct lume_files_dirent
{
    uint64_t inode;
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
_Static_assert(sizeof(struct lume_files_dirent) == 272, "directory record ABI");
#endif
