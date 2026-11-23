#ifndef LUME_STORAGE_H
#define LUME_STORAGE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
struct lume_files_volume;
struct lume_file_time;
struct lume_file_times;
struct lume_file_allocation;
struct lume_stat;
struct lume_inode_access;

typedef bool (*lume_storage_visit)(const char *, bool, uint64_t, void *);

#define LUME_STORAGE_INODE_BASE UINT64_C(0x8000000000000000)

enum
{
    LUME_STORAGE_REPLACE_UNCHANGED,
    LUME_STORAGE_REPLACE_UNCERTAIN,
    LUME_STORAGE_REPLACE_COMMITTED
};

#define LUME_STORAGE_NAMESPACE_UNCHANGED LUME_STORAGE_REPLACE_UNCHANGED
#define LUME_STORAGE_NAMESPACE_UNCERTAIN LUME_STORAGE_REPLACE_UNCERTAIN
#define LUME_STORAGE_NAMESPACE_COMMITTED LUME_STORAGE_REPLACE_COMMITTED

struct lume_storage_snapshot
{
    uint64_t mount, object, generation, bytes;
};

struct lume_storage_dirent
{
    uint64_t inode;
    uint32_t kind;
    char name[256];
};

struct lume_storage_ops
{
    bool (*walk)(lume_storage_visit, void *);
    /* A provider with next supports directory enumeration without importing every entry. */
    int (*next)(const char *, uint64_t *, struct lume_storage_dirent *);
    int (*create)(const char *, bool);
    int (*truncate)(const char *);
    int (*read)(const char *, uint64_t, void *, size_t);
    int (*write)(const char *, uint64_t, const void *, size_t);
    int (*remove)(const char *);
    int (*resize)(const char *, uint64_t);
    int (*rename)(const char *, const char *);
    /* Use handle callbacks as a complete group for existing files and VFS offsets.
     * A close attempt consumes the handle even when it reports an error. */
    int (*open)(const char *, unsigned, void **);
    int (*read_handle)(void *, uint64_t, void *, size_t);
    int (*write_handle)(void *, uint64_t, const void *, size_t);
    int (*resize_handle)(void *, uint64_t);
    int (*close)(void *);
    int (*sync_handle)(void *);
    /* Replacement accepts objects of the same kind. Directory targets must be empty.
     * Open regular files retain their backing until their final handle is closed.
     * The state records namespace changes even when reclaiming old contents fails. */
    int (*replace)(const char *, const char *, unsigned *state);
    /* Compare complete paths using the volume's filename case rules. */
    bool (*path_equal)(const char *, const char *);
    int (*remove_state)(const char *, unsigned *state);
    int (*rename_state)(const char *, const char *, unsigned *state);
    /* Equal paths must have equal hashes. Providers may omit the hash callback. */
    uint32_t (*path_hash)(const char *);
    int (*volume_info)(struct lume_files_volume *);
    /* A retained backing has one identity and content generation across all aliases.
     * Content mutation advances the generation before changing any bytes. */
    int (*snapshot_handle)(void *, struct lume_storage_snapshot *);
    /* Release cached handles before resetting or remounting the backend. */
    void (*program_cache_owner)(int (*release)(void));
    /* Native inode keys exceed UINT32_MAX to separate them from RAM node identities. */
    int (*metadata)(const char *, struct lume_stat *, struct lume_inode_access *,
                    struct lume_file_times *);
    bool read_only;
    /* Return target bytes without a terminator. The final component remains a link. */
    int (*readlink)(const char *, void *, size_t);
    /* Native permission metadata bypasses the permission sidecar. */
    int (*set_mode)(const char *, unsigned);
    int (*set_owner)(const char *, uint32_t uid, uint32_t gid);
    /* Commit the directory entry and its access metadata together. */
    int (*create_owned)(const char *, unsigned kind, const struct lume_inode_access *, uint64_t *inode);
    int (*link)(const char *source, const char *target);
    int (*set_times)(const char *, const struct lume_file_time *);
    int (*symlink)(const char *path, const char *target, const struct lume_inode_access *,
                   uint64_t *inode);
    int (*allocation)(uint64_t inode, struct lume_file_allocation *);
};

struct lume_volume
{
    uint64_t first, count;
};

static inline bool lume_volume_contains(const struct lume_volume *volume, uint64_t sector,
                                        unsigned count)
{
    return count && sector >= volume->first && sector - volume->first < volume->count &&
           count <= volume->count - (sector - volume->first);
}

enum lume_volume_format
{
    LUME_VOLUME_NONE,
    LUME_VOLUME_FAT32,
    LUME_VOLUME_EXT4
};

#define LUME_PC_DISK_ID UINT32_C(0x364d554c)
#define LUME_PC_DATA_FIRST UINT64_C(131072)
#define LUME_Q35_DISK_ID UINT32_C(0x354d554c)
#define LUME_Q35_EFI_DISK_ID UINT32_C(0x384d554c)
#define LUME_INSTALL_DISK_ID UINT32_C(0x374d554c)
#define LUME_INSTALL_DATA_FIRST UINT32_C(1048576)
#define LUME_Q35_EFI_FIRST UINT64_C(2048)
#define LUME_Q35_EFI_SECTORS UINT64_C(2097152)
#define LUME_Q35_EFI_DATA_FIRST (LUME_Q35_EFI_FIRST + LUME_Q35_EFI_SECTORS)
#define LUME_CARD_DISK_ID UINT32_C(0x324d554c)
#define LUME_CARD_BOOT_FIRST UINT64_C(2048)
#define LUME_CARD_BOOT_SECTORS UINT64_C(1048576)
#define LUME_CARD_DATA_FIRST (LUME_CARD_BOOT_FIRST + LUME_CARD_BOOT_SECTORS)

bool lume_volume_select(const unsigned char mbr[512], uint64_t sectors, struct lume_volume *);
enum lume_volume_format lume_volume_identify(const unsigned char mbr[512], uint64_t sectors,
                                             struct lume_volume *);
bool lume_volume_boot_select(const unsigned char mbr[512], uint64_t sectors, struct lume_volume *);
bool lume_volume_alternate_boot_select(const unsigned char mbr[512], uint64_t sectors,
                                       struct lume_volume *);
bool lume_volume_journal_select(const unsigned char mbr[512], uint64_t sectors,
                                struct lume_volume *);
const struct lume_storage_ops *lume_storage_start(void);
void lume_storage_installer_mode(bool enable);
bool lume_storage_is_installer(void);
bool lume_storage_is_ext4(void);
/* Finish the data volume after the last VFS operation and before stopping the block device. */
bool lume_storage_finish(void);
/* Schedule expired partial-sector and metadata flushes on the serialized kernel worker. */
void lume_storage_poll_background(void);
#endif
