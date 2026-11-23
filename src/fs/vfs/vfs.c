#include "lume/file_access.h"
#include "lume/vfs.h"
#include "lume/early.h"
#include "alias.h"
#include "lume/wait.h"
#include "lume/devfs.h"
#include "lume/devfs_character.h"
#include "lume/external_file.h"
#include "lume/file_modes.h"
#include "lume/file_mode.h"
#include "lume/poll.h"
#include "lume/smp.h"
#include "lume/sandbox_policy.h"
#include "lume/socket_file.h"
#include "lume/tty_device.h"
#include "lume/kernel_memory.h"
#include "lume/path.h"
#include "lume/vfs_paths.h"
#include "lume/files.h"
#include "lume/io_redirect.h"
#include "lume/worker.h"
#include <string.h>
#include "file_locks.h"
#include "fd_publication.h"

#define NODES 192u
#define SHORT_STORAGE_INODE_BASE UINT32_C(0x8000)
#define CAPACITY 4096u
#define RAM_FILES 32u
#define FD_FIFO_PENDING (-2)
static unsigned char ram_data[RAM_FILES][CAPACITY];
static bool ram_used[RAM_FILES];
static uint64_t descriptor_sequence;
static uint64_t poll_source_sequence;
static uint64_t poll_pending[LUME_POLL_WORDS], poll_subscribed[LUME_POLL_WORDS];
static struct lume_poll_source console_source;

struct poll_link
{
    struct lume_poll_source *source;
    uint64_t generation;
};

static struct
{
#if LUME_FILES_DYNAMIC
    struct poll_link *links;
    unsigned capacity;
#else
    struct poll_link links[LUME_FD_LIMIT * 2];
#endif
    unsigned count;
} poll_subscriptions[LUME_POLL_TASK_LIMIT];

static bool poll_input_ready;
static int64_t close_deferred(struct lume_files *, uint64_t);
static int (*terminal_open)(struct lume_files *, const char *, uint64_t);
static int64_t (*terminal_stat)(struct lume_files *, const char *, uint32_t, struct lume_stat *,
                                struct lume_inode_access *);
static int (*terminal_set_access)(struct lume_files *, const char *,
                                  const struct lume_inode_access *);

void lume_vfs_terminal_open(int (*callback)(struct lume_files *, const char *, uint64_t))
{
    terminal_open = callback;
}

void lume_vfs_terminal_stat(int64_t (*callback)(struct lume_files *, const char *, uint32_t,
                                                struct lume_stat *, struct lume_inode_access *))
{
    terminal_stat = callback;
}

void lume_vfs_terminal_set_access(int (*callback)(struct lume_files *, const char *,
                                                  const struct lume_inode_access *))
{
    terminal_set_access = callback;
}

bool lume_vfs_poll_input_ready(void)
{
    return poll_input_ready;
}

void lume_poll_source_notify(struct lume_poll_source *source)
{
    bool changed = false;
    for (unsigned word = 0; word < LUME_POLL_WORDS; ++word)
    {
        uint64_t waiting = word ? source->waiting_more[word - 1] : source->waiting;
        changed |= (waiting & ~poll_pending[word]) != 0;
        poll_pending[word] |= waiting;
    }
    if (changed)
        lume_smp_wake();
}

void lume_poll_source_reset(struct lume_poll_source *source)
{
    lume_poll_source_notify(source);
    if (!++poll_source_sequence)
        ++poll_source_sequence;
    *source = (struct lume_poll_source){.generation = poll_source_sequence};
}

void lume_vfs_poll_cancel(unsigned task)
{
    if (task >= LUME_POLL_TASK_LIMIT)
        return;
    unsigned word = task / 64u;
    uint64_t bit = UINT64_C(1) << (task % 64u);
    for (unsigned i = 0; i < poll_subscriptions[task].count; ++i)
    {
        struct lume_poll_source *source = poll_subscriptions[task].links[i].source;
        if (source->generation == poll_subscriptions[task].links[i].generation)
        {
            uint64_t *waiting = word ? &source->waiting_more[word - 1] : &source->waiting;
            *waiting &= ~bit;
        }
    }
    poll_subscriptions[task].count = 0;
    poll_subscribed[word] &= ~bit;
    poll_pending[word] &= ~bit;
}

void lume_vfs_poll_release(unsigned task)
{
    if (task >= LUME_POLL_TASK_LIMIT)
        return;
    lume_vfs_poll_cancel(task);
#if LUME_FILES_DYNAMIC
    lume_kernel_free(poll_subscriptions[task].links);
    poll_subscriptions[task].links = NULL;
    poll_subscriptions[task].capacity = 0;
#endif
}

bool lume_vfs_poll_take(unsigned task)
{
    if (task >= LUME_POLL_TASK_LIMIT)
        return false;
    unsigned word = task / 64u;
    uint64_t bit = UINT64_C(1) << (task % 64u);
    bool ready = (poll_pending[word] & bit) != 0;
    poll_pending[word] &= ~bit;
    return ready;
}

void lume_poll_sources_retire_range(const void *allocation, size_t bytes)
{
    uintptr_t first = (uintptr_t)allocation;
    if (!bytes || bytes > UINTPTR_MAX - first)
        return;
    uintptr_t end = first + bytes;
    bool changed = false;
    for (unsigned word = 0; word < LUME_POLL_WORDS; ++word)
    {
        uint64_t tasks = poll_subscribed[word];
        while (tasks)
        {
            unsigned slot = (unsigned)__builtin_ctzll(tasks);
            uint64_t bit = UINT64_C(1) << slot;
            unsigned task = word * 64u + slot;
            tasks &= tasks - 1;
            unsigned index = 0;
            while (index < poll_subscriptions[task].count)
            {
                struct poll_link *link = &poll_subscriptions[task].links[index];
                uintptr_t address = (uintptr_t)link->source;
                if (address < first || address >= end)
                {
                    ++index;
                    continue;
                }
                uint64_t *waiting =
                    word ? &link->source->waiting_more[word - 1] : &link->source->waiting;
                *waiting &= ~bit;
                changed |= (poll_pending[word] & bit) == 0;
                poll_pending[word] |= bit;
                *link = poll_subscriptions[task].links[--poll_subscriptions[task].count];
            }
            if (!poll_subscriptions[task].count)
                poll_subscribed[word] &= ~bit;
        }
    }
    if (changed)
        lume_smp_wake();
}

void lume_poll_source_retire(struct lume_poll_source *source)
{
    lume_poll_sources_retire_range(source, sizeof(*source));
}

static bool poll_subscribe_source(unsigned task, struct lume_poll_source *source)
{
    if (!source)
        return true;
    if (!source->generation)
        return false;
    unsigned word = task / 64u;
    uint64_t bit = UINT64_C(1) << (task % 64u);
    uint64_t *waiting = word ? &source->waiting_more[word - 1] : &source->waiting;
    if (*waiting & bit)
        return true;
    unsigned count = poll_subscriptions[task].count;
    if (count == LUME_FD_LIMIT * 2
#if LUME_FILES_DYNAMIC
        || count == poll_subscriptions[task].capacity
#endif
    )
        return false;
    poll_subscriptions[task].links[count].source = source;
    poll_subscriptions[task].links[count].generation = source->generation;
    ++poll_subscriptions[task].count;
    poll_subscribed[word] |= bit;
    *waiting |= bit;
    return true;
}

void lume_vfs_poll_input(bool ready)
{
    bool changed = poll_input_ready != ready;
    poll_input_ready = ready;
    if (changed && ready)
        lume_poll_source_notify(&console_source);
}

static void bind_descriptor(struct lume_files *files, unsigned fd, int object, uint32_t flags)
{
    if (!++descriptor_sequence)
        ++descriptor_sequence;
    lume_poll_source_reset(&files->fd[fd].changed);
    struct fd_guard guard = fd_enter(files);
    files->fd[fd].object = object;
    files->fd[fd].flags = flags;
    files->fd[fd].generation = descriptor_sequence;
    fd_leave(guard);
}

/* Duplicated and inherited descriptors share one offset and access mode. Pipe endpoints
 * keep their channel alive until the last reference to each open description closes. */
#define PIPES (2u * LUME_TASK_LIMIT)

#include "open_files.h"

static unsigned storage_reap_count, storage_reap_head, storage_reap_tail;
static bool storage_reap_enabled, storage_reaping;

static unsigned nofile_default = LUME_FD_MAX;
static void resource_limits_start(void);

struct directory_authority
{
    int object;
    bool active;
};

struct lume_directory_authorities
{
    unsigned generation;
    int x11_object;
    bool x11_active;
    struct directory_authority entry[LUME_SANDBOX_DIRECTORY_LIMIT];
};

struct channel
{
    unsigned readers, writers, head, count, capacity, opening;
    uint64_t fifo_identity, reader_epoch, writer_epoch;
    bool growing, copying;
    struct lume_wait_queue readable, writable;
    struct lume_poll_source changed;
#if LUME_FILES_DYNAMIC
    unsigned char *data;
#else
    unsigned char data[LUME_PIPE_BYTES];
#endif
};

#if LUME_FILES_DYNAMIC
static struct channel *channels;
#if defined(__aarch64__) || defined(LUME_ARCH_AARCH64) || defined(LUME_ARCH_AMD64) ||              \
    defined(LUME_ARCH_I686)
#define PIPE_MAX_PAGES 16u
#define PIPE_BUDGET_PAGES 8192u
#define PIPE_BUDGET_MAX_PAGES 65536u
#else
#define PIPE_MAX_PAGES 4u
#define PIPE_BUDGET_PAGES 256u
#define PIPE_BUDGET_MAX_PAGES PIPE_BUDGET_PAGES
#endif
static unsigned pipe_pages;
static unsigned pipe_page_limit = PIPE_BUDGET_PAGES;

static void pipe_backing_release(struct channel *channel)
{
    if (!channel->data)
        return;
    unsigned pages = channel->capacity / 4096u;
    for (unsigned i = 0; i < pages; ++i)
        if (!lume_page_free((uintptr_t)channel->data + (uint64_t)i * 4096u))
            lume_fail("pipe backing release");
    pipe_pages -= pages;
    channel->data = NULL;
}
#else
static struct channel channels[PIPES];
#endif

static unsigned channel_slots(void)
{
#if LUME_FILES_DYNAMIC
    return channels ? PIPES : 0;
#else
    return PIPES;
#endif
}

static void release_channel(struct channel *channel)
{
#if LUME_FILES_DYNAMIC
    pipe_backing_release(channel);
#endif
    *channel = (struct channel){0};
}

static int allocate_channel(void)
{
#if LUME_FILES_DYNAMIC
    if (!channels)
    {
        struct channel *table = lume_kernel_allocate(sizeof(*table) * PIPES);
        if (channels)
            lume_kernel_free(table);
        else
            channels = table;
        if (!channels)
            return -LUME_ENOMEM;
    }
#endif
    unsigned slot = 0;
    while (slot < PIPES &&
           (channels[slot].readers || channels[slot].writers || channels[slot].opening))
        ++slot;
    if (slot == PIPES)
        return -LUME_ENOMEM;
    struct channel *channel = &channels[slot];
#if LUME_FILES_DYNAMIC
    if (pipe_pages == pipe_page_limit)
        return -LUME_ENOMEM;
    channel->opening = 1;
    ++pipe_pages;
    channel->data = (void *)(uintptr_t)lume_page_alloc(0);
    if (!channel->data)
    {
        --pipe_pages;
        channel->opening = 0;
        return -LUME_ENOMEM;
    }
#else
    channel->opening = 1;
#endif
    channel->capacity = LUME_PIPE_BYTES;
    lume_poll_source_reset(&channel->changed);
    return (int)slot;
}

enum
{
    STANDARD_DEVICE_BASE = 3,
    STANDARD_DEVICE_COUNT = 3
};

static struct node
{
    char *path;
    uint32_t kind;
    bool writable, linked, on_fat, persistent, pending_fat;
    bool mode_set, free_queued;
    unsigned mode;
    uint32_t uid, gid, links;
    unsigned device;
    unsigned references;
    unsigned executables;
    uint32_t path_hash, path_next, parent;
    struct lume_fat_file fat_file;
    const unsigned char *image;
    uint64_t bytes;
    uint64_t version;
    uint64_t storage_inode;
    uint64_t fifo_identity;
    unsigned char *data;
    struct lume_block *block;
    struct lume_devfs_character *character;
    struct lume_devfs_group *group;
    const struct lume_devfs_attribute *attribute;
} default_nodes[NODES];
static struct node *nodes = default_nodes;
static unsigned node_capacity = NODES;
static uint32_t *name_buckets;
static unsigned name_bucket_count;

static unsigned count;
static unsigned free_nodes;
static unsigned namespace_generation;
static uint64_t fifo_sequence;
static void (*mapping_changed)(uint64_t, uint64_t, uint64_t);
static struct lume_block *block_devices[LUME_DEVICE_LIMIT];
static struct lume_devfs_group resource_group;
static struct lume_devfs_group *attribute_groups[LUME_DEVICE_LIMIT] = {&resource_group};

static struct
{
    struct lume_devfs_character *device;
    unsigned opening;
} character_devices[LUME_DEVICE_LIMIT];
static struct lume_fat root_fat;
static const struct lume_storage_ops *storage;
static unsigned storage_mount_flags;
static bool import_storage_metadata(struct node *, bool);
static unsigned reserve_node(char *, unsigned);
static void publish_node(unsigned);
static void cancel_node(unsigned);
static int node_access(const struct lume_files *, const struct node *, unsigned);
static int node_access_with(const struct lume_files *, const struct node *, unsigned, unsigned);
static int64_t node_mode(const struct node *);
static int set_node_mode(struct node *, unsigned);
static int64_t mkdir_mode(struct lume_files *, const char *, unsigned);

static bool native_storage(const struct node *node)
{
    return node->persistent && storage && storage->metadata;
}

static bool setid_backing(const struct node *node)
{
    return native_storage(node) && storage->set_owner && storage->create_owned;
}

static bool same_backing(const struct node *a, const struct node *b)
{
    return a == b || (native_storage(a) && a->storage_inode && b->persistent &&
                      a->storage_inode == b->storage_inode &&
                      a->fifo_identity == b->fifo_identity);
}

static uint64_t node_identity(const struct node *node)
{
    return ((uint64_t)namespace_generation << 32) | ((unsigned)(node - nodes) + 1u);
}

static uint64_t storage_inode_number(uint64_t inode)
{
    /* Keep small native inode identities representable by 16-bit ino_t. */
    if (node_capacity < SHORT_STORAGE_INODE_BASE && inode >= LUME_STORAGE_INODE_BASE &&
        inode - LUME_STORAGE_INODE_BASE <= UINT16_MAX - SHORT_STORAGE_INODE_BASE)
        return SHORT_STORAGE_INODE_BASE + (inode - LUME_STORAGE_INODE_BASE);
    return inode;
}

static uint64_t node_inode(unsigned index)
{
    uint64_t inode = storage_inode_number(nodes[index].storage_inode);
    return inode ? inode : (uint64_t)index + 1;
}

void lume_vfs_mapping_notify(void (*notify)(uint64_t, uint64_t, uint64_t))
{
    mapping_changed = notify;
}

static void node_changed(struct node *node)
{
    if (!++node->version)
        lume_fail("file version exhausted");
    if (mapping_changed)
        mapping_changed(node_identity(node), node->bytes, node->version);
}

static void native_aliases(struct node *node, bool contents, bool attributes)
{
    if (!native_storage(node) || !node->storage_inode)
        return;
    for (unsigned i = 0; i < count; ++i)
    {
        struct node *alias = &nodes[i];
        if (alias == node || !same_backing(node, alias) ||
            (!alias->linked && (node->kind != LUME_NODE_PIPE || !alias->references)))
            continue;
        alias->bytes = node->bytes;
        if (attributes)
        {
            alias->uid = node->uid;
            alias->gid = node->gid;
            alias->links = node->links;
            alias->mode = node->mode;
            alias->mode_set = node->mode_set;
        }
        if (contents)
            node_changed(alias);
    }
}

static bool executable_backing(const struct node *node)
{
    if (node->executables)
        return true;
    if (native_storage(node) && node->storage_inode)
        for (unsigned i = 0; i < count; ++i)
            if (nodes[i].persistent && nodes[i].storage_inode == node->storage_inode &&
                nodes[i].executables)
                return true;
    return false;
}

int lume_vfs_object_identity(int object, struct lume_file_identity *identity)
{
    if (!identity || object < 0 || object >= (int)OPEN_FILES ||
        !open_file_count(open_file_at(object)))
        return -LUME_EBADF;
    const struct open_file *file = open_file_at(object);
    if ((file->flags & 3) == LUME_O_WRONLY)
        return -LUME_EACCES;
    if (file->external || file->pipe >= 0 || file->node < 0 ||
        nodes[file->node].kind != LUME_NODE_FILE)
        return -LUME_EINVAL;
    const struct node *node = &nodes[file->node];
    *identity = (struct lume_file_identity){node_identity(node), node->version, node->bytes};
    return 0;
}

int lume_vfs_object_retain(int object)
{
    if (object < 0 || object >= (int)OPEN_FILES || !open_file_count(open_file_at(object)))
        return -LUME_EBADF;
    return open_file_acquire(open_file_at(object)) ? 0 : -LUME_EOVERFLOW;
}

int lume_vfs_exec_pin(int object)
{
    struct lume_file_identity identity;
    int result = lume_vfs_object_identity(object, &identity);
    if (result)
        return result;
    unsigned index = (unsigned)open_file_at(object)->node;
    for (unsigned i = 0; i < OPEN_FILES; ++i)
        if (open_file_count(open_file_at(i)) && !open_file_at(i)->external &&
            open_file_at(i)->pipe < 0 && open_file_at(i)->node >= 0 &&
            same_backing(&nodes[open_file_at(i)->node], &nodes[index]) &&
            (open_file_at(i)->flags & 3))
            return -LUME_ETXTBSY;
    if (nodes[index].executables == UINT32_MAX)
        return -LUME_EOVERFLOW;
    ++nodes[index].executables;
    return 0;
}

void lume_vfs_exec_unpin(int object)
{
    if (object < 0 || object >= (int)OPEN_FILES || !open_file_count(open_file_at(object)) ||
        open_file_at(object)->node < 0 || !nodes[open_file_at(object)->node].executables)
        lume_fail("executable reference release");
    --nodes[open_file_at(object)->node].executables;
}

static bool storage_handles(void)
{
    return storage && storage->open && storage->read_handle && storage->write_handle &&
           storage->resize_handle && storage->sync_handle && storage->close;
}

/* Boot installs dynamically allocated staging before loading programs. */
static unsigned char default_program_buffer[65536];
static void *program_buffer = default_program_buffer;
static size_t program_capacity = sizeof(default_program_buffer);
static void *(*program_allocate)(size_t);

static struct
{
    const struct lume_storage_ops *owner;
    void *handle;
    struct lume_storage_snapshot snapshot;
} program_cache;

static int program_cache_clear(void)
{
    const struct lume_storage_ops *owner = program_cache.owner;
    void *handle = program_cache.handle;
    memset(&program_cache, 0, sizeof(program_cache));
    return handle ? owner->close(handle) : 0;
}

bool lume_vfs_node_buffer(void *buffer, size_t bytes)
{
    if (!buffer || count || (uintptr_t)buffer % _Alignof(struct node) ||
        bytes / sizeof(struct node) < NODES || bytes / sizeof(struct node) > UINT32_MAX / 4u)
        return false;
    unsigned capacity = (unsigned)(bytes / sizeof(struct node));
    unsigned bucket_count = 1;
    while (bucket_count < capacity * 2)
        bucket_count *= 2;
#if UINTPTR_MAX == UINT32_MAX
    if (bucket_count > SIZE_MAX / sizeof(uint32_t))
        return false;
#endif
    uint32_t *buckets = lume_kernel_allocate((size_t)bucket_count * sizeof(*buckets));
    if (!buckets)
        return false;
    memset(buckets, 0, (size_t)bucket_count * sizeof(*buckets));
    lume_kernel_free(name_buckets);
    name_buckets = buckets;
    name_bucket_count = bucket_count;
    nodes = buffer;
    node_capacity = capacity;
    return true;
}

void lume_vfs_program_allocator(void *(*allocate)(size_t))
{
    (void)program_cache_clear();
    program_allocate = allocate;
}

void lume_vfs_program_buffer(void *buffer, size_t bytes)
{
    (void)program_cache_clear();
    program_buffer = buffer;
    program_capacity = bytes;
}

static void release_node_memory(struct node *n)
{
    if (n->kind == LUME_NODE_LINK && !n->persistent)
        lume_path_release((char *)n->data);
    else if (n->data)
        for (unsigned i = 0; i < RAM_FILES; ++i)
            if (n->data == ram_data[i])
                ram_used[i] = false;
    n->data = 0;
    lume_path_release(n->path);
    n->path = NULL;
}

static void release_ram(struct node *n)
{
    if (n->linked || n->references)
        return;
    release_node_memory(n);
    if (!n->block && !n->free_queued)
    {
        n->path_next = free_nodes;
        free_nodes = (unsigned)(n - nodes) + 1;
        n->free_queued = true;
    }
}

static size_t length(const char *s)
{
    size_t n = 0;
    while (s[n])
        ++n;
    return n;
}

static bool equal(const char *a, const char *b)
{
    while (*a && *a == *b)
    {
        ++a;
        ++b;
    }
    return *a == *b;
}

static void copy(void *d, const void *s, size_t n)
{
    unsigned char *a = d;
    const unsigned char *b = s;
    while (n--)
        *a++ = *b++;
}

static int read_node_link(const struct node *node, void *out, size_t bytes)
{
    if (node->persistent)
        return storage && storage->readlink ? storage->readlink(node->path, out, bytes)
                                             : -LUME_EIO;
    if (!node->data)
        return -LUME_EIO;
    if (bytes > node->bytes)
        bytes = (size_t)node->bytes;
    copy(out, node->data, bytes);
    return (int)bytes;
}

static uint32_t namespace_hash(const char *path)
{
    if (storage && storage->path_hash && !strncmp(path, "/data", 5) && (!path[5] || path[5] == '/'))
        return storage->path_hash(path);
    return lume_path_hash_folded(path);
}

static void index_insert_hashed(unsigned index, uint32_t hash)
{
    struct node *node = &nodes[index];
    node->path_hash = hash;
    unsigned bucket = node->path_hash & (name_bucket_count - 1);
    node->path_next = name_buckets[bucket];
    name_buckets[bucket] = index + 1;
}

static void index_insert(unsigned index)
{
    index_insert_hashed(index, namespace_hash(nodes[index].path));
}

static void index_remove(unsigned index)
{
    struct node *node = &nodes[index];
    uint32_t *link = &name_buckets[node->path_hash & (name_bucket_count - 1)];
    while (*link && *link != index + 1)
        link = &nodes[*link - 1].path_next;
    if (!*link)
        lume_fail("namespace index removal");
    *link = node->path_next;
    node->path_next = 0;
}

static int find_loaded(const char *p)
{
    if (!name_bucket_count)
        return -LUME_ENOENT;
    uint32_t hash = namespace_hash(p);
    unsigned first = name_buckets[hash & (name_bucket_count - 1)];
    for (unsigned link = first; link; link = nodes[link - 1].path_next)
        if (equal(p, nodes[link - 1].path))
            return (int)link - 1;
    for (unsigned link = first; link; link = nodes[link - 1].path_next)
    {
        unsigned i = link - 1;
        struct node *node = &nodes[i];
        bool same = node->persistent && storage && storage->path_equal && storage->path_hash
                        ? storage->path_equal(p, node->path)
                        : node->on_fat && lume_path_equal_folded(p, node->path);
        if (same)
            return (int)i;
    }
    if (storage && storage->path_equal && !storage->path_hash)
        for (unsigned i = 0; i < count; ++i)
            if (nodes[i].linked && nodes[i].persistent && storage->path_equal(p, nodes[i].path))
                return (int)i;
    return -LUME_ENOENT;
}

static int find(const char *p)
{
    int index = find_loaded(p);
    if (index >= 0 || !storage || !storage->next || !storage->metadata ||
        p[0] != '/' || p[1] != 'd' || p[2] != 'a' || p[3] != 't' || p[4] != 'a' ||
        p[5] != '/' || length(p) >= LUME_FILES_PATH_MAX)
        return index;
    char parent[LUME_FILES_PATH_MAX];
    copy(parent, p, length(p) + 1);
    char *last = strrchr(parent, '/');
    if (!last || last == parent)
        return -LUME_ENOENT;
    *last = 0;
    int base = find(parent);
    if (base < 0)
        return base;
    if (nodes[base].kind != LUME_NODE_DIR)
        return -LUME_ENOTDIR;
    struct lume_stat metadata = {0};
    struct lume_inode_access access;
    int result = storage->metadata(p, &metadata, &access, NULL);
    if (result)
        return result;
    if (metadata.inode <= UINT32_MAX ||
        (metadata.kind != LUME_NODE_DIR && metadata.kind != LUME_NODE_FILE &&
         metadata.kind != LUME_NODE_LINK && metadata.kind != LUME_NODE_PIPE))
        return -LUME_EIO;
    char *owned = lume_path_duplicate(p);
    if (!owned)
        return -LUME_ENOMEM;
    unsigned reserved = reserve_node(owned, metadata.kind == LUME_NODE_DIR ? LUME_NODE_DIR
                                                                           : LUME_NODE_FILE);
    if (reserved == UINT32_MAX)
    {
        lume_path_release(owned);
        return -LUME_ENOSPC;
    }
    struct node *node = &nodes[reserved];
    node->bytes = metadata.size;
    node->persistent = true;
    node->writable = !storage->read_only;
    if (!import_storage_metadata(node, false))
    {
        cancel_node(reserved);
        return -LUME_EIO;
    }
    publish_node(reserved);
    return (int)reserved;
}

/* Resolve components in order so a regular file followed by /.. remains invalid. */
static int resolve_path_with(struct lume_files *f, const char *input, char out[LUME_FILES_PATH_MAX],
                             bool follow_final, bool follow_trailing, unsigned access_flags)
{
    out[0] = 0;
    if (!*input)
        return -LUME_ENOENT;
    if (f->identity.jailed)
    {
        const char *part = input;
        while (*part)
        {
            while (*part == '/')
                ++part;
            const char *end = part;
            while (*end && *end != '/')
                ++end;
            size_t bytes = (size_t)(end - part);
            if (bytes && !(bytes == 1 && part[0] == '.') &&
                !(bytes == 2 && part[0] == '.' && part[1] == '.'))
                return -LUME_ENOENT;
            part = end;
        }
        out[0] = '/';
        out[1] = 0;
        return find("/");
    }
    char raw[LUME_FILES_PATH_MAX * 2];
    const char *cwd = *input == '/' ? "/" : lume_files_cwd(f);
    if (*input != '/' && !cwd)
        return -LUME_ENOENT;
    size_t bytes = length(input);
    if (bytes >= sizeof(raw))
        return -LUME_ENAMETOOLONG;
    copy(raw, input, bytes + 1);
    size_t used = length(cwd);
    copy(out, cwd, used + 1);
    int index = find(out);
    const char *cursor = raw;
    unsigned links = 0;
    while (*cursor)
    {
        while (*cursor == '/')
            ++cursor;
        if (!*cursor)
            break;
        if (index < 0 || nodes[index].kind != LUME_NODE_DIR)
        {
            out[0] = 0;
            return index < 0 ? index : -LUME_ENOTDIR;
        }
        int permission = node_access_with(f, &nodes[index], LUME_ACCESS_EXECUTE, access_flags);
        if (permission)
        {
            out[0] = 0;
            return permission;
        }
        const char *part = cursor;
        while (*cursor && *cursor != '/')
            ++cursor;
        size_t count = (size_t)(cursor - part);
        if (count == 1 && part[0] == '.')
            continue;
        if (count == 2 && part[0] == '.' && part[1] == '.')
        {
            while (used > 1 && out[used - 1] != '/')
                --used;
            if (used > 1)
                --used;
            out[used] = 0;
            index = find(out);
            continue;
        }
        if (count > LUME_FILES_NAME_MAX || used + (used > 1) + count >= LUME_FILES_PATH_MAX)
        {
            out[0] = 0;
            return -LUME_ENAMETOOLONG;
        }
        size_t parent_bytes = used;
        int directory = index;
        if (used > 1)
            out[used++] = '/';
        copy(out + used, part, count);
        used += count;
        out[used] = 0;
        index = find(out);
        if (index >= 0)
        {
            used = length(nodes[index].path);
            copy(out, nodes[index].path, used + 1);
            const char *remaining = cursor;
            while (*remaining == '/')
                ++remaining;
            if (nodes[index].kind == LUME_NODE_LINK &&
                (follow_final || *remaining || (follow_trailing && *cursor)))
            {
                if (++links > 40)
                    return -LUME_ELOOP;
                char *target = lume_kernel_allocate(LUME_FILES_PATH_MAX);
                if (!target)
                    return -LUME_ENOMEM;
                int result = read_node_link(&nodes[index], target, LUME_FILES_PATH_MAX);
                if (result <= 0 || result >= (int)LUME_FILES_PATH_MAX)
                {
                    lume_kernel_free(target);
                    return result < 0 ? result : result ? -LUME_ENAMETOOLONG : -LUME_ENOENT;
                }
                for (int i = 0; i < result; ++i)
                    if (!target[i])
                    {
                        lume_kernel_free(target);
                        return -LUME_EIO;
                    }
                bool absolute = target[0] == '/';
                size_t suffix = length(cursor);
                if ((size_t)result + suffix >= sizeof raw)
                {
                    lume_kernel_free(target);
                    return -LUME_ENAMETOOLONG;
                }
                memmove(raw + result, cursor, suffix + 1);
                copy(raw, target, (size_t)result);
                lume_kernel_free(target);
                cursor = raw;
                used = absolute ? 1 : parent_bytes;
                out[used] = 0;
                index = absolute ? find("/") : directory;
            }
        }
    }
    size_t raw_bytes = length(raw);
    if (index >= 0 && raw_bytes && raw[raw_bytes - 1] == '/')
    {
        if (nodes[index].kind != LUME_NODE_DIR)
            return -LUME_ENOTDIR;
        int permission = node_access_with(f, &nodes[index], LUME_ACCESS_EXECUTE, access_flags);
        if (permission)
            return permission;
    }
    return index;
}

static int resolve_path(struct lume_files *f, const char *input, char out[LUME_FILES_PATH_MAX],
                        bool follow_final, bool follow_trailing)
{
    return resolve_path_with(f, input, out, follow_final, follow_trailing, 0);
}

static int resolve(struct lume_files *f, const char *input, char out[LUME_FILES_PATH_MAX])
{
    return resolve_path(f, input, out, true, true);
}

static unsigned parent_node(const char *path)
{
    if (!path[0] || (path[0] == '/' && !path[1]))
        return 0;
    char parent[LUME_FILES_PATH_MAX];
    copy(parent, path, length(path) + 1);
    char *last = strrchr(parent, '/');
    if (!last)
        return UINT32_MAX;
    if (last == parent)
        parent[1] = 0;
    else
        *last = 0;
    int index = find(parent);
    if (index < 0 || nodes[index].kind != LUME_NODE_DIR)
        return UINT32_MAX;
    return (unsigned)index + 1;
}

static void set_node(unsigned index, char *p, unsigned kind, const void *data, size_t bytes,
                     bool writable)
{
    struct node *n = &nodes[index];
    if (index < count)
        release_node_memory(n);
    *n = (struct node){0};
    n->path = p;
    n->parent = parent_node(p);
    if (n->parent == UINT32_MAX)
        lume_fail("namespace parent missing");
    n->kind = kind;
    n->image = data;
    n->bytes = bytes;
    n->writable = writable;
    n->linked = true;
    index_insert(index);
}

static bool add(const char *p, unsigned kind, const void *data, size_t bytes, bool writable)
{
    char *owned = lume_path_duplicate(p);
    if (!owned)
        return false;
    set_node(count, owned, kind, data, bytes, writable);
    ++count;
    return true;
}

static void add_required(const char *p, unsigned kind, const void *data, size_t bytes,
                         bool writable)
{
    if (!add(p, kind, data, bytes, writable))
        lume_fail("initial namespace allocation");
}

static unsigned available_node(void)
{
    return free_nodes ? free_nodes - 1 : count;
}

static unsigned reserve_node(char *p, unsigned kind)
{
    unsigned index = available_node();
    if (index == node_capacity)
        return UINT32_MAX;
    if (free_nodes)
        free_nodes = nodes[index].path_next;
    nodes[index] = (struct node){.path = p, .kind = kind, .writable = true, .references = 1};
    nodes[index].parent = parent_node(p);
    if (nodes[index].parent == UINT32_MAX)
        lume_fail("namespace parent missing");
    if (index == count)
        ++count;
    return index;
}

static void publish_node(unsigned index)
{
    if (nodes[index].linked || nodes[index].references != 1)
        lume_fail("namespace reservation publication");
    nodes[index].references = 0;
    nodes[index].linked = true;
    index_insert(index);
}

static void cancel_node(unsigned index)
{
    if (nodes[index].linked || nodes[index].references != 1)
        lume_fail("namespace reservation cancellation");
    nodes[index].references = 0;
    release_ram(&nodes[index]);
}

static void block_path(const struct lume_block *b, char out[LUME_FILES_PATH_MAX])
{
    copy(out, "/dev/", 5);
    copy(out + 5, b->device->name, length(b->device->name) + 1);
}

static int add_block(struct lume_block *b)
{
    char p[LUME_FILES_PATH_MAX];
    block_path(b, p);
    if (find(p) >= 0)
        return -LUME_EEXIST;
    unsigned slot;
    for (slot = 8; slot < count; ++slot)
        if (!nodes[slot].linked && !nodes[slot].references && nodes[slot].block)
            break;
    if (slot == count)
    {
        if (count == node_capacity)
            return -LUME_ENOSPC;
        if (!add(p, LUME_NODE_BLOCK, 0, (size_t)(b->sectors * b->sector_bytes), !b->read_only))
            return -LUME_ENOMEM;
    }
    else
    {
        char *owned = lume_path_duplicate(p);
        if (!owned)
            return -LUME_ENOMEM;
        set_node(slot, owned, LUME_NODE_BLOCK, 0, (size_t)(b->sectors * b->sector_bytes),
                 !b->read_only);
    }
    nodes[slot].block = b;
    return 0;
}

static size_t initial_device_nodes(void)
{
    size_t total = 11;
    for (unsigned i = 0; i < LUME_DEVICE_LIMIT; ++i)
    {
        total += block_devices[i] != NULL;
        if (attribute_groups[i])
            total += attribute_groups[i]->count + 1;
        struct lume_devfs_character *device = character_devices[i].device;
        if (!device)
            continue;
        ++total;
        bool first = true;
        for (unsigned j = 0; j < i; ++j)
            if (character_devices[j].device &&
                equal(character_devices[j].device->directory, device->directory))
                first = false;
        total += first;
    }
    return total;
}

static bool reserved_device_directory(const char *name)
{
    return equal(name, "console") || equal(name, "null") || equal(name, "zero") ||
           equal(name, "pts");
}

int lume_devfs_register_block(struct lume_block *b)
{
    if (!b || !b->device || !b->ops || !b->ops->read || !b->sectors || !b->max_sectors ||
        !b->sector_bytes || b->sector_bytes > 4096 || b->sectors > INT64_MAX / b->sector_bytes ||
        (b->device->state != LUME_DEVICE_CLAIMED && b->device->state != LUME_DEVICE_READY))
        return -LUME_EINVAL;
    const char *name = b->device->name;
    if (!name || !*name || length(name) > LUME_NAME_MAX || equal(name, ".") || equal(name, "..") ||
        equal(name, "console"))
        return -LUME_EINVAL;
    for (const char *p = name; *p; ++p)
        if (*p == '/')
            return -LUME_EINVAL;
    if (reserved_device_directory(name))
        return -LUME_EEXIST;
    for (unsigned i = 0; i < LUME_DEVICE_LIMIT; ++i)
        if (character_devices[i].device && equal(character_devices[i].device->directory, name))
            return -LUME_EEXIST;
    for (unsigned i = 0; i < LUME_DEVICE_LIMIT; ++i)
        if (attribute_groups[i] && equal(attribute_groups[i]->name, name))
            return -LUME_EEXIST;
    unsigned slot = LUME_DEVICE_LIMIT;
    for (unsigned i = 0; i < LUME_DEVICE_LIMIT; ++i)
    {
        if (block_devices[i] && equal(block_devices[i]->device->name, name))
            return -LUME_EEXIST;
        if (!block_devices[i] && slot == LUME_DEVICE_LIMIT)
            slot = i;
    }
    if (slot == LUME_DEVICE_LIMIT || (!count && initial_device_nodes() + 1 > node_capacity))
        return -LUME_ENOSPC;
    int error = count ? add_block(b) : 0;
    if (error)
        return error;
    block_devices[slot] = b;
    return 0;
}

int lume_devfs_unregister_block(struct lume_block *b)
{
    for (unsigned i = 0; i < LUME_DEVICE_LIMIT; ++i)
        if (b && block_devices[i] == b)
        {
            for (unsigned j = 0; j < count; ++j)
                if (nodes[j].linked && nodes[j].block == b && nodes[j].references)
                    return -LUME_EBUSY;
            for (unsigned j = 0; j < count; ++j)
                if (nodes[j].linked && nodes[j].block == b)
                {
                    index_remove(j);
                    nodes[j].linked = false;
                    release_ram(&nodes[j]);
                }
            block_devices[i] = 0;
            return 0;
        }
    return b ? 0 : -LUME_EINVAL;
}

static bool attribute_name(const char *name)
{
    if (!name || !*name || length(name) > LUME_FILES_NAME_MAX || equal(name, ".") ||
        equal(name, ".."))
        return false;
    for (const char *p = name; *p; ++p)
        if (*p == '/')
            return false;
    return true;
}

static void character_path(const struct lume_devfs_character *device, bool directory,
                           char path[LUME_FILES_PATH_MAX])
{
    copy(path, "/dev/", 5);
    size_t used = length(device->directory);
    copy(path + 5, device->directory, used + 1);
    if (!directory)
    {
        path[5 + used] = '/';
        copy(path + 6 + used, device->name, length(device->name) + 1);
    }
}

static int add_character(struct lume_devfs_character *device)
{
    char path[LUME_FILES_PATH_MAX], directory[LUME_FILES_PATH_MAX];
    character_path(device, false, path);
    character_path(device, true, directory);
    if (find(path) >= 0)
        return -LUME_EEXIST;
    int parent = find(directory);
    if (parent >= 0 &&
        (nodes[parent].kind != LUME_NODE_DIR || nodes[parent].group || nodes[parent].persistent))
        return -LUME_ENOTDIR;
    size_t available = node_capacity - count;
    for (unsigned entry = free_nodes; entry; entry = nodes[entry - 1].path_next)
        ++available;
    if (available < (parent < 0 ? 2u : 1u))
        return -LUME_ENOSPC;
    char *owned = lume_path_duplicate(path);
    char *owned_directory = parent < 0 ? lume_path_duplicate(directory) : NULL;
    if (!owned || (parent < 0 && !owned_directory))
    {
        lume_path_release(owned);
        lume_path_release(owned_directory);
        return -LUME_ENOMEM;
    }
    if (parent < 0)
    {
        unsigned slot = reserve_node(owned_directory, LUME_NODE_DIR);
        nodes[slot].writable = false;
        publish_node(slot);
    }
    unsigned slot = reserve_node(owned, LUME_NODE_CHAR);
    nodes[slot].character = device;
    publish_node(slot);
    return 0;
}

int lume_devfs_register_character(struct lume_devfs_character *device)
{
    if (!device || !device->open || !attribute_name(device->directory) ||
        !attribute_name(device->name) ||
        length(device->directory) + length(device->name) + 7 > LUME_FILES_PATH_MAX)
        return -LUME_EINVAL;
    if (reserved_device_directory(device->directory))
        return -LUME_EEXIST;
    unsigned slot = LUME_DEVICE_LIMIT;
    bool existing_directory = false;
    for (unsigned i = 0; i < LUME_DEVICE_LIMIT; ++i)
    {
        if ((block_devices[i] && equal(block_devices[i]->device->name, device->directory)) ||
            (attribute_groups[i] && equal(attribute_groups[i]->name, device->directory)))
            return -LUME_EEXIST;
        struct lume_devfs_character *other = character_devices[i].device;
        if (other && equal(other->directory, device->directory))
            existing_directory = true;
        if (other && equal(other->directory, device->directory) && equal(other->name, device->name))
            return -LUME_EEXIST;
        if (!other && slot == LUME_DEVICE_LIMIT)
            slot = i;
    }
    if (slot == LUME_DEVICE_LIMIT ||
        (!count && initial_device_nodes() + 1 + !existing_directory > node_capacity))
        return -LUME_ENOSPC;
    int result = count ? add_character(device) : 0;
    if (!result)
        character_devices[slot].device = device;
    return result;
}

int lume_devfs_unregister_character(struct lume_devfs_character *device)
{
    if (!device)
        return -LUME_EINVAL;
    for (unsigned i = 0; i < LUME_DEVICE_LIMIT; ++i)
        if (character_devices[i].device == device)
        {
            if (character_devices[i].opening)
                return -LUME_EBUSY;
            for (unsigned j = 0; j < count; ++j)
                if (nodes[j].character == device && nodes[j].linked)
                {
                    index_remove(j);
                    nodes[j].linked = false;
                    nodes[j].character = NULL;
                    release_ram(&nodes[j]);
                }
            character_devices[i].device = NULL;
            return 0;
        }
    return 0;
}

static void group_path(const struct lume_devfs_group *group, const char *name,
                       char out[LUME_FILES_PATH_MAX])
{
    copy(out, "/dev/", 5);
    size_t used = 5;
    copy(out + used, group->name, length(group->name) + 1);
    used += length(group->name);
    if (name)
    {
        out[used++] = '/';
        copy(out + used, name, length(name) + 1);
    }
}

static int add_group(struct lume_devfs_group *group)
{
    char path[LUME_FILES_PATH_MAX];
    group_path(group, NULL, path);
    if (find(path) >= 0)
        return -LUME_EEXIST;
    size_t available = node_capacity - count;
    for (unsigned entry = free_nodes; entry; entry = nodes[entry - 1].path_next)
        ++available;
    if (available < group->count + 1)
        return -LUME_ENOSPC;
    char *paths[LUME_DEVFS_ATTRIBUTES + 1] = {0};
    for (size_t i = 0; i <= group->count; ++i)
    {
        group_path(group, i ? group->attributes[i - 1].name : NULL, path);
        paths[i] = lume_path_duplicate(path);
        if (!paths[i])
        {
            for (size_t j = 0; j < i; ++j)
                lume_path_release(paths[j]);
            return -LUME_ENOMEM;
        }
    }
    /* Allocate every path before publishing the directory and its attributes. */
    for (size_t i = 0; i <= group->count; ++i)
    {
        const struct lume_devfs_attribute *attribute = i ? &group->attributes[i - 1] : NULL;
        unsigned slot = reserve_node(paths[i], attribute ? LUME_NODE_CHAR : LUME_NODE_DIR);
        nodes[slot].writable = attribute && attribute->write;
        nodes[slot].group = group;
        nodes[slot].attribute = attribute;
        publish_node(slot);
    }
    return 0;
}

int lume_devfs_register_group(struct lume_devfs_group *group)
{
    if (!group || !attribute_name(group->name) || !group->attributes || !group->count ||
        group->count > LUME_DEVFS_ATTRIBUTES)
        return -LUME_EINVAL;
    if (reserved_device_directory(group->name))
        return -LUME_EEXIST;
    for (size_t i = 0; i < group->count; ++i)
    {
        const struct lume_devfs_attribute *attribute = &group->attributes[i];
        if (!attribute_name(attribute->name) || !attribute->read ||
            7 + length(group->name) + length(attribute->name) > LUME_FILES_PATH_MAX)
            return -LUME_EINVAL;
        for (size_t j = 0; j < i; ++j)
            if (equal(attribute->name, group->attributes[j].name))
                return -LUME_EEXIST;
    }
    unsigned slot = LUME_DEVICE_LIMIT;
    size_t initial_nodes = initial_device_nodes() + group->count + 1;
    for (unsigned i = 0; i < LUME_DEVICE_LIMIT; ++i)
    {
        if (character_devices[i].device &&
            equal(character_devices[i].device->directory, group->name))
            return -LUME_EEXIST;
        if (block_devices[i] && equal(block_devices[i]->device->name, group->name))
            return -LUME_EEXIST;
        if (attribute_groups[i])
        {
            if (equal(attribute_groups[i]->name, group->name))
                return -LUME_EEXIST;
        }
        else if (slot == LUME_DEVICE_LIMIT)
            slot = i;
    }
    if (slot == LUME_DEVICE_LIMIT || (!count && initial_nodes > node_capacity))
        return -LUME_ENOSPC;
    int result = count ? add_group(group) : 0;
    if (!result)
        attribute_groups[slot] = group;
    return result;
}

int lume_devfs_can_unregister_group(struct lume_devfs_group *group)
{
    if (!group)
        return -LUME_EINVAL;
    for (unsigned i = 0; i < count; ++i)
        if (nodes[i].group == group && nodes[i].references)
            return -LUME_EBUSY;
    return 0;
}

int lume_devfs_unregister_group(struct lume_devfs_group *group)
{
    int result = lume_devfs_can_unregister_group(group);
    if (result)
        return result;
    for (unsigned i = 0; i < LUME_DEVICE_LIMIT; ++i)
        if (attribute_groups[i] == group)
        {
            for (unsigned j = 0; j < count; ++j)
                if (nodes[j].linked && nodes[j].group == group)
                {
                    index_remove(j);
                    nodes[j].linked = false;
                    release_ram(&nodes[j]);
                    nodes[j].group = NULL;
                    nodes[j].attribute = NULL;
                }
            attribute_groups[i] = NULL;
            return 0;
        }
    return 0;
}

#include "resource_limits.h"

void lume_vfs_init(const void *shell, size_t bytes)
{
    if (mapping_changed)
        mapping_changed(0, 0, 0);
    (void)program_cache_clear();
    if (storage && storage->program_cache_owner)
        storage->program_cache_owner(NULL);
    if (storage_handles())
        for (unsigned i = 0; i < OPEN_FILES; ++i)
            if ((open_file_count(open_file_at(i)) || open_file_at(i)->deferred_close) &&
                open_file_at(i)->storage_handle)
                (void)storage->close(open_file_at(i)->storage_handle);
    for (unsigned task = 0; task < LUME_POLL_TASK_LIMIT; ++task)
        lume_vfs_poll_release(task);
    memset(poll_pending, 0, sizeof(poll_pending));
    memset(poll_subscribed, 0, sizeof(poll_subscribed));
    memset(poll_subscriptions, 0, sizeof(poll_subscriptions));
    console_source = (struct lume_poll_source){0};
    lume_poll_source_reset(&console_source);
    poll_input_ready = false;
    lume_file_modes_reset();
    file_lock_count = 0;
    open_file_reset();
    storage_reap_count = storage_reap_head = storage_reap_tail = 0;
    storage_reap_enabled = storage_reaping = false;
    for (unsigned i = 0; i < channel_slots(); ++i)
        release_channel(&channels[i]);
#if LUME_FILES_DYNAMIC
    lume_kernel_free(channels);
    channels = NULL;
#endif
    for (unsigned i = 0; i < count; ++i)
    {
        release_node_memory(&nodes[i]);
        nodes[i] = (struct node){0};
    }
    count = 0;
    free_nodes = 0;
    if (!name_buckets)
    {
        name_bucket_count = 512;
        name_buckets = lume_kernel_allocate(name_bucket_count * sizeof(*name_buckets));
        if (!name_buckets)
            lume_fail("namespace index allocation");
    }
    memset(name_buckets, 0, (size_t)name_bucket_count * sizeof(*name_buckets));
    if (namespace_generation == UINT32_MAX)
        lume_fail("namespace generation exhausted");
    ++namespace_generation;
    root_fat = (struct lume_fat){0};
    storage = 0;
    storage_mount_flags = 0;
    for (unsigned i = 0; i < RAM_FILES; ++i)
        ram_used[i] = false;
    add_required("/", LUME_NODE_DIR, 0, 0, false);
    add_required("/dev", LUME_NODE_DIR, 0, 0, false);
    add_required("/bin", LUME_NODE_DIR, 0, 0, false);
    add_required("/etc", LUME_NODE_DIR, 0, 0, false);
    add_required("/tmp", LUME_NODE_DIR, 0, 0, true);
    nodes[count - 1].mode = 01777;
    nodes[count - 1].mode_set = true;
    add_required("/dev/console", LUME_NODE_CHAR, 0, 0, true);
    add_required("/dev/null", LUME_NODE_CHAR, 0, 0, true);
    nodes[count - 1].device = 1;
    add_required("/dev/zero", LUME_NODE_CHAR, 0, 0, true);
    nodes[count - 1].device = 2;
#if LUME_FILES_DYNAMIC
    static const char *const standard_devices[STANDARD_DEVICE_COUNT] = {
        "/dev/stdin", "/dev/stdout", "/dev/stderr"};
    for (unsigned i = 0; i < STANDARD_DEVICE_COUNT; ++i)
    {
        add_required(standard_devices[i], LUME_NODE_CHAR, 0, 0, false);
        nodes[count - 1].device = STANDARD_DEVICE_BASE + i;
    }
#endif
    add_required("/bin/sh", LUME_NODE_FILE, shell, bytes, false);
    static const char motd[] =
#if defined(LUME_ARCH_I686)
        "LumeOS i686 / PCI PC\nNative ABI 1.0. COM1 and PS/2 console.\n";
#elif defined(LUME_ARCH_AMD64)
        "LumeOS AMD64 / QEMU q35\nNative ABI 1.0. COM1 serial console.\n";
#else
        "LumeOS AArch64 / Raspberry Pi 5\nNative ABI 1.0. GPIO UART console.\n";
#endif
    add_required("/etc/motd", LUME_NODE_FILE, motd, sizeof(motd) - 1, false);
    add_required("/dev/pts", LUME_NODE_DIR, 0, 0, false);
    for (unsigned i = 0; i < LUME_DEVICE_LIMIT; ++i)
        if (block_devices[i])
            (void)add_block(block_devices[i]);
    for (unsigned i = 0; i < LUME_DEVICE_LIMIT; ++i)
        if (attribute_groups[i] && add_group(attribute_groups[i]))
            lume_fail("device attribute namespace allocation");
    for (unsigned i = 0; i < LUME_DEVICE_LIMIT; ++i)
        if (character_devices[i].device && add_character(character_devices[i].device))
            lume_fail("character device namespace allocation");
    resource_limits_start();
}

static int new_object(int node, int pipe, uint32_t flags)
{
    int index = open_file_reserve();
    if (index < 0)
        return index;
    struct open_file *file = open_file_at((unsigned)index);
    file->node = node;
    file->pipe = pipe;
    file->flags = flags;
    if (node >= 0)
        ++nodes[node].references;
    if (pipe >= 0)
    {
        struct channel *channel = &channels[pipe];
        if ((flags & 3) != LUME_O_WRONLY)
        {
            ++channel->readers;
            ++channel->reader_epoch;
        }
        if ((flags & 3) != LUME_O_RDONLY)
        {
            ++channel->writers;
            ++channel->writer_epoch;
        }
    }
    return index;
}

static bool files_ready(const struct lume_files *f)
{
    if (f->cwd_generation != namespace_generation)
        return false;
#if LUME_FILES_DYNAMIC
    return f->fd != NULL;
#else
    (void)f;
    return true;
#endif
}

static struct node *current_directory(const struct lume_files *files)
{
    return files->cwd_node && files->cwd_node <= count &&
                   files->cwd_generation == namespace_generation
               ? &nodes[files->cwd_node - 1]
               : NULL;
}

const char *lume_files_cwd(const struct lume_files *files)
{
    if (files->cwd_generation != namespace_generation)
        return NULL;
    struct node *node = current_directory(files);
    if (files->cwd_node)
        return node && node->linked ? node->path : NULL;
    return files->cwd;
}

static void drop_directory(struct lume_files *files)
{
    struct node *node = current_directory(files);
    files->cwd_node = files->cwd_generation = 0;
    if (node)
    {
        if (!node->references)
            lume_fail("directory reference release");
        --node->references;
        release_ram(node);
    }
}

static void hold_directory(struct lume_files *files, unsigned index)
{
    ++nodes[index].references;
    drop_directory(files);
    files->cwd_node = index + 1;
    files->cwd_generation = namespace_generation;
    size_t bytes = length(nodes[index].path) + 1;
    if (bytes <= sizeof(files->cwd))
        copy(files->cwd, nodes[index].path, bytes);
    else
    {
        files->cwd[0] = '/';
        files->cwd[1] = 0;
    }
}

int lume_files_copy_cwd(struct lume_files *out, const struct lume_files *in)
{
    if (out == in)
        return 0;
    struct node *node = current_directory(in);
    if (in->cwd_node && !node)
        return -116;
    if (node)
        hold_directory(out, in->cwd_node - 1);
    else
    {
        drop_directory(out);
        copy(out->cwd, in->cwd, sizeof(out->cwd));
        out->cwd_generation = namespace_generation;
    }
    return 0;
}

bool lume_files_init(struct lume_files *f)
{
    *f = (struct lume_files){0};
    f->cwd_generation = namespace_generation;
    f->identity.umask = 022;
    for (unsigned i = 0; i < LUME_LIMIT_COUNT; ++i)
        f->identity.limit[i].soft = f->identity.limit[i].hard = UINT64_MAX;
    f->identity.limit[LUME_LIMIT_FILES].soft = nofile_default;
    f->identity.limit[LUME_LIMIT_FILES].hard = LUME_FD_MAX;
    f->identity.limit[LUME_LIMIT_PROCESSES].soft = f->identity.limit[LUME_LIMIT_PROCESSES].hard =
        LUME_TASK_LIMIT;
    f->identity.limit[LUME_LIMIT_CORE_SIZE].soft = f->identity.limit[LUME_LIMIT_CORE_SIZE].hard = 0;
    f->cwd[0] = '/';
#if LUME_FILES_DYNAMIC
    f->fd = lume_kernel_allocate(sizeof(*f->fd) * LUME_FD_MAX);
    if (!f->fd)
        return false;
    memset(f->fd, 0, sizeof(*f->fd) * LUME_FD_MAX);
#endif
    for (unsigned i = 0; i < LUME_FD_MAX; ++i)
        f->fd[i].object = -1;
    for (unsigned i = 0; i < 3; ++i)
    {
        int object = new_object(5, -1, i ? LUME_O_WRONLY : LUME_O_RDONLY);
        if (object < 0)
        {
            lume_files_release(f);
            return false;
        }
        bind_descriptor(f, i, object, 0);
    }
    if (count)
        hold_directory(f, 0);
    return true;
}

bool lume_files_clone(struct lume_files *out, const struct lume_files *in)
{
    if (out == in)
        return false;
    if (!files_ready(in))
    {
        *out = (struct lume_files){0};
        return false;
    }
    *out = (struct lume_files){0};
#if !LUME_FILES_DYNAMIC
    for (unsigned i = 0; i < LUME_FD_MAX; ++i)
        out->fd[i].object = -1;
#endif
#if LUME_FILES_DYNAMIC
    if (!in->fd)
        return false;
    struct lume_fd *table = lume_kernel_allocate(sizeof(*table) * LUME_FD_MAX);
    if (!table)
        return false;
#endif
    struct lume_directory_authorities *authorities = NULL;
    if (in->directory_authorities)
    {
        authorities = lume_kernel_allocate(sizeof(*authorities));
        if (!authorities)
        {
#if LUME_FILES_DYNAMIC
            lume_kernel_free(table);
#endif
            return false;
        }
        memcpy(authorities, in->directory_authorities, sizeof(*authorities));
    }
    *out = *in;
    out->directory_authorities = authorities;
    out->cwd_node = out->cwd_generation = 0;
#if LUME_FILES_DYNAMIC
    out->fd = table;
    memcpy(out->fd, in->fd, sizeof(*table) * LUME_FD_MAX);
#endif
    if (lume_files_copy_cwd(out, in))
    {
        lume_kernel_free(out->directory_authorities);
        out->directory_authorities = NULL;
#if LUME_FILES_DYNAMIC
        lume_kernel_free(out->fd);
        out->fd = NULL;
#else
        for (unsigned i = 0; i < LUME_FD_MAX; ++i)
            out->fd[i].object = -1;
#endif
        return false;
    }
    for (unsigned i = 0; i < LUME_FD_MAX; ++i)
    {
        out->fd[i].changed = (struct lume_poll_source){0};
        lume_poll_source_reset(&out->fd[i].changed);
        if (out->fd[i].object >= 0)
            open_file_retain(open_file_at(out->fd[i].object));
        else if (out->fd[i].object == FD_FIFO_PENDING)
            out->fd[i].object = -1;
    }
    if (authorities)
    {
        for (unsigned i = 0; i < LUME_SANDBOX_DIRECTORY_LIMIT; ++i)
            if (authorities->entry[i].active)
                open_file_retain(open_file_at(authorities->entry[i].object));
        if (authorities->generation == namespace_generation && authorities->x11_active)
            open_file_retain(open_file_at(authorities->x11_object));
    }
    return true;
}

bool lume_files_inherit(struct lume_files *out, const struct lume_files *in)
{
    if (!lume_files_clone(out, in))
        return false;
    for (unsigned i = 0; i < LUME_FD_MAX; ++i)
        if (out->fd[i].flags & LUME_FD_CLOEXEC)
            (void)lume_vfs_close(out, i);
    return true;
}

void lume_files_move(struct lume_files *out, struct lume_files *in)
{
    if (out == in)
        return;
    *out = *in;
    in->cwd_node = in->cwd_generation = 0;
    in->directory_authorities = NULL;
#if LUME_FILES_DYNAMIC
    in->fd = NULL;
#else
    for (unsigned i = 0; i < LUME_FD_MAX; ++i)
        in->fd[i].object = -1;
#endif
}

#if LUME_FILES_DYNAMIC
static void release_descriptor_table(void *table)
{
    lume_kernel_free(table);
}
#endif

void lume_files_release(struct lume_files *f)
{
    struct lume_directory_authorities *authorities = f->directory_authorities;
    f->directory_authorities = NULL;
    if (authorities)
    {
        if (authorities->generation == namespace_generation)
        {
            for (unsigned i = 0; i < LUME_SANDBOX_DIRECTORY_LIMIT; ++i)
                if (authorities->entry[i].active)
                    lume_vfs_drop(authorities->entry[i].object);
            if (authorities->x11_active)
                lume_vfs_drop(authorities->x11_object);
        }
        lume_kernel_free(authorities);
    }
#if LUME_FILES_DYNAMIC
    if (!f->fd)
    {
        drop_directory(f);
        return;
    }
#endif
    unsigned closed = 0;
    for (unsigned i = 0; i < LUME_FD_MAX; ++i)
    {
        if (f->fd[i].object < 0)
            continue;
        (void)close_deferred(f, i);
        if (++closed == 16)
        {
            closed = 0;
            lume_worker_checkpoint(0);
        }
    }
    drop_directory(f);
#if LUME_FILES_DYNAMIC
    lume_poll_sources_retire_range(f->fd, sizeof(*f->fd) * LUME_FD_MAX);
    struct lume_fd *table = f->fd;
    f->fd = NULL;
    lume_worker_compute(release_descriptor_table, table);
#endif
}

uint64_t lume_files_pages(const struct lume_files *f)
{
    uint64_t pages = 0;
    if (f->directory_authorities)
    {
        uint64_t prefix = (uintptr_t)f->directory_authorities & UINT64_C(4095);
        pages = (prefix + sizeof(*f->directory_authorities) + 4095) / 4096;
    }
#if LUME_FILES_DYNAMIC
    if (!f->fd)
        return pages;
    uint64_t prefix = (uintptr_t)f->fd & UINT64_C(4095);
    return pages + (prefix + sizeof(*f->fd) * LUME_FD_MAX + 4095) / 4096;
#else
    return pages;
#endif
}

static struct open_file *descriptor(struct lume_files *f, uint64_t fd)
{
    return fd < LUME_FD_MAX &&
#if LUME_FILES_DYNAMIC
                   f->fd &&
#endif
                   f->fd[fd].object >= 0
               ? open_file_at(f->fd[fd].object)
               : 0;
}

bool lume_vfs_console_io(struct lume_files *files, uint64_t fd)
{
    struct open_file *file = descriptor(files, fd);
    return file && open_file_count(file) && !file->external && file->pipe < 0 &&
           nodes[file->node].kind == LUME_NODE_CHAR && !nodes[file->node].block &&
           !nodes[file->node].group && nodes[file->node].device <= 2;
}

bool lume_vfs_console_terminal(struct lume_files *files, uint64_t fd)
{
    return lume_vfs_console_io(files, fd) && !nodes[descriptor(files, fd)->node].device;
}

bool lume_vfs_pipe_io(struct lume_files *files, uint64_t fd)
{
    struct open_file *file = descriptor(files, fd);
    return file && open_file_count(file) && !file->external && file->pipe >= 0;
}

/* Admission and the following resident copy require the scheduler lock. */
int64_t lume_vfs_pipe_accept(struct lume_files *files, uint64_t fd, uint64_t bytes, bool writing)
{
    struct open_file *file = descriptor(files, fd);
    if (!file || !open_file_count(file) || file->external || file->pipe < 0 ||
        (writing ? !(file->flags & 3) : (file->flags & 3) == LUME_O_WRONLY))
        return -LUME_EBADF;
    struct channel *channel = &channels[file->pipe];
    if (!bytes)
        return 0;
    if (channel->growing || channel->copying)
        return -LUME_EAGAIN;
    if (writing)
    {
        if (!channel->readers)
            return -LUME_EPIPE;
        unsigned space = channel->capacity - channel->count;
        if (!space || (bytes <= LUME_PIPE_ATOMIC && bytes > space))
            return -LUME_EAGAIN;
        return (int64_t)(bytes < space ? bytes : space);
    }
    if (!channel->count)
        return channel->writers ? -LUME_EAGAIN : 0;
    return (int64_t)(bytes < channel->count ? bytes : channel->count);
}

void lume_vfs_pipe_grow(struct lume_files *files, uint64_t fd, uint64_t bytes)
{
#if LUME_FILES_DYNAMIC
    struct open_file *file = descriptor(files, fd);
    if (!file || file->external || file->pipe < 0 || !(file->flags & 3))
        return;
    struct channel *channel = channels + file->pipe;
    if (channel->growing || channel->copying || bytes <= channel->capacity ||
        channel->capacity >= PIPE_MAX_PAGES * 4096u)
        return;
    unsigned pages =
        (unsigned)((bytes < PIPE_MAX_PAGES * 4096u ? bytes : PIPE_MAX_PAGES * 4096u) + 4095u) /
        4096u;
    if (pages > pipe_page_limit - pipe_pages)
        return;
    int held = lume_vfs_hold(files, (unsigned)fd);
    if (held < 0)
        return;
    channel->growing = true;
    pipe_pages += pages;
    uint64_t memory = lume_pages_alloc_span_uninitialized(pages);
    if (memory)
    {
        unsigned first = channel->capacity - channel->head;
        if (first > channel->count)
            first = channel->count;
        lume_worker_copy((void *)(uintptr_t)memory, channel->data + channel->head, first);
        lume_worker_copy((unsigned char *)(uintptr_t)memory + first, channel->data,
                         channel->count - first);
        pipe_backing_release(channel);
        channel->data = (void *)(uintptr_t)memory;
        channel->capacity = pages * 4096u;
        channel->head = 0;
    }
    else
        pipe_pages -= pages;
    channel->growing = false;
    lume_wait_wake(&channel->readable);
    lume_wait_wake(&channel->writable);
    lume_poll_source_notify(&channel->changed);
    lume_vfs_drop(held);
#else
    (void)files;
    (void)fd;
    (void)bytes;
#endif
}

bool lume_vfs_memory_io(struct lume_files *files, uint64_t fd)
{
    struct open_file *file = descriptor(files, fd);
    return file && open_file_count(file) &&
           (file->external ? file->external->memory_io : file->pipe >= 0);
}

unsigned lume_vfs_memory_lane(struct lume_files *files, uint64_t fd)
{
    struct open_file *file = descriptor(files, fd);
    if (!file || !open_file_count(file))
        return 0;
    uintptr_t identity = file->pipe >= 0 ? (uintptr_t)file->pipe : (uintptr_t)file->context >> 4;
    return (unsigned)(identity ^ (identity >> 12)) & 3u;
}

bool lume_vfs_storage_io(struct lume_files *files, uint64_t fd)
{
    struct open_file *file = descriptor(files, fd);
    return file && open_file_count(file) && !file->external && file->pipe < 0 &&
           nodes[file->node].kind == LUME_NODE_FILE && nodes[file->node].persistent &&
           !nodes[file->node].block;
}

bool lume_vfs_exec_callbacks(const struct lume_files *files)
{
#if LUME_FILES_DYNAMIC
    if (!files->fd)
        return false;
#endif
    for (unsigned i = 0; i < LUME_FD_MAX; ++i)
    {
        int object = files->fd[i].object;
        if (object >= 0 && (files->fd[i].flags & LUME_FD_CLOEXEC) && open_file_at(object)->external)
            return true;
    }
    return false;
}

int lume_vfs_attach(struct lume_files *files, const struct lume_file_ops *ops, void *context,
                    unsigned flags)
{
#if LUME_FILES_DYNAMIC
    if (!files->fd)
        return -LUME_ENOMEM;
#endif
    unsigned fd = 0;
    while (fd < files->identity.limit[LUME_LIMIT_FILES].soft && files->fd[fd].object != -1)
        ++fd;
    if (fd >= files->identity.limit[LUME_LIMIT_FILES].soft)
        return -LUME_EMFILE;
    int object = new_object(-1, -1, LUME_O_RDWR);
    if (object < 0)
        return object;
    open_file_at(object)->external = ops;
    open_file_at(object)->context = context;
    open_file_at(object)->status = flags & LUME_FD_NONBLOCK;
    bind_descriptor(files, fd, object, flags & LUME_FD_CLOEXEC);
    return (int)fd;
}

void *lume_vfs_external(struct lume_files *files, unsigned fd, const struct lume_file_ops *ops)
{
    struct open_file *object = descriptor(files, fd);
    return object && object->external == ops ? object->context : 0;
}

const struct lume_vm_dma_pin *lume_vfs_memory(struct lume_files *files, unsigned fd, unsigned *mode)
{
    struct open_file *object = descriptor(files, fd);
    if (!object || !object->external || !object->external->memory || !mode)
        return NULL;
    *mode = object->flags & 3u;
    return object->external->memory(object->context);
}

int lume_vfs_hold(struct lume_files *files, unsigned fd)
{
    struct fd_guard guard = fd_enter(files);
    struct open_file *object = descriptor(files, fd);
    int result = -LUME_EBADF;
    if (object)
        result = open_file_acquire(object) ? files->fd[fd].object : -LUME_EOVERFLOW;
    fd_leave(guard);
    return result;
}

int lume_vfs_borrow(struct lume_files *files, unsigned fd, const struct lume_file_ops *ops,
                    void **context)
{
    struct fd_guard guard = fd_enter(files);
    struct open_file *file = descriptor(files, fd);
    int result = -LUME_EBADF;
    if (file && context && lume_ref_get(&file->borrowers))
    {
        result = files->fd[fd].object;
        *context = file->context;
    }
    fd_leave(guard);
    if (result >= 0 && file->external != ops)
    {
        lume_vfs_drop_independent(result);
        return -LUME_EBADF;
    }
    return result;
}

int lume_vfs_install(struct lume_files *files, int object)
{
#if LUME_FILES_DYNAMIC
    if (!files->fd)
        return -LUME_ENOMEM;
#endif
    if (object < 0 || object >= (int)OPEN_FILES || !open_file_count(open_file_at(object)))
        return -LUME_EBADF;
    for (unsigned i = 0; i < files->identity.limit[LUME_LIMIT_FILES].soft; ++i)
        if (files->fd[i].object == -1)
        {
            bind_descriptor(files, i, object, 0);
            return (int)i;
        }
    return -LUME_EMFILE;
}

int lume_vfs_directory_authority(struct lume_files *files, unsigned operation, uint64_t fd)
{
    if (operation < LUME_SANDBOX_DIRECTORY_MINT || operation > LUME_SANDBOX_DIRECTORY_RELEASE)
        return -LUME_EINVAL;
    unsigned profile = files->identity.sandbox_profile;
    if ((operation == LUME_SANDBOX_DIRECTORY_MINT && profile != LUME_SANDBOX_NONE) ||
        (operation != LUME_SANDBOX_DIRECTORY_MINT && profile != LUME_SANDBOX_NETWORK))
        return -LUME_EPERM;
    if (!files_ready(files))
        return -LUME_EBADF;
    struct open_file *file = descriptor(files, fd);
    if (!file)
        return -LUME_EBADF;
    if (file->external || file->pipe >= 0 || file->node < 0 ||
        nodes[file->node].kind != LUME_NODE_DIR ||
        (!nodes[file->node].linked && operation != LUME_SANDBOX_DIRECTORY_RELEASE))
        return -LUME_ENOTDIR;
    if (operation == LUME_SANDBOX_DIRECTORY_MINT)
    {
        file->directory_authority_generation = namespace_generation;
        return 0;
    }
    if (file->directory_authority_generation != namespace_generation)
        return -LUME_EPERM;
    struct lume_directory_authorities *authorities = files->directory_authorities;
    if (!authorities && operation == LUME_SANDBOX_DIRECTORY_ACTIVATE)
    {
        authorities = lume_kernel_allocate(sizeof(*authorities));
        if (!authorities)
            return -LUME_ENOMEM;
        memset(authorities, 0, sizeof(*authorities));
        authorities->generation = namespace_generation;
        files->directory_authorities = authorities;
    }
    if (!authorities || authorities->generation != namespace_generation)
        return -LUME_EINVAL;
    unsigned slot = LUME_SANDBOX_DIRECTORY_LIMIT;
    for (unsigned i = 0; i < LUME_SANDBOX_DIRECTORY_LIMIT; ++i)
    {
        struct directory_authority *entry = &authorities->entry[i];
        if (!entry->active)
        {
            if (slot == LUME_SANDBOX_DIRECTORY_LIMIT)
                slot = i;
            continue;
        }
        if (open_file_at(entry->object)->node != file->node)
            continue;
        if (operation == LUME_SANDBOX_DIRECTORY_RELEASE)
        {
            int object = entry->object;
            *entry = (struct directory_authority){0};
            lume_vfs_drop(object);
            return 0;
        }
        return 0;
    }
    if (operation == LUME_SANDBOX_DIRECTORY_RELEASE)
        return -LUME_EINVAL;
    if (slot == LUME_SANDBOX_DIRECTORY_LIMIT)
        return -LUME_ENOSPC;
    int object = lume_vfs_hold(files, (unsigned)fd);
    if (object < 0)
        return object;
    authorities->entry[slot] = (struct directory_authority){.object = object, .active = true};
    return 0;
}

int lume_vfs_x11_authority(struct lume_files *files, uint64_t fd)
{
    if (files->identity.sandbox_profile != LUME_SANDBOX_NONE)
        return -LUME_EPERM;
    if (fd > UINT32_MAX || !files_ready(files))
        return -LUME_EBADF;
    struct open_file *file = descriptor(files, fd);
    if (!file)
        return -LUME_EBADF;
    if (file->external || file->pipe >= 0 || file->node < 0 || (file->flags & 3) ||
        nodes[file->node].kind != LUME_NODE_FILE || !nodes[file->node].linked)
        return -LUME_EPERM;
    struct lume_directory_authorities *authorities = files->directory_authorities;
    if (authorities && authorities->generation != namespace_generation)
        return -LUME_EINVAL;
    if (authorities && authorities->x11_active)
        return open_file_at(authorities->x11_object)->node == file->node ? 0 : -LUME_EBUSY;
    int object = lume_vfs_hold(files, (unsigned)fd);
    if (object < 0)
        return object;
    if (!authorities)
    {
        authorities = lume_kernel_allocate(sizeof(*authorities));
        if (!authorities)
        {
            lume_vfs_drop(object);
            return -LUME_ENOMEM;
        }
        memset(authorities, 0, sizeof(*authorities));
        authorities->generation = namespace_generation;
        files->directory_authorities = authorities;
    }
    authorities->x11_object = object;
    authorities->x11_active = true;
    return 0;
}

bool lume_vfs_sandbox_x11_authority(struct lume_files *files, const char *canonical)
{
    struct lume_directory_authorities *authorities = files->directory_authorities;
    if (files->identity.sandbox_profile != LUME_SANDBOX_GPU_SOFTWARE || !authorities ||
        authorities->generation != namespace_generation || !authorities->x11_active ||
        !files_ready(files))
        return false;
    int index = find(canonical);
    return index >= 0 && nodes[index].linked && nodes[index].kind == LUME_NODE_FILE &&
           open_file_at(authorities->x11_object)->node == index;
}

static bool directory_contains(unsigned anchor, unsigned index)
{
    for (unsigned depth = 0; depth < count; ++depth)
    {
        struct node *node = &nodes[index];
        if (!node->linked || (node->kind != LUME_NODE_DIR && node->kind != LUME_NODE_FILE))
            return false;
        if (index == anchor)
            return node->kind == LUME_NODE_DIR;
        if (!node->parent || node->parent > count)
            return false;
        index = node->parent - 1;
        if (nodes[index].kind != LUME_NODE_DIR)
            return false;
    }
    return false;
}

bool lume_vfs_sandbox_directory(struct lume_files *files, const char *canonical, bool parent)
{
    struct lume_directory_authorities *authorities = files->directory_authorities;
    if (files->identity.sandbox_profile != LUME_SANDBOX_NETWORK || !authorities ||
        authorities->generation != namespace_generation || !files_ready(files))
        return false;
    int index = find(canonical);
    if (index >= 0 && nodes[index].kind != LUME_NODE_DIR && nodes[index].kind != LUME_NODE_FILE)
        return false;
    if (parent)
    {
        unsigned directory = index >= 0 ? nodes[index].parent : parent_node(canonical);
        if (!directory || directory > count)
            return false;
        index = (int)directory - 1;
    }
    if (index < 0)
        return false;
    for (unsigned i = 0; i < LUME_SANDBOX_DIRECTORY_LIMIT; ++i)
    {
        struct directory_authority *entry = &authorities->entry[i];
        if (entry->active &&
            directory_contains((unsigned)open_file_at(entry->object)->node, (unsigned)index))
            return true;
    }
    return false;
}

static int sandbox_mutation(struct lume_files *files, const char *canonical, bool parent)
{
    return !files->identity.sandbox_profile || lume_vfs_sandbox_directory(files, canonical, parent)
               ? 0
               : -LUME_EPERM;
}

static bool privileged_device_io(const struct node *node, bool writing)
{
    return node->block || (node->kind == LUME_NODE_CHAR && !node->device && !node->attribute) ||
           (writing && node->group);
}

/* Persist cleared set-ID bits before modifying the file data. */
static int clear_file_setid(struct node *node)
{
    if (!node->linked && native_storage(node))
        for (unsigned i = 0; i < count; ++i)
            if (nodes[i].linked && same_backing(node, &nodes[i]))
            {
                node = &nodes[i];
                break;
            }
    return node->mode & 06000u ? set_node_mode(node, node->mode & ~06000u) : 0;
}

int lume_vfs_sandbox_access(struct lume_files *files, uint64_t fd, bool writing)
{
    unsigned profile = files->identity.sandbox_profile;
    if (!profile)
        return 0;
    struct open_file *object = descriptor(files, fd);
    if (!object)
        return 0;
    if (profile != LUME_SANDBOX_NETWORK && fd <= UINT32_MAX &&
        lume_socket_network_io(files, (unsigned)fd))
        return -LUME_EPERM;
    unsigned kind = object->external    ? object->external->kind
                    : object->pipe >= 0 ? LUME_NODE_PIPE
                                        : nodes[object->node].kind;
    bool inert = !object->external && object->pipe < 0 &&
                 (nodes[object->node].device == 1 || nodes[object->node].device == 2);
    if (kind == LUME_NODE_BLOCK || (kind == LUME_NODE_CHAR && !inert &&
                                    (!writing || (!lume_vfs_console_io(files, fd) &&
                                                  !lume_tty_slave_output(files, (unsigned)fd)))))
        return -LUME_EPERM;
    return 0;
}

static int standard_descriptor(const struct node *node)
{
    unsigned fd = node->device - STANDARD_DEVICE_BASE;
    return fd < STANDARD_DEVICE_COUNT ? (int)fd : -1;
}

static int standard_descriptor_access(struct lume_files *files, unsigned fd, unsigned requested)
{
    struct open_file *source = descriptor(files, fd);
    if (!source)
        return -LUME_ENOENT;
    unsigned access = source->flags & 3;
    if ((requested & LUME_ACCESS_EXECUTE) ||
        ((requested & LUME_ACCESS_READ) && access == LUME_O_WRONLY) ||
        ((requested & LUME_ACCESS_WRITE) && access == LUME_O_RDONLY))
        return -LUME_EACCES;
    int permission = requested & LUME_ACCESS_READ ? lume_vfs_sandbox_access(files, fd, false) : 0;
    if (!permission && (requested & LUME_ACCESS_WRITE))
        permission = lume_vfs_sandbox_access(files, fd, true);
    return permission;
}

static int create_node(struct lume_files *files, const char *path, unsigned kind, unsigned mode,
                       const char *target)
{
    unsigned parent = parent_node(path);
    if (!parent || parent == UINT32_MAX)
        return -LUME_ENOENT;
    const struct node *base = &nodes[parent - 1];
    int result = sandbox_mutation(files, path, true);
    if (!result)
        result = node_access(files, base, LUME_ACCESS_WRITE | LUME_ACCESS_EXECUTE);
    if (result)
        return result;
    if (!base->writable)
        return -LUME_EROFS;
    bool persistent = base->persistent;
    bool symbolic = kind == LUME_NODE_LINK;
    bool atomic_mode = persistent && (symbolic ? storage->symlink != NULL : storage->create_owned != NULL);
    if (symbolic && persistent && !atomic_mode)
        return -LUME_EOPNOTSUPP;
    if (persistent && !atomic_mode &&
        (kind == LUME_NODE_PIPE || files->identity.euid ||
         (mode != UINT32_MAX && mode & 07000u)))
        return -LUME_EOPNOTSUPP;
    bool directory = kind == LUME_NODE_DIR;
    int64_t parent_mode = atomic_mode || !persistent ? node_mode(base) : 0;
    if (parent_mode < 0)
        return (int)parent_mode;
    if (available_node() == node_capacity)
        return -LUME_ENOSPC;
    char *owned = lume_path_duplicate(path);
    if (!owned)
        return -LUME_ENOMEM;
    unsigned reserved = reserve_node(owned, kind);
    if (reserved == UINT32_MAX)
    {
        lume_path_release(owned);
        return -LUME_ENOSPC;
    }
    struct node *node = &nodes[reserved];
    node->persistent = persistent;
    if (kind == LUME_NODE_PIPE || symbolic)
        node->links = 1;
    if (symbolic)
        node->bytes = length(target);
    if (atomic_mode || !persistent)
    {
        unsigned permissions = mode != UINT32_MAX ? mode : directory ? 0755u : 0644u;
        permissions = symbolic ? 0777u : permissions & ~files->identity.umask;
        unsigned gid = files->identity.egid;
        if (parent_mode & 02000)
        {
            gid = base->gid;
            if (directory)
                permissions |= 02000;
        }
        const struct lume_inode_access access = {files->identity.euid, gid, permissions};
        if (symbolic && !persistent)
        {
            node->data = (unsigned char *)lume_path_duplicate(target);
            if (!node->data)
                result = -LUME_ENOMEM;
        }
        else if (symbolic)
            result = storage->symlink(path, target, &access, &node->storage_inode);
        else if (atomic_mode)
            result = storage->create_owned(path, kind, &access, &node->storage_inode);
        node->uid = access.uid;
        node->gid = access.gid;
        node->mode = access.mode;
        node->mode_set = true;
    }
    else
    {
        result = storage->create(path, directory);
        if (!result && native_storage(node) && !import_storage_metadata(node, true))
            result = -LUME_EIO;
    }
    if (result)
    {
        cancel_node(reserved);
        return result;
    }
    publish_node(reserved);
    if (persistent && !atomic_mode && mode != UINT32_MAX)
    {
        result = set_node_mode(node, mode & ~files->identity.umask);
        if (result)
            return result;
    }
    return (int)reserved;
}

static int64_t open_fifo(struct lume_files *files, unsigned fd, int index, uint32_t flags,
                         unsigned descriptor_flags, struct lume_fifo_open *pending)
{
    struct node *node = &nodes[index];
    unsigned slot = 0, slots = node->fifo_identity ? channel_slots() : 0;
    while (slot < slots && channels[slot].fifo_identity != node->fifo_identity)
        ++slot;
    struct channel *channel = slot < slots ? &channels[slot] : NULL;
    unsigned mode = flags & 3;
    bool writing = mode == LUME_O_WRONLY;
    bool peer = channel && (writing ? channel->readers : channel->writers);
    bool nonblocking = descriptor_flags & LUME_FD_NONBLOCK;
    bool wait = mode != LUME_O_RDWR && !peer && !nonblocking;
    if (writing && nonblocking && !peer)
        return -LUME_ENXIO;
    if (wait && !pending)
        return -LUME_EAGAIN;
    if (!channel)
    {
        int allocated = allocate_channel();
        if (allocated < 0)
            return allocated;
        slot = (unsigned)allocated;
        channel = &channels[slot];
    }
    else
        ++channel->opening;
    uint64_t peer_epoch = writing ? channel->reader_epoch : channel->writer_epoch;
    int object = new_object(index, (int)slot, flags);
    --channel->opening;
    if (object < 0)
    {
        if (!channel->readers && !channel->writers && !channel->opening)
            release_channel(channel);
        return object;
    }
    if (!node->fifo_identity)
    {
        if (!++fifo_sequence)
            lume_fail("FIFO identity exhausted");
        uint64_t identity = fifo_sequence;
        for (unsigned i = 0; i < count; ++i)
            if (&nodes[i] != node && nodes[i].linked && same_backing(node, &nodes[i]))
                nodes[i].fifo_identity = identity;
        node->fifo_identity = identity;
    }
    channel->fifo_identity = node->fifo_identity;
    struct open_file *description = open_file_at((unsigned)object);
    description->status = descriptor_flags & LUME_FD_NONBLOCK;
    description->fifo_writer_epoch = channel->writer_epoch - (channel->writers != 0);
    lume_wait_wake(&channel->readable);
    lume_wait_wake(&channel->writable);
    lume_poll_source_notify(&channel->changed);
    if (wait)
    {
        /* The reservation remains invisible to descriptor lookups until a peer opens. */
        bind_descriptor(files, fd, FD_FIFO_PENDING, 0);
        *pending = (struct lume_fifo_open){
            .peer_epoch = peer_epoch,
            .object = (unsigned)object + 1u,
            .fd = fd,
            .flags = descriptor_flags & LUME_FD_CLOEXEC,
        };
        return LUME_IO_WAIT_FIFO;
    }
    bind_descriptor(files, fd, object, descriptor_flags & LUME_FD_CLOEXEC);
    return fd;
}

static int64_t open_mode(struct lume_files *f, const char *input, uint64_t flags, unsigned mode,
                         bool compat, unsigned descriptor_flags, struct lume_fifo_open *pending,
                         bool *shared)
{
    int permission = lume_sandbox_open(f, input, flags);
    if (permission)
        return permission;
#if LUME_FILES_DYNAMIC
    if (!f->fd)
        return -LUME_ENOMEM;
#endif
    if (terminal_open)
    {
        int fd = terminal_open(f, input, flags & ~(uint64_t)LUME_FILES_OPEN_NOFOLLOW);
        if (fd != -LUME_ENOSYS)
            return fd;
    }
    const uint64_t allowed =
        3 | LUME_O_CREAT | LUME_O_TRUNC | LUME_O_APPEND | LUME_O_EXCL | LUME_FILES_OPEN_NOFOLLOW;
    if ((flags & ~allowed) || (flags & 3) == 3 ||
        (!(flags & 3) && (flags & (LUME_O_TRUNC | LUME_O_APPEND))))
        return -LUME_EINVAL;
    unsigned fd = 0;
    while (fd < f->identity.limit[LUME_LIMIT_FILES].soft && f->fd[fd].object != -1)
        ++fd;
    if (fd >= f->identity.limit[LUME_LIMIT_FILES].soft)
        return -LUME_EMFILE;
    char p[LUME_FILES_PATH_MAX] = {0};
    bool nofollow = (flags & LUME_FILES_OPEN_NOFOLLOW) ||
                    ((flags & (LUME_O_CREAT | LUME_O_EXCL)) == (LUME_O_CREAT | LUME_O_EXCL));
    int index = resolve_path(f, input, p, !nofollow, true);
    if (index >= 0 && (flags & LUME_O_CREAT) && (flags & LUME_O_EXCL))
        return -LUME_EEXIST;
    if (index >= 0 && nodes[index].kind == LUME_NODE_LINK)
        return -LUME_ELOOP;
    flags &= ~(uint64_t)LUME_FILES_OPEN_NOFOLLOW;
    bool created = index == -LUME_ENOENT && (flags & LUME_O_CREAT) && p[0];
    if (created)
    {
        if (input[length(input) - 1] == '/')
            return -LUME_ENOENT;
        index = create_node(f, p, LUME_NODE_FILE, mode, NULL);
    }
    if (index < 0)
        return index;
    struct node *n = &nodes[index];
    unsigned access_mode = (unsigned)flags & 3;
    unsigned requested = access_mode == LUME_O_RDONLY   ? LUME_ACCESS_READ
                         : access_mode == LUME_O_WRONLY ? LUME_ACCESS_WRITE
                                                        : LUME_ACCESS_READ | LUME_ACCESS_WRITE;
    int standard = standard_descriptor(n);
    if (standard >= 0)
    {
        permission = standard_descriptor_access(f, (unsigned)standard, requested);
        if (permission)
            return permission;
        if (shared)
            *shared = true;
        return lume_vfs_dup2(f, (unsigned)standard, fd);
    }
    if ((n->persistent && (storage_mount_flags & LUME_VFS_MOUNT_NODEV) &&
         (n->kind == LUME_NODE_BLOCK || n->kind == LUME_NODE_CHAR)) ||
        (f->identity.euid && privileged_device_io(n, access_mode != LUME_O_RDONLY)) ||
        (n->group && access_mode != LUME_O_RDONLY && f->identity.jailed))
        return -LUME_EACCES;
    if (n->block && n->block->device->state != LUME_DEVICE_READY)
        return -LUME_EIO;
    if ((flags & 3) && n->kind == LUME_NODE_DIR)
        return -LUME_EISDIR;
    permission = created ? 0 : node_access(f, n, requested);
    if (permission)
        return permission;
    if (n->kind == LUME_NODE_PIPE)
        return open_fifo(f, fd, index, (uint32_t)flags, descriptor_flags, pending);
    if (compat && access_mode != LUME_O_RDONLY && n->kind == LUME_NODE_FILE && n->references)
        return -LUME_EBUSY;
    if ((flags & 3) && !n->writable)
        return -LUME_EROFS;
    if ((flags & 3) && executable_backing(n))
        return -LUME_ETXTBSY;
    if (!created && (flags & LUME_O_TRUNC) && n->kind == LUME_NODE_FILE)
    {
        int error = clear_file_setid(n);
        if (error)
            return error;
        if (n->persistent)
        {
            int e = storage->truncate(p);
            if (e)
                return e;
        }
        n->bytes = 0;
        node_changed(n);
        native_aliases(n, true, false);
    }
    if (n->character)
    {
        struct lume_devfs_character *device = n->character;
        for (unsigned i = 0; i < LUME_DEVICE_LIMIT; ++i)
            if (character_devices[i].device == device)
            {
                if (character_devices[i].opening == UINT32_MAX)
                    return -LUME_EBUSY;
                ++character_devices[i].opening;
                int result = device->open(device->context, f, flags);
                --character_devices[i].opening;
                return result;
            }
        return -LUME_EIO;
    }
    int object = new_object(index, -1, (uint32_t)flags);
    if (object < 0)
        return object;
    if (n->persistent && n->kind == LUME_NODE_FILE && storage_handles())
    {
        int result =
            storage->open(n->path, (unsigned)(flags & 3), &open_file_at(object)->storage_handle);
        if (result)
        {
            --n->references;
            open_file_recycle(open_file_at((unsigned)object));
            return result;
        }
    }
    bind_descriptor(f, (unsigned)fd, object, 0);
    return fd;
}

int64_t lume_vfs_open(struct lume_files *files, const char *input, uint64_t flags)
{
    return lume_vfs_open_wait(files, input, flags, false, NULL);
}

int64_t lume_vfs_open_wait(struct lume_files *files, const char *input, uint64_t flags, bool compat,
                           struct lume_fifo_open *pending)
{
    return open_mode(files, input, flags, UINT32_MAX, compat, 0, pending, NULL);
}

static int finish_object(struct open_file *d)
{
    int result = 0;
    if (d->external)
        d->external->close(d->context);
    else if (d->pipe >= 0)
    {
        struct channel *c = &channels[d->pipe];
        if ((d->flags & 3) != LUME_O_RDONLY)
            --c->writers;
        if ((d->flags & 3) != LUME_O_WRONLY)
            --c->readers;
        lume_wait_wake(&c->readable);
        lume_wait_wake(&c->writable);
        lume_poll_source_notify(&c->changed);
        if (!c->readers && !c->writers && !c->opening)
            release_channel(c);
    }
    else
    {
        if (d->storage_handle)
        {
            result = storage->close(d->storage_handle);
            d->storage_handle = 0;
        }
    }
    if (d->node >= 0)
    {
        --nodes[d->node].references;
        release_ram(&nodes[d->node]);
    }
    open_file_recycle(d);
    return result;
}

static int retire_description(struct open_file *d, bool deferred)
{
    if (deferred && storage_reap_enabled && !d->external && d->pipe < 0 && d->storage_handle)
    {
        /* The description retains its node and handle until STORAGE finishes closing it. */
        d->deferred_close = true;
        d->next_reap = 0;
        if (storage_reap_tail)
            open_file_at(storage_reap_tail - 1)->next_reap = d->index + 1;
        else
            storage_reap_head = d->index + 1;
        storage_reap_tail = d->index + 1;
        ++storage_reap_count;
        return 0;
    }
    return finish_object(d);
}

static int release_object(struct open_file *d, bool deferred)
{
    unsigned previous = open_file_release(d);
    if (!previous)
        return -LUME_EBADF;
    if (previous > 1)
        return 0;
    --open_file_live;
    /* Descriptor references and independent borrows retire separately. The last
     * borrow keeps callbacks and provider state alive until scheduler entry. */
    d->closing = true;
    file_locks_drop((int)d->index);
    if (lume_ref_block_count(&d->borrowers))
        return 0;
    return retire_description(d, deferred);
}

struct lume_wait_queue *lume_vfs_fifo_open_queue(const struct lume_fifo_open *pending)
{
    if (!pending->object)
        return NULL;
    struct open_file *file = open_file_at(pending->object - 1);
    struct channel *channel = &channels[file->pipe];
    return (file->flags & 3) == LUME_O_WRONLY ? &channel->writable : &channel->readable;
}

int64_t lume_vfs_fifo_open_resume(struct lume_files *files, struct lume_fifo_open *pending)
{
    if (!pending->object)
        return -LUME_EBADF;
    int object = (int)pending->object - 1;
    const struct open_file *file = open_file_at((unsigned)object);
    const struct channel *channel = &channels[file->pipe];
    uint64_t epoch =
        (file->flags & 3) == LUME_O_WRONLY ? channel->reader_epoch : channel->writer_epoch;
    if (epoch == pending->peer_epoch)
        return LUME_IO_WAIT_FIFO;
    unsigned fd = pending->fd;
    bind_descriptor(files, fd, object, pending->flags);
    *pending = (struct lume_fifo_open){0};
    return fd;
}

void lume_vfs_fifo_open_cancel(struct lume_files *files, struct lume_fifo_open *pending)
{
    if (!pending->object)
        return;
    bind_descriptor(files, pending->fd, -1, 0);
    (void)release_object(open_file_at(pending->object - 1), true);
    *pending = (struct lume_fifo_open){0};
}

void lume_vfs_reap_start(void)
{
    storage_reap_enabled = true;
}

bool lume_vfs_reap_pending(void)
{
    return storage_reap_count != 0;
}

unsigned lume_vfs_reap(unsigned budget)
{
    if (storage_reaping || !budget || !storage_reap_count)
        return 0;
    storage_reaping = true;
    unsigned done = 0;
    while (done < budget && storage_reap_count)
    {
        struct open_file *d = open_file_at(storage_reap_head - 1);
        storage_reap_head = d->next_reap;
        if (!storage_reap_head)
            storage_reap_tail = 0;
        d->next_reap = 0;
        d->deferred_close = false;
        --storage_reap_count;
        (void)finish_object(d);
        ++done;
    }
    storage_reaping = false;
    return done;
}

void lume_vfs_drop(int object)
{
    if (object >= 0 && object < (int)OPEN_FILES)
        (void)release_object(open_file_at(object), true);
}

static void drop_description(void *context)
{
    struct open_file *file = context;
    if (open_file_count(file) || lume_ref_count(&file->borrowers) || !file->closing)
        lume_fail("open description borrow retirement");
    (void)retire_description(file, true);
}

void lume_vfs_drop_independent(int object)
{
    if (object < 0)
        return;
    struct open_file *file = open_file_at((unsigned)object);
    uint32_t state = __atomic_load_n(&file->borrowers.state, __ATOMIC_ACQUIRE);
    for (;;)
    {
        if (!(state & ~LUME_REF_CLOSED))
            lume_fail("open description borrow release");
        if (__atomic_compare_exchange_n(&file->borrowers.state, &state, state - 1, true,
                                        __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
            break;
    }
    if (state == (LUME_REF_CLOSED | 1u))
        lume_worker_scheduler(drop_description, file);
}

int64_t lume_vfs_close(struct lume_files *f, uint64_t fd)
{
    struct open_file *d = descriptor(f, fd);
    if (!d)
        return -LUME_EBADF;
    bind_descriptor(f, (unsigned)fd, -1, 0);
    return release_object(d, false);
}

static int64_t close_deferred(struct lume_files *f, uint64_t fd)
{
    struct open_file *d = descriptor(f, fd);
    if (!d)
        return -LUME_EBADF;
    bind_descriptor(f, (unsigned)fd, -1, 0);
    return release_object(d, true);
}

int64_t lume_vfs_open_compat(struct lume_files *f, const char *input, uint64_t flags)
{
    return lume_vfs_open_wait(f, input, flags, true, NULL);
}

int64_t lume_vfs_remove_compat(struct lume_files *f, const char *input, bool directory)
{
    char p[LUME_FILES_PATH_MAX];
    int index = resolve(f, input, p);
    if (index >= 0 && nodes[index].writable && nodes[index].references)
        return -LUME_EBUSY;
    return lume_vfs_remove(f, input, directory);
}

/* Byte requests use whole-sector transfers. Partial writes preserve the bytes
 * outside the requested range with a read/modify/write in serialized context. */
static int64_t block_io(struct open_file *d, struct node *n, void *data, uint64_t bytes,
                        bool writing)
{
    struct lume_block *b = n->block;
    if (b->device->state != LUME_DEVICE_READY)
        return -LUME_EIO;
    if (writing && b->read_only)
        return -LUME_EROFS;
    if (!bytes)
        return 0;
    if (d->offset >= n->bytes)
        return writing ? -LUME_ENOSPC : 0;
    if (bytes > n->bytes - d->offset)
        bytes = n->bytes - d->offset;
    if (writing)
    {
        int error = program_cache_clear();
        if (error)
            return error;
    }
    unsigned char sector[4096];
    unsigned char *p = data;
    uint64_t done = 0;
    while (done < bytes)
    {
        uint64_t first = d->offset / b->sector_bytes;
        unsigned within = (unsigned)(d->offset % b->sector_bytes);
        uint64_t remaining = bytes - done;
        unsigned batch = 0;
        if (!within && remaining >= b->sector_bytes)
        {
            uint64_t sectors = remaining / b->sector_bytes;
            batch = sectors > b->max_sectors ? b->max_sectors : (unsigned)sectors;
        }
        uint64_t amount;
        int error;
        if (batch)
        {
            amount = (uint64_t)batch * b->sector_bytes;
            error = writing ? lume_block_write(b, first, batch, p + done)
                            : lume_block_read(b, first, batch, p + done);
        }
        else
        {
            amount = b->sector_bytes - within;
            if (amount > remaining)
                amount = remaining;
            error = lume_block_read(b, first, 1, sector);
            if (!error)
            {
                if (writing)
                {
                    copy(sector + within, p + done, (size_t)amount);
                    error = lume_block_write(b, first, 1, sector);
                }
                else
                    copy(p + done, sector + within, (size_t)amount);
            }
        }
        if (error)
            return done ? (int64_t)done : error;
        d->offset += amount;
        done += amount;
    }
    return (int64_t)done;
}

static int64_t read_description(struct lume_files *f, struct open_file *d, void *data,
                                uint64_t bytes)
{
    if (!d || (d->flags & 3) == LUME_O_WRONLY)
        return -LUME_EBADF;
    if (d->external)
        return d->external->read(d->context, data, bytes);
    if (d->pipe < 0 && ((f->identity.jailed && !nodes[d->node].device) ||
                        (f->identity.euid && privileged_device_io(&nodes[d->node], false))))
        return -LUME_EACCES;
    if (d->pipe >= 0)
    {
        struct channel *c = &channels[d->pipe];
        if (!bytes)
            return 0;
        if (c->growing || c->copying)
            return -LUME_EAGAIN;
        if (!c->count)
            return c->writers ? -LUME_EAGAIN : 0;
        if (bytes > c->count)
            bytes = c->count;
        unsigned first = c->capacity - c->head;
        if (first > bytes)
            first = (unsigned)bytes;
        open_file_retain(d);
        c->copying = true;
        lume_worker_copy(data, c->data + c->head, first);
        lume_worker_copy((unsigned char *)data + first, c->data, (size_t)bytes - first);
        c->head = (c->head + (unsigned)bytes) % c->capacity;
        c->count -= (unsigned)bytes;
        c->copying = false;
        lume_wait_wake(&c->readable);
        lume_wait_wake(&c->writable);
        lume_poll_source_notify(&c->changed);
        (void)release_object(d, true);
        return (int64_t)bytes;
    }
    struct node *n = &nodes[d->node];
    if (n->block)
        return block_io(d, n, data, bytes, false);
    if (n->kind == LUME_NODE_DIR)
        return -LUME_EISDIR;
    if (!bytes)
        return 0;
    if (n->attribute)
    {
        if (!d->attribute_cached)
        {
            int result =
                n->attribute->read(n->group->context, d->attribute_data, sizeof(d->attribute_data));
            if (result < 0)
                return result;
            if ((unsigned)result > sizeof(d->attribute_data))
                return -LUME_EIO;
            d->attribute_bytes = (unsigned)result;
            d->attribute_cached = true;
        }
        uint64_t available = d->offset < d->attribute_bytes ? d->attribute_bytes - d->offset : 0;
        if (bytes > available)
            bytes = available;
        if (bytes)
            copy(data, d->attribute_data + d->offset, (size_t)bytes);
        d->offset += bytes;
        return (int64_t)bytes;
    }
    if (n->kind == LUME_NODE_CHAR)
    {
        if (n->device)
        {
            if (n->device == 1)
                return 0;
            memset(data, 0, (size_t)bytes);
            return (int64_t)bytes;
        }
        lume_vfs_poll_input(false);
        int c = lume_console_getc();
        if (c < 0)
            return c;
        *(unsigned char *)data = (unsigned char)c;
        return 1;
    }
    uint64_t available = d->offset < n->bytes ? n->bytes - d->offset : 0;
    if (bytes > available)
        bytes = available;
    if (bytes && n->persistent)
    {
        int r = d->storage_handle
                    ? storage->read_handle(d->storage_handle, d->offset, data, (size_t)bytes)
                    : storage->read(n->path, d->offset, data, (size_t)bytes);
        if (r < 0)
            return r;
        bytes = (uint64_t)r;
    }
    else if (bytes && n->on_fat)
    {
        if (lume_fat_read_cursor(&root_fat, n->fat_file, d->offset, data, (size_t)bytes,
                                 &d->fat_cursor))
            return -LUME_EIO;
    }
    else if (bytes)
        copy(data, (n->image ? n->image : n->data) + d->offset, (size_t)bytes);
    d->offset += bytes;
    return (int64_t)bytes;
}

int64_t lume_vfs_read(struct lume_files *f, uint64_t fd, void *data, uint64_t bytes)
{
    int permission = lume_vfs_sandbox_access(f, fd, false);
    if (permission)
        return permission;
    return read_description(f, descriptor(f, fd), data, bytes);
}

static int64_t write_description(struct lume_files *f, struct open_file *d, const void *data,
                                 uint64_t bytes)
{
    if (!d || !(d->flags & 3))
        return -LUME_EBADF;
    if (d->external)
    {
        if (bytes && (d->flags & LUME_O_APPEND) && d->external->kind == LUME_NODE_FILE)
        {
            if (!d->external->seek)
                return -LUME_ESPIPE;
            int64_t previous = d->external->seek(d->context, 0, LUME_SEEK_CUR);
            if (previous < 0)
                return previous;
            int64_t position = d->external->seek(d->context, 0, LUME_SEEK_END);
            if (position < 0)
                return position;
            int64_t result = d->external->write(d->context, data, bytes);
            if (result <= 0)
            {
                int64_t restored = d->external->seek(d->context, previous, LUME_SEEK_SET);
                if (restored < 0)
                    return restored;
            }
            return result;
        }
        return d->external->write(d->context, data, bytes);
    }
    if (d->pipe < 0 && ((f->identity.jailed && !nodes[d->node].device) ||
                        (f->identity.euid && privileged_device_io(&nodes[d->node], true))))
        return -LUME_EACCES;
    if (d->pipe >= 0)
    {
        struct channel *c = &channels[d->pipe];
        if (!bytes)
            return 0;
        if (!c->readers)
            return -LUME_EPIPE;
        if (c->growing || c->copying)
            return -LUME_EAGAIN;
        unsigned space = c->capacity - c->count;
        if (!space || (bytes <= LUME_PIPE_ATOMIC && bytes > space))
            return -LUME_EAGAIN;
        if (bytes > space)
            bytes = space;
        unsigned end = (c->head + c->count) % c->capacity;
        unsigned first = c->capacity - end;
        if (first > bytes)
            first = (unsigned)bytes;
        open_file_retain(d);
        c->copying = true;
        lume_worker_copy(c->data + end, data, first);
        lume_worker_copy(c->data, (const unsigned char *)data + first, (size_t)bytes - first);
        c->count += (unsigned)bytes;
        c->copying = false;
        lume_wait_wake(&c->readable);
        lume_wait_wake(&c->writable);
        lume_poll_source_notify(&c->changed);
        (void)release_object(d, true);
        return (int64_t)bytes;
    }
    struct node *n = &nodes[d->node];
    if (n->block)
        return block_io(d, n, (void *)data, bytes, true);
    if (n->attribute)
    {
        if (!n->attribute->write)
            return -LUME_EROFS;
        if (!bytes)
            return 0;
        if (bytes > LUME_DEVFS_ATTRIBUTE_BYTES)
            return -LUME_E2BIG;
        int result = n->attribute->write(n->group->context, data, (size_t)bytes);
        if (result)
            return result < 0 ? result : -LUME_EIO;
        d->attribute_cached = false;
        d->offset = 0;
        return (int64_t)bytes;
    }
    if (n->kind == LUME_NODE_CHAR)
    {
        if (n->device)
            return (int64_t)bytes;
        if (bytes)
            lume_console_write(data, (size_t)bytes);
        return (int64_t)bytes;
    }
    if (!n->writable)
        return -LUME_EROFS;
    if (executable_backing(n))
        return -LUME_ETXTBSY;
    if (!bytes)
        return 0;
    uint64_t offset = d->flags & LUME_O_APPEND ? n->bytes : d->offset;
    if (offset > f->identity.limit[LUME_LIMIT_FILE_SIZE].soft ||
        bytes > f->identity.limit[LUME_LIMIT_FILE_SIZE].soft - offset)
        return -27;
    int error = clear_file_setid(n);
    if (error)
        return error;
    if (n->persistent)
    {
        int r = d->storage_handle
                    ? storage->write_handle(d->storage_handle, offset, data, (size_t)bytes)
                    : storage->write(n->path, offset, data, (size_t)bytes);
        if (r < 0)
            return r;
        if (r)
        {
            d->offset = offset + (uint64_t)r;
            if (d->offset > n->bytes)
                n->bytes = d->offset;
            node_changed(n);
            native_aliases(n, true, false);
        }
        return r;
    }
    if (offset > CAPACITY || bytes > CAPACITY - offset)
        return -LUME_ENOSPC;
    if (!n->data)
    {
        for (unsigned i = 0; i < RAM_FILES; ++i)
            if (!ram_used[i])
            {
                ram_used[i] = true;
                n->data = ram_data[i];
                break;
            }
        if (!n->data)
            return -LUME_ENOSPC;
    }
    while (n->bytes < offset)
        n->data[n->bytes++] = 0;
    copy(n->data + offset, data, (size_t)bytes);
    d->offset = offset + bytes;
    if (d->offset > n->bytes)
        n->bytes = d->offset;
    node_changed(n);
    return (int64_t)bytes;
}

int64_t lume_vfs_write(struct lume_files *f, uint64_t fd, const void *data, uint64_t bytes)
{
    int permission = lume_vfs_sandbox_access(f, fd, true);
    if (permission)
        return permission;
    return write_description(f, descriptor(f, fd), data, bytes);
}

bool lume_vfs_write_user(struct lume_files *files, uint64_t fd, unsigned pid, uint64_t address,
                         uint64_t bytes, int64_t *result)
{
    if (!result)
        return false;
    struct open_file *file = descriptor(files, fd);
    if (!file || !file->external || !file->external->write_user)
        return false;
    *result = lume_vfs_sandbox_access(files, fd, true);
    if (*result)
        return true;
    if (!(file->flags & 3))
    {
        *result = -LUME_EBADF;
        return true;
    }
    int held = lume_vfs_hold(files, (unsigned)fd);
    if (held < 0)
    {
        *result = held;
        return true;
    }
    *result = file->external->write_user(file->context, pid, address, bytes);
    lume_vfs_drop(held);
    return true;
}

static int positioned_description(struct lume_files *files, uint64_t fd, uint64_t bytes,
                                  int64_t offset, bool writing, struct open_file *positioned)
{
    struct open_file *description = descriptor(files, fd);
    if (!description ||
        (writing ? !(description->flags & 3) : (description->flags & 3) == LUME_O_WRONLY))
        return -LUME_EBADF;
    if (offset < 0 || bytes > LUME_FILE_IO_MAX || bytes > INT64_MAX - (uint64_t)offset)
        return -LUME_EINVAL;
    if (description->pipe >= 0)
        return -LUME_ESPIPE;
    if (description->external)
    {
        open_file_snapshot(positioned, description);
        positioned->offset = (uint64_t)offset;
        return 0;
    }
    struct node *node = &nodes[description->node];
    if (node->kind == LUME_NODE_DIR)
        return -LUME_EISDIR;
    if (node->kind != LUME_NODE_FILE && !node->block)
        return -LUME_ESPIPE;
    /* A transfer owns its cursor while duplicated descriptors keep their shared offset. */
    open_file_snapshot(positioned, description);
    positioned->offset = (uint64_t)offset;
    positioned->flags &= ~LUME_O_APPEND;
    return 0;
}

int64_t lume_vfs_pread(struct lume_files *files, uint64_t fd, void *data, uint64_t bytes,
                       int64_t offset)
{
    int permission = lume_vfs_sandbox_access(files, fd, false);
    if (permission)
        return permission;
    struct open_file positioned;
    int result = positioned_description(files, fd, bytes, offset, false, &positioned);
    if (result)
        return result;
    if (positioned.external)
        return positioned.external->pread
                   ? positioned.external->pread(positioned.context, data, bytes, (uint64_t)offset)
                   : -LUME_ESPIPE;
    return read_description(files, &positioned, data, bytes);
}

int64_t lume_vfs_pwrite(struct lume_files *files, uint64_t fd, const void *data, uint64_t bytes,
                        int64_t offset)
{
    int permission = lume_vfs_sandbox_access(files, fd, true);
    if (permission)
        return permission;
    struct open_file positioned;
    int result = positioned_description(files, fd, bytes, offset, true, &positioned);
    if (result)
        return result;
    if (positioned.external)
        return positioned.external->pwrite
                   ? positioned.external->pwrite(positioned.context, data, bytes, (uint64_t)offset)
                   : -LUME_ESPIPE;
    return write_description(files, &positioned, data, bytes);
}

int64_t lume_vfs_sync(struct lume_files *f, uint64_t fd)
{
    int permission = lume_vfs_sandbox_access(f, fd, true);
    if (permission)
        return permission;
    struct open_file *d = descriptor(f, fd);
    if (!d)
        return -LUME_EBADF;
    if (d->pipe >= 0 || d->external)
        return -LUME_EINVAL;
    struct node *n = &nodes[d->node];
    if (d->storage_handle)
        return storage->sync_handle(d->storage_handle);
    return n->block ? lume_block_sync(n->block) : 0;
}

int64_t lume_vfs_ofd_lock(struct lume_files *files, bool query,
                          struct lume_file_lock_request *request)
{
    if (request->version != LUME_FILE_LOCK_VERSION || request->bytes != sizeof(*request) ||
        request->reserved || request->reserved2 || request->pid ||
        request->whence > LUME_SEEK_END || request->type < LUME_FILE_LOCK_READ ||
        request->type > LUME_FILE_LOCK_UNLOCK || (query && request->type == LUME_FILE_LOCK_UNLOCK))
        return -LUME_EINVAL;
    struct open_file *description = descriptor(files, (uint64_t)request->fd);
    if (!description)
        return -LUME_EBADF;
    if (description->pipe >= 0 || description->external)
        return -95;
    struct node *node = &nodes[description->node];
    if (node->kind == LUME_NODE_DIR)
        return -LUME_EISDIR;
    if (!query &&
        ((request->type == LUME_FILE_LOCK_READ && (description->flags & 3) == LUME_O_WRONLY) ||
         (request->type == LUME_FILE_LOCK_WRITE && !(description->flags & 3))))
        return -LUME_EBADF;
    uint64_t base = request->whence == LUME_SEEK_SET   ? 0
                    : request->whence == LUME_SEEK_CUR ? description->offset
                                                       : node->bytes;
    struct file_lock_record range = {
        .owner = (int)description->index, .node = description->node, .type = request->type};
    int result = file_lock_range(request, base, &range);
    return result ? result : file_locks_apply(range, query, request);
}

int64_t lume_vfs_flock(struct lume_files *files, struct lume_file_lock_request *request)
{
    if (request->version != LUME_FILE_LOCK_VERSION || request->bytes != sizeof(*request) ||
        request->reserved || request->reserved2 || request->pid || request->whence ||
        request->start || request->length || request->type < LUME_FILE_LOCK_READ ||
        request->type > LUME_FILE_LOCK_UNLOCK)
        return -LUME_EINVAL;
    struct open_file *description = descriptor(files, (uint64_t)request->fd);
    if (!description)
        return -LUME_EBADF;
    if (description->pipe >= 0 || description->external)
        return -95;
    if (nodes[description->node].kind == LUME_NODE_DIR)
        return -LUME_EISDIR;
    int owner = (int)description->index;
    /* Conversion releases only this description's whole-file lock. */
    for (unsigned i = 0; i < file_lock_count;)
        if (file_locks[i].scope == 2 && file_locks[i].owner == owner)
            file_locks[i] = file_locks[--file_lock_count];
        else
            ++i;
    struct file_lock_record range = {.first = 0,
                                     .last = INT64_MAX,
                                     .owner = owner,
                                     .node = description->node,
                                     .type = request->type,
                                     .scope = 2};
    return file_locks_apply(range, false, request);
}

#include "process_locks.h"

int64_t lume_vfs_seek(struct lume_files *f, uint64_t fd, int64_t offset, uint64_t whence)
{
    int permission = lume_vfs_sandbox_access(f, fd, true);
    if (permission)
        return permission;
    struct open_file *d = descriptor(f, fd);
    if (!d)
        return -LUME_EBADF;
    if (d->external)
        return d->external->seek ? d->external->seek(d->context, offset, (unsigned)whence)
                                 : -LUME_ESPIPE;
    if (d->pipe >= 0)
        return -LUME_ESPIPE;
    struct node *n = &nodes[d->node];
    if (n->attribute && !whence && !offset)
    {
        d->offset = 0;
        d->attribute_cached = false;
        return 0;
    }
    if (n->kind == LUME_NODE_CHAR)
    {
        if (n->device)
        {
            if (whence > LUME_SEEK_END)
                return -LUME_EINVAL;
            d->offset = 0;
            return 0;
        }
        return -LUME_ESPIPE;
    }
    uint64_t base = whence == 0 ? 0 : whence == 1 ? d->offset : n->bytes;
    if (whence > 2 || (offset < 0 && (uint64_t)(-(offset + 1)) + 1 > base) ||
        (offset > 0 && (uint64_t)offset > INT64_MAX - base))
        return -LUME_EINVAL;
    d->offset = base + (uint64_t)offset;
    return (int64_t)d->offset;
}

int64_t lume_vfs_stat(struct lume_files *f, uint64_t fd, struct lume_stat *s)
{
    struct open_file *d = descriptor(f, fd);
    if (!d)
        return -LUME_EBADF;
    if (d->external)
    {
        if (d->external->stat)
            return d->external->stat(d->context, s);
        *s = (struct lume_stat){UINT64_C(0x200000) + (unsigned)f->fd[fd].object, 0,
                                d->external->kind, 0};
        return 0;
    }
    if (d->pipe >= 0 && d->node < 0)
    {
        *s = (struct lume_stat){UINT64_C(0x100000) + (unsigned)d->pipe, 0, LUME_NODE_PIPE, 0};
        return 0;
    }
    struct node *n = &nodes[d->node];
    *s = (struct lume_stat){node_inode((unsigned)d->node), n->bytes, n->kind,
                            n->kind == LUME_NODE_PIPE ? n->links : 0};
    return 0;
}

enum directory_entry_format
{
    DIRECTORY_ENTRY_SHORT,
    DIRECTORY_ENTRY_LONG,
    DIRECTORY_ENTRY_POSIX
};

static void directory_posix_entry(void *out, unsigned index, uint64_t position, const char *name,
                                  size_t name_bytes)
{
    struct lume_files_dirent_posix entry = {.inode = node_inode(index),
                                            .position = position,
                                            .kind = standard_descriptor(&nodes[index]) >= 0
                                                        ? 0
                                                        : nodes[index].kind,
                                            .name_bytes = (uint32_t)name_bytes};
    copy(entry.name, name, name_bytes + 1);
    copy(out, &entry, sizeof(entry));
}

static int64_t directory_entries(struct lume_files *f, uint64_t fd, void *out, uint64_t bytes,
                                 enum directory_entry_format format)
{
    struct open_file *d = descriptor(f, fd);
    if (!d)
        return -LUME_EBADF;
    if (f->identity.jailed)
        return 0;
    if (d->pipe >= 0 || d->external)
        return -LUME_ENOTDIR;
    struct node *n = &nodes[d->node];
    if (n->kind != LUME_NODE_DIR)
        return -LUME_ENOTDIR;
    if (!n->linked)
        return 0;
    int permission = lume_sandbox_path(f, n->path);
    if (permission)
        return permission;
    size_t record_bytes = format == DIRECTORY_ENTRY_POSIX  ? sizeof(struct lume_files_dirent_posix)
                          : format == DIRECTORY_ENTRY_LONG ? sizeof(struct lume_files_dirent)
                                                          : sizeof(struct lume_dirent);
    if (bytes < record_bytes)
        return -LUME_EINVAL;
    size_t prefix = length(n->path);
    uint64_t used = 0;
    if (format == DIRECTORY_ENTRY_POSIX)
    {
        if (!d->offset)
        {
            d->offset = LUME_FILES_DIR_AFTER_DOT;
            directory_posix_entry(out, (unsigned)d->node, d->offset, ".", 1);
            used += record_bytes;
        }
        if (d->offset == LUME_FILES_DIR_AFTER_DOT && bytes - used >= record_bytes)
        {
            unsigned parent = (unsigned)d->node;
            if (n->parent && n->parent <= count && nodes[n->parent - 1].linked &&
                !lume_sandbox_path(f, nodes[n->parent - 1].path))
                parent = n->parent - 1;
            d->offset = LUME_FILES_DIR_AFTER_DOTDOT;
            directory_posix_entry((unsigned char *)out + used, parent, d->offset, "..", 2);
            used += record_bytes;
        }
        if (d->offset == LUME_FILES_DIR_AFTER_DOTDOT && bytes - used >= record_bytes)
            d->offset = 0;
    }
    if (n->persistent && storage && storage->next)
    {
        while (d->offset != LUME_FILES_DIR_AFTER_DOT &&
               d->offset != LUME_FILES_DIR_AFTER_DOTDOT && bytes - used >= record_bytes)
        {
            struct lume_storage_dirent found;
            uint64_t next = d->offset;
            int result = storage->next(n->path, &next, &found);
            if (result <= 0)
                return result < 0 && !used ? result : (int64_t)used;
            size_t name_bytes = 0;
            while (name_bytes < sizeof(found.name) && found.name[name_bytes])
                ++name_bytes;
            size_t name_capacity =
                format == DIRECTORY_ENTRY_SHORT ? sizeof(((struct lume_dirent *)0)->name)
                                                 : LUME_FILES_NAME_MAX + 1;
            if (!name_bytes || name_bytes == sizeof(found.name) || !found.inode ||
                next <= d->offset)
                return used ? (int64_t)used : -LUME_EIO;
            if (name_bytes >= name_capacity)
                return used ? (int64_t)used : -LUME_ENAMETOOLONG;
            if (next >= LUME_FILES_DIR_AFTER_DOT)
                return used ? (int64_t)used : -LUME_EOVERFLOW;
            d->offset = next;
            if (format == DIRECTORY_ENTRY_POSIX)
            {
                struct lume_files_dirent_posix entry = {.inode = storage_inode_number(found.inode),
                                                        .position = next,
                                                        .kind = found.kind,
                                                        .name_bytes = (uint32_t)name_bytes};
                copy(entry.name, found.name, name_bytes + 1);
                copy((unsigned char *)out + used, &entry, sizeof(entry));
            }
            else if (format == DIRECTORY_ENTRY_LONG)
            {
                struct lume_files_dirent entry = {
                    .inode = next, .kind = found.kind, .name_bytes = (uint32_t)name_bytes};
                copy(entry.name, found.name, name_bytes + 1);
                copy((unsigned char *)out + used, &entry, sizeof(entry));
            }
            else
            {
                struct lume_dirent entry = {.inode = storage_inode_number(found.inode),
                                           .kind = found.kind};
                copy(entry.name, found.name, name_bytes + 1);
                copy((unsigned char *)out + used, &entry, sizeof(entry));
            }
            used += record_bytes;
        }
        return (int64_t)used;
    }
    while (d->offset < count && bytes - used >= record_bytes)
    {
        unsigned i = (unsigned)d->offset++;
        if (!nodes[i].linked)
            continue;
        const char *p = nodes[i].path;
        size_t k = 0;
        while (k < prefix && n->path[k] == p[k])
            ++k;
        if (k != prefix || (prefix > 1 && p[k] != '/'))
            continue;
        if (prefix > 1)
            ++k;
        if (!p[k])
            continue;
        const char *name = p + k;
        while (p[k] && p[k] != '/')
            ++k;
        if (p[k])
            continue;
        size_t name_bytes = length(name);
        size_t name_capacity =
            format == DIRECTORY_ENTRY_SHORT ? sizeof(((struct lume_dirent *)0)->name)
                                             : LUME_FILES_NAME_MAX + 1;
        if (name_bytes >= name_capacity)
        {
            --d->offset;
            return used ? (int64_t)used : -LUME_ENAMETOOLONG;
        }
        if (format == DIRECTORY_ENTRY_POSIX)
            directory_posix_entry((unsigned char *)out + used, i, d->offset, name, name_bytes);
        else if (format == DIRECTORY_ENTRY_LONG)
        {
            /* FILES_GETDENTS consumers use this field as the next seek position. */
            struct lume_files_dirent entry = {
                .inode = d->offset, .kind = nodes[i].kind, .name_bytes = (uint32_t)name_bytes};
            copy(entry.name, name, name_bytes + 1);
            copy((unsigned char *)out + used, &entry, sizeof(entry));
        }
        else
        {
            struct lume_dirent entry = {.inode = node_inode(i), .kind = nodes[i].kind};
            copy(entry.name, name, name_bytes + 1);
            copy((unsigned char *)out + used, &entry, sizeof(entry));
        }
        used += record_bytes;
    }
    return (int64_t)used;
}

int64_t lume_vfs_getdents(struct lume_files *files, uint64_t fd, void *out, uint64_t bytes)
{
    return directory_entries(files, fd, out, bytes, DIRECTORY_ENTRY_SHORT);
}

int64_t lume_vfs_getdents_long(struct lume_files *files, uint64_t fd, void *out, uint64_t bytes)
{
    return directory_entries(files, fd, out, bytes, DIRECTORY_ENTRY_LONG);
}

int64_t lume_vfs_getdents_posix(struct lume_files *files, uint64_t fd, void *out, uint64_t bytes)
{
    return directory_entries(files, fd, out, bytes, DIRECTORY_ENTRY_POSIX);
}

static unsigned default_mode_at(const struct node *n, const char *path)
{
    if (n->attribute)
        return 0020444 | (n->attribute->write ? 0200 : 0);
    unsigned mode = (n->kind == LUME_NODE_DIR     ? 0040555
                     : n->kind == LUME_NODE_CHAR  ? 0020444
                     : n->kind == LUME_NODE_BLOCK ? 0060444
                     : n->kind == LUME_NODE_LINK  ? 0120777
                     : n->kind == LUME_NODE_PIPE  ? 0010444
                                                  : 0100555) |
                    (n->writable ? 0222 : 0);
    if (n->writable && (n->kind == LUME_NODE_FILE || n->kind == LUME_NODE_DIR))
    {
        unsigned permission = n->kind == LUME_NODE_DIR ? 0755 : 0644;
        if (n->kind == LUME_NODE_FILE &&
            (!strncmp(path, "/data/bin/", 10) || !strncmp(path, "/data/libexec/", 14)))
            permission = 0755;
        mode = (mode & ~0777u) | permission;
    }
    return mode;
}

static unsigned default_mode(const struct node *n)
{
    return default_mode_at(n, n->path);
}

static int64_t node_mode(const struct node *node)
{
    unsigned mode = default_mode(node);
    if (node->mode_set)
        mode = (mode & ~07777u) | node->mode;
    return node->persistent && node->linked && !storage->read_only && !storage->metadata
               ? lume_file_mode_get(node->path, mode, true)
               : mode;
}

static int node_access_with(const struct lume_files *files, const struct node *node,
                            unsigned requested, unsigned flags)
{
    if (!requested || (!flags && !files->identity.euid))
        return 0;
    int64_t mode = node_mode(node);
    if (mode < 0)
        return (int)mode;
    const struct lume_inode_access access = {node->uid, node->gid, (uint32_t)mode};
    return lume_file_access(&files->identity, &access, requested, flags);
}

static int node_access(const struct lume_files *files, const struct node *node, unsigned requested)
{
    return node_access_with(files, node, requested, 0);
}

static int remove_access(const struct lume_files *files, const struct node *node)
{
    if (!files->identity.euid)
        return 0;
    if (!node->parent)
        return -LUME_EACCES;
    const struct node *parent = &nodes[node->parent - 1];
    int64_t mode = node_mode(parent);
    if (mode < 0)
        return (int)mode;
    const struct lume_inode_access access = {parent->uid, parent->gid, (uint32_t)mode};
    return lume_file_remove_access(&files->identity, &access, node->uid, 0);
}

static struct node *linked_backing(struct node *node)
{
    if (!node->linked && native_storage(node))
        for (unsigned i = 0; i < count; ++i)
            if (nodes[i].linked && same_backing(node, &nodes[i]))
                return &nodes[i];
    return node;
}

static int64_t stat_node_times(unsigned index, struct lume_stat *out, struct lume_file_times *times,
                                struct lume_file_allocation *allocation)
{
    struct node *original = &nodes[index], *n = linked_backing(original);
    bool native = native_storage(n);
    if (allocation && native && storage->allocation)
    {
        int error = storage->allocation(n->storage_inode, allocation);
        if (error)
            return error;
    }
    if (native && n->linked && (!storage->read_only || times))
    {
        struct lume_inode_access access;
        struct lume_stat metadata = {0};
        int error = storage->metadata(n->path, &metadata, &access, times);
        if (error)
            return error;
        if (metadata.inode != n->storage_inode || metadata.kind != n->kind)
            return -LUME_EIO;
        bool changed = n->bytes != metadata.size;
        n->bytes = metadata.size;
        n->uid = access.uid;
        n->gid = access.gid;
        n->links = metadata.links;
        n->mode = access.mode & 07777u;
        n->mode_set = true;
        native_aliases(n, changed, true);
        original->uid = n->uid;
        original->gid = n->gid;
        *out = metadata;
        out->inode = node_inode(index);
        return access.mode;
    }
    *out = (struct lume_stat){node_inode(index), n->bytes, n->kind, n->links};
    return node_mode(n);
}

static int64_t stat_node(unsigned index, struct lume_stat *out)
{
    return stat_node_times(index, out, NULL, NULL);
}

struct stat_details
{
    struct lume_files_stat_times metadata;
    struct lume_file_allocation *allocation;
};

static int64_t stat_owned_node(unsigned index, struct lume_stat *out,
                               struct stat_details *details)
{
    int64_t result = stat_node_times(index, out, details ? &details->metadata.times : NULL,
                                     details ? details->allocation : NULL);
    if (result >= 0 && details)
    {
        details->metadata.stat.uid = nodes[index].uid;
        details->metadata.stat.gid = nodes[index].gid;
    }
    return result;
}

static int64_t stat_fd(struct lume_files *f, uint64_t fd, struct lume_stat *out,
                       struct stat_details *details)
{
    struct open_file *d = descriptor(f, fd);
    if (d && d->external && d->external->stat_access)
    {
        struct lume_inode_access access;
        int64_t result = d->external->stat_access(d->context, out, &access);
        if (details && result >= 0)
        {
            details->metadata.stat.uid = access.uid;
            details->metadata.stat.gid = access.gid;
        }
        return result;
    }
    if (d && (d->node < 0 || d->external))
    {
        (void)lume_vfs_stat(f, fd, out);
        return d->external ? (d->external->kind == LUME_NODE_FILE     ? 0100600
                              : d->external->kind == LUME_NODE_SOCKET ? 0140600
                                                                      : 0020600)
                           : 0010600;
    }
    return d ? stat_owned_node((unsigned)d->node, out, details) : -LUME_EBADF;
}

int64_t lume_vfs_stat_mode(struct lume_files *f, uint64_t fd, struct lume_stat *out)
{
    return stat_fd(f, fd, out, NULL);
}

int64_t lume_vfs_stat_inode(uint64_t inode, struct lume_stat *out)
{
    if (node_capacity < SHORT_STORAGE_INODE_BASE && inode >= SHORT_STORAGE_INODE_BASE &&
        inode <= UINT16_MAX)
        inode = LUME_STORAGE_INODE_BASE + (inode - SHORT_STORAGE_INODE_BASE);
    if (inode > UINT32_MAX)
    {
        for (unsigned i = 0; i < count; ++i)
            if (nodes[i].linked && nodes[i].storage_inode == inode)
                return stat_node(i, out);
        return -LUME_ENOENT;
    }
    if (!inode || inode > count || !nodes[inode - 1].linked)
        return -LUME_ENOENT;
    return stat_node((unsigned)inode - 1, out);
}

static int sandbox_stat(struct lume_files *files, const char *canonical, unsigned index)
{
    int permission = lume_sandbox_path(files, canonical);
    if (!permission || files->identity.sandbox_profile != LUME_SANDBOX_NETWORK ||
        nodes[index].kind != LUME_NODE_DIR)
        return permission;
    struct lume_directory_authorities *authorities = files->directory_authorities;
    if (!authorities || authorities->generation != namespace_generation || !files_ready(files))
        return permission;
    /* Ancestor metadata does not grant access to directory contents. */
    for (unsigned i = 0; i < LUME_SANDBOX_DIRECTORY_LIMIT; ++i)
    {
        struct directory_authority *entry = &authorities->entry[i];
        if (!entry->active)
            continue;
        int anchor = open_file_at(entry->object)->node;
        if (anchor >= 0 && (unsigned)anchor < count && directory_contains(index, (unsigned)anchor))
            return 0;
    }
    return permission;
}

static int64_t stat_terminal(struct lume_files *files, const char *path, struct lume_stat *out,
                             struct stat_details *details)
{
    int permission = lume_sandbox_path(files, path);
    if (permission)
        return permission;
    struct lume_inode_access access;
    int64_t result = terminal_stat(files, path, files->identity.euid, out, &access);
    if (details && result >= 0)
    {
        details->metadata.stat.uid = access.uid;
        details->metadata.stat.gid = access.gid;
    }
    return result;
}

static int64_t stat_resolved(struct lume_files *files, const char *path, int index,
                             struct lume_stat *out, struct stat_details *details)
{
    if (index < 0)
        return index;
    int permission = sandbox_stat(files, path, (unsigned)index);
    if (permission)
        return permission;
    int standard = standard_descriptor(&nodes[index]);
    if (standard < 0)
        return stat_owned_node((unsigned)index, out, details);
    return descriptor(files, (unsigned)standard) ? stat_fd(files, (unsigned)standard, out, details)
                                                : -LUME_ENOENT;
}

static int64_t stat_path(struct lume_files *f, const char *input, struct lume_stat *out,
                         struct stat_details *details)
{
    char p[LUME_FILES_PATH_MAX];
    int i = resolve(f, input, p);
    if (i == -LUME_ENOENT && terminal_stat && !strncmp(p, "/dev/pts/", 9))
        return stat_terminal(f, p, out, details);
    return stat_resolved(f, p, i, out, details);
}

int64_t lume_vfs_stat_path(struct lume_files *f, const char *input, struct lume_stat *out)
{
    return stat_path(f, input, out, NULL);
}

static int volume_node(struct lume_files *files, const struct node *node,
                       struct lume_files_volume *out)
{
    int permission = lume_sandbox_path(files, node->path);
    if (permission)
        return permission;
    if (!node->persistent || !storage || !storage->volume_info)
        return -LUME_EOPNOTSUPP;
    int result = storage->volume_info(out);
    if (!result && storage->metadata && storage->create_owned)
        out->flags |= LUME_FILES_VOLUME_NATIVE_PERMISSIONS;
    return result;
}

int lume_vfs_volume_path(struct lume_files *files, const char *input, struct lume_files_volume *out)
{
    if (files->identity.euid)
        return -LUME_EACCES;
    char path[LUME_FILES_PATH_MAX];
    int index = resolve(files, input, path);
    return index < 0 ? index : volume_node(files, &nodes[index], out);
}

int lume_vfs_volume_fd(struct lume_files *files, uint64_t fd, struct lume_files_volume *out)
{
    struct open_file *file = descriptor(files, fd);
    if (!file)
        return -LUME_EBADF;
    if (files->identity.jailed || files->identity.euid)
        return -LUME_EACCES;
    if (file->node < 0 || file->external || file->pipe >= 0)
        return -LUME_EOPNOTSUPP;
    return volume_node(files, &nodes[file->node], out);
}

static int file_owner(struct lume_files *files, unsigned operation,
                      const struct lume_file_mode_request *request, const char *input)
{
    if (files->identity.euid || files->identity.jailed)
        return -LUME_EPERM;
    if (request->version != LUME_FILE_MODE_VERSION || request->bytes != sizeof(*request) ||
        request->mode != UINT32_MAX || request->flags || request->descriptor_flags ||
        (request->uid != UINT32_MAX && request->uid > LUME_ID_MAX) ||
        (request->gid != UINT32_MAX && request->gid > LUME_ID_MAX))
        return -LUME_EINVAL;
    const struct lume_inode_access change = {request->uid, request->gid, UINT32_MAX};
    int index;
    if (operation == LUME_FILE_MODE_OWNER_FD)
    {
        const struct open_file *description = descriptor(files, (unsigned)request->fd);
        if (!description || (description->pipe >= 0 && description->node < 0))
            return -LUME_EBADF;
        if (description->external)
            return description->external->set_access
                       ? description->external->set_access(description->context, &files->identity,
                                                           &change)
                       : -LUME_EBADF;
        index = description->node;
    }
    else
    {
        char path[LUME_FILES_PATH_MAX];
        index = resolve_path(files, input, path, operation == LUME_FILE_MODE_OWNER, true);
        if (index == -LUME_ENOENT && terminal_set_access && !strncmp(path, "/dev/pts/", 9))
        {
            int permission = sandbox_mutation(files, path, false);
            return permission ? permission : terminal_set_access(files, path, &change);
        }
    }
    if (index < 0)
        return index;
    struct node *node = &nodes[index];
    int result = sandbox_mutation(files, node->path, false);
    if (result)
        return result;
    if (!node->writable)
        return -LUME_EROFS;
    if (node->kind != LUME_NODE_FILE && node->kind != LUME_NODE_DIR &&
        node->kind != LUME_NODE_LINK && node->kind != LUME_NODE_PIPE)
        return -LUME_EOPNOTSUPP;
    if (request->uid == UINT32_MAX && request->gid == UINT32_MAX)
        return 0;
    if (node->persistent)
    {
        if (!node->linked || !storage->set_owner)
            return -LUME_EOPNOTSUPP;
        result = storage->set_owner(node->path, request->uid, request->gid);
        if (result)
            return result;
    }
    if (request->uid != UINT32_MAX)
        node->uid = request->uid;
    if (request->gid != UINT32_MAX)
        node->gid = request->gid;
    if (node->kind == LUME_NODE_FILE)
        node->mode &= ~06000u;
    native_aliases(node, false, true);
    return 0;
}

int64_t lume_vfs_file_mode_wait(struct lume_files *files, unsigned operation,
                                struct lume_file_mode_request *request, const char *input,
                                bool compat, struct lume_fifo_open *pending)
{
    if (operation == LUME_FILE_MODE_OWNER || operation == LUME_FILE_MODE_OWNER_FD ||
        operation == LUME_FILE_MODE_OWNER_LINK)
        return file_owner(files, operation, request, input);
    if (files->identity.jailed &&
        (operation != LUME_FILE_MODE_OPEN || request->flags != LUME_O_RDONLY))
        return -LUME_EPERM;
    if (request->version != LUME_FILE_MODE_VERSION || request->bytes != sizeof(*request) ||
        operation > LUME_FILE_MODE_MKDIR ||
        (request->mode != UINT32_MAX && request->mode & ~07777u) ||
        (request->uid != UINT32_MAX && request->uid) ||
        (request->gid != UINT32_MAX && request->gid))
        return -LUME_EINVAL;
    if (operation == LUME_FILE_MODE_OPEN)
    {
        if ((request->mode & ~0777u) ||
            (request->descriptor_flags & ~(LUME_FD_CLOEXEC | LUME_FD_NONBLOCK)))
            return -LUME_EINVAL;
        bool shared = false;
        int64_t fd = open_mode(files, input, request->flags, request->mode, compat,
                               request->descriptor_flags, pending, &shared);
        if (fd >= 0)
        {
            files->fd[fd].flags = request->descriptor_flags & LUME_FD_CLOEXEC;
            if (!shared)
                descriptor(files, (unsigned)fd)->status = request->descriptor_flags & LUME_FD_NONBLOCK;
        }
        return fd;
    }
    if (operation == LUME_FILE_MODE_MKDIR)
        return request->mode & ~01777u ? -LUME_EINVAL : mkdir_mode(files, input, request->mode);
    bool nofollow =
        !compat && operation == LUME_FILE_MODE_PATH && (request->flags & LUME_FILES_MODE_NOFOLLOW);
    char canonical[LUME_FILES_PATH_MAX];
    const struct lume_inode_access change = {UINT32_MAX, UINT32_MAX, request->mode};
    int index;
    if (operation == LUME_FILE_MODE_FD)
    {
        struct open_file *description = descriptor(files, (unsigned)request->fd);
        if (!description || (description->pipe >= 0 && description->node < 0))
            return -LUME_EBADF;
        if (description->external)
            return description->external->set_access
                       ? description->external->set_access(description->context, &files->identity,
                                                           &change)
                       : -LUME_EBADF;
        index = description->node;
    }
    else
    {
        index = resolve_path(files, input, canonical, !nofollow, true);
        if (index == -LUME_ENOENT && terminal_set_access && !strncmp(canonical, "/dev/pts/", 9))
        {
            int permission = sandbox_mutation(files, canonical, false);
            return permission ? permission : terminal_set_access(files, canonical, &change);
        }
    }
    if (index < 0)
        return index;
    struct node *node = &nodes[index];
    if (request->mode == UINT32_MAX)
        return 0;
    int permission = sandbox_mutation(files, node->path, false);
    if (permission)
        return permission;
    if (nofollow && node->kind == LUME_NODE_LINK)
        return -LUME_EOPNOTSUPP;
    if (!node->writable)
        return -LUME_EROFS;
    if (files->identity.euid && files->identity.euid != node->uid)
        return -LUME_EPERM;
    unsigned mode = request->mode;
    if ((mode & 01000u) && node->kind != LUME_NODE_DIR)
        return -LUME_EINVAL;
    if ((mode & 04000u) && node->kind != LUME_NODE_FILE)
        return -LUME_EINVAL;
    if (node->kind != LUME_NODE_DIR && (mode & 06000u) &&
        (node->kind != LUME_NODE_FILE || !setid_backing(node)))
        return -LUME_EOPNOTSUPP;
    if (files->identity.euid && (mode & 02000u) &&
        !lume_file_group_member(&files->identity, node->gid, false))
        mode &= ~02000u;
    return set_node_mode(node, mode);
}

static int set_node_mode(struct node *node, unsigned mode)
{
    int result = 0;
    if (node->persistent && node->linked)
    {
        if (!storage->metadata && mode & ~0777u)
            return -LUME_EOPNOTSUPP;
        result = storage->metadata
                     ? (storage->set_mode ? storage->set_mode(node->path, mode) : -LUME_EOPNOTSUPP)
                 : mode == (default_mode(node) & 0777u)
                     ? lume_file_mode_remove(node->path, true)
                     : lume_file_mode_set(node->path, mode, true);
    }
    if (result)
        return result;
    node->mode = mode;
    node->mode_set = true;
    native_aliases(node, false, true);
    return 0;
}

int64_t lume_vfs_file_mode(struct lume_files *files, unsigned operation,
                           struct lume_file_mode_request *request, const char *input)
{
    return lume_vfs_file_mode_wait(files, operation, request, input, true, NULL);
}

int64_t lume_vfs_file_mode_long(struct lume_files *files, unsigned operation,
                                struct lume_file_mode_request *request, const char *input)
{
    return lume_vfs_file_mode_wait(files, operation, request, input, false, NULL);
}

int64_t lume_vfs_resize(struct lume_files *f, uint64_t fd, uint64_t length)
{
    int permission = lume_vfs_sandbox_access(f, fd, true);
    if (permission)
        return permission;
    struct open_file *d = descriptor(f, fd);
    if (!d || !(d->flags & 3))
        return -LUME_EBADF;
    if (d->external)
        return d->external->resize ? d->external->resize(d->context, length) : -LUME_EINVAL;
    if (d->pipe >= 0)
        return -LUME_EINVAL;
    struct node *n = &nodes[d->node];
    if (n->kind != LUME_NODE_FILE)
        return -LUME_EINVAL;
    if (!n->writable)
        return -LUME_EROFS;
    if (executable_backing(n))
        return -LUME_ETXTBSY;
    if (f->identity.jailed)
        return -LUME_EACCES;
    if (length > INT64_MAX)
        return -LUME_EINVAL;
    if (length > n->bytes && length > f->identity.limit[LUME_LIMIT_FILE_SIZE].soft)
        return -27;
    int result = length != n->bytes ? clear_file_setid(n) : 0;
    if (result)
        return result;
    if (n->persistent)
    {
        if (!d->storage_handle && !storage->resize)
            return -LUME_ENOSYS;
        int error = d->storage_handle ? storage->resize_handle(d->storage_handle, length)
                                      : storage->resize(n->path, length);
        if (error)
            return error;
    }
    else
    {
        if (length > CAPACITY)
            return -LUME_ENOSPC;
        if (length && !n->data)
        {
            for (unsigned i = 0; i < RAM_FILES; ++i)
                if (!ram_used[i])
                {
                    ram_used[i] = true;
                    n->data = ram_data[i];
                    break;
                }
            if (!n->data)
                return -LUME_ENOSPC;
        }
        if (length > n->bytes)
            memset(n->data + n->bytes, 0, (size_t)(length - n->bytes));
    }
    n->bytes = length;
    node_changed(n);
    native_aliases(n, true, false);
    return 0;
}

int64_t lume_vfs_truncate(struct lume_files *f, uint64_t fd)
{
    struct open_file *description = descriptor(f, fd);
    return description ? lume_vfs_resize(f, fd, description->offset) : -LUME_EBADF;
}

static bool subtree(const char *path, const char *root)
{
    size_t bytes = length(root);
    return !strncmp(path, root, bytes) && (!path[bytes] || path[bytes] == '/');
}

struct rename_path
{
    unsigned node, permissions, previous_mode;
    uint32_t hash;
    char *old_path, *new_path;
};

static int64_t rename_namespace(struct lume_files *files, const char *from, const char *to,
                                bool replace)
{
    char a[LUME_FILES_PATH_MAX], b[LUME_FILES_PATH_MAX], parent[LUME_FILES_PATH_MAX];
    int source = resolve_path(files, from, a, false, false);
    int target = resolve_path(files, to, b, false, false);
    if (source < 0)
        return source;
    if (target < 0 && (target != -LUME_ENOENT || !b[0]))
        return target;
    int permission = sandbox_mutation(files, a, true);
    if (!permission)
        permission = sandbox_mutation(files, b, true);
    if (permission)
        return permission;
    int spelling = lume_path_final_spelling(to, b);
    if (spelling)
        return spelling;
    bool same = target == source;
    struct node *incoming = &nodes[source];
    permission = remove_access(files, incoming);
    if (permission)
        return permission;
    if (same && equal(a, b))
        return 0;
    bool directory = incoming->kind == LUME_NODE_DIR;
    if (incoming->kind != LUME_NODE_FILE && incoming->kind != LUME_NODE_LINK &&
        incoming->kind != LUME_NODE_PIPE && !directory)
        return -LUME_EINVAL;
    if (!incoming->writable || incoming->block || equal(a, "/tmp") || equal(a, "/data"))
        return -LUME_EROFS;
    if (lume_file_mode_namespace_busy(a) || lume_file_mode_namespace_busy(b))
        return -LUME_EBUSY;
    size_t to_bytes = length(to);
    if (!directory && to_bytes && to[to_bytes - 1] == '/')
        return -LUME_ENOTDIR;
    unsigned destination = parent_node(b);
    if (destination == UINT32_MAX)
        return -LUME_ENOENT;
    if (!destination || !nodes[destination - 1].writable)
        return -LUME_EROFS;
    permission = target >= 0 ? remove_access(files, &nodes[target])
                             : node_access(files, &nodes[destination - 1],
                                           LUME_ACCESS_WRITE | LUME_ACCESS_EXECUTE);
    if (permission)
        return permission;
    if (!same && target >= 0 && !replace)
        return -LUME_EEXIST;
    if (replace && target >= 0 && native_storage(incoming) &&
        same_backing(incoming, &nodes[target]))
        return 0;
    if (incoming->persistent != nodes[destination - 1].persistent)
        return -18;
    if (directory && incoming->parent != destination)
    {
        permission = node_access(files, incoming, LUME_ACCESS_WRITE);
        if (permission)
            return permission;
    }
    if (directory && !same && subtree(b, a))
        return -LUME_EINVAL;
    unsigned previous_permissions = 0;
    struct node *previous = target >= 0 && !same ? &nodes[target] : NULL;
    struct lume_stat previous_metadata;
    if (previous)
    {
        if ((previous->kind == LUME_NODE_DIR) != directory)
            return directory ? -LUME_ENOTDIR : -LUME_EISDIR;
        if (!previous->writable || previous->block || equal(b, "/tmp") || equal(b, "/data"))
            return -LUME_EROFS;
        if (previous->persistent != incoming->persistent)
            return -18;
        if (directory)
            for (unsigned i = 0; i < count; ++i)
                if ((int)i != target && nodes[i].linked && subtree(nodes[i].path, previous->path))
                    return -LUME_ENOTEMPTY;
        int64_t mode = stat_node((unsigned)target, &previous_metadata);
        if (mode < 0)
            return mode;
        previous_permissions = (unsigned)mode & 07777u;
    }
    bool persistent = incoming->persistent;
    bool legacy_modes = persistent && !storage->metadata;
    if (persistent && incoming->kind == LUME_NODE_FILE && incoming->references &&
        !storage->rename_state)
        return -LUME_EBUSY;
    if (persistent && previous && previous->kind == LUME_NODE_FILE && previous->references &&
        !storage->remove_state && previous_metadata.links <= 1)
        return -LUME_EBUSY;
    unsigned affected = 0;
    for (unsigned i = 0; i < count; ++i)
        if (nodes[i].linked && subtree(nodes[i].path, a))
            ++affected;
    struct rename_path *paths = lume_kernel_allocate((size_t)affected * sizeof(*paths));
    struct lume_file_mode_change *changes =
        lume_kernel_allocate((size_t)(affected + 1) * sizeof(*changes));
    if (!paths || !changes)
    {
        lume_kernel_free(changes);
        lume_kernel_free(paths);
        return -LUME_ENOMEM;
    }
    unsigned prepared = 0;
    int result = 0;
    size_t old_bytes = length(a), new_bytes = length(b);
    for (unsigned i = 0; !result && i < count; ++i)
    {
        if (!nodes[i].linked || !subtree(nodes[i].path, a))
            continue;
        size_t tail = length(nodes[i].path + old_bytes);
        if (new_bytes + tail >= sizeof(parent))
        {
            result = -LUME_ENAMETOOLONG;
            break;
        }
        copy(parent, b, new_bytes);
        copy(parent + new_bytes, nodes[i].path + old_bytes, tail + 1);
        char *owned = lume_path_duplicate(parent);
        if (!owned)
        {
            result = -LUME_ENOMEM;
            break;
        }
        struct lume_stat metadata;
        int64_t permissions = stat_node(i, &metadata);
        if (permissions < 0)
        {
            lume_path_release(owned);
            result = (int)permissions;
            break;
        }
        struct rename_path *entry = &paths[prepared];
        *entry = (struct rename_path){.node = i,
                                      .permissions = (unsigned)permissions & 07777u,
                                      .hash = namespace_hash(owned),
                                      .old_path = nodes[i].path,
                                      .new_path = owned};
        if (legacy_modes)
        {
            result = lume_file_mode_override(owned, &entry->previous_mode, true);
            unsigned mode = entry->permissions;
            if (previous && (int)i == source)
                mode &= previous_permissions;
            changes[prepared] = (struct lume_file_mode_change){
                owned, mode == (default_mode_at(&nodes[i], owned) & 0777u) ? UINT32_MAX : mode};
        }
        ++prepared;
        lume_worker_checkpoint(0);
    }
    if (!result && legacy_modes)
        result = lume_file_mode_batch(changes, affected, true);
    if (result)
        goto discard;
    unsigned state = LUME_STORAGE_NAMESPACE_COMMITTED;
    if (persistent)
    {
        state = LUME_STORAGE_NAMESPACE_UNCHANGED;
        if (previous)
            result = storage->replace ? storage->replace(a, b, &state) : -LUME_ENOSYS;
        else if (storage->rename_state)
            result = storage->rename_state(a, b, &state);
        else
        {
            result = storage->rename ? storage->rename(a, b) : -LUME_ENOSYS;
            if (!result)
                state = LUME_STORAGE_NAMESPACE_COMMITTED;
        }
    }
    if (state != LUME_STORAGE_NAMESPACE_COMMITTED)
    {
        if (legacy_modes && state == LUME_STORAGE_NAMESPACE_UNCHANGED)
        {
            for (unsigned i = 0; i < prepared; ++i)
                changes[i] =
                    (struct lume_file_mode_change){paths[i].new_path, paths[i].previous_mode};
            int restored = lume_file_mode_batch(changes, prepared, true);
            if (!result)
                result = restored;
        }
        if (!result)
            result = -LUME_EIO;
        goto discard;
    }
    /* Namespace publication contains no allocations, callbacks or worker checkpoints. */
    incoming->parent = destination;
    if (previous)
    {
        previous->mode = previous_permissions;
        previous->mode_set = true;
        index_remove((unsigned)target);
        previous->linked = false;
        if (previous->kind == LUME_NODE_PIPE ||
            (previous->kind == LUME_NODE_FILE && native_storage(previous)))
        {
            previous->links = previous_metadata.links ? previous_metadata.links - 1u : 0;
            native_aliases(previous, false, true);
        }
    }
    for (unsigned i = 0; i < prepared; ++i)
        index_remove(paths[i].node);
    for (unsigned i = 0; i < prepared; ++i)
    {
        struct node *node = &nodes[paths[i].node];
        node->path = paths[i].new_path;
        paths[i].new_path = NULL;
        node->mode = paths[i].permissions;
        node->mode_set = true;
        index_insert_hashed(paths[i].node, paths[i].hash);
    }
    if (previous)
        release_ram(previous);
    unsigned cleanup = 0;
    if (!result && legacy_modes)
    {
        for (unsigned i = 0; i < prepared; ++i)
            if (!lume_path_equal_folded(paths[i].old_path, nodes[paths[i].node].path))
                changes[cleanup++] = (struct lume_file_mode_change){paths[i].old_path, UINT32_MAX};
        if (previous)
            changes[cleanup++] = (struct lume_file_mode_change){
                incoming->path,
                incoming->mode == (default_mode(incoming) & 0777u) ? UINT32_MAX : incoming->mode};
        result = lume_file_mode_batch(changes, cleanup, true);
    }
    for (unsigned i = 0; i < prepared; ++i)
        lume_path_release(paths[i].old_path);
    lume_kernel_free(changes);
    lume_kernel_free(paths);
    return result;
discard:
    for (unsigned i = 0; i < prepared; ++i)
        lume_path_release(paths[i].new_path);
    lume_kernel_free(changes);
    lume_kernel_free(paths);
    return result;
}

int64_t lume_vfs_rename(struct lume_files *files, const char *from, const char *to)
{
    return rename_namespace(files, from, to, false);
}

int64_t lume_vfs_replace(struct lume_files *files, const char *from, const char *to)
{
    return rename_namespace(files, from, to, true);
}

static int64_t enter_directory(struct lume_files *f, int index, const char *canonical)
{
    if (index < 0)
        return index;
    int permission = lume_sandbox_path(f, canonical);
    if (permission)
        return permission;
    if (nodes[index].kind != LUME_NODE_DIR)
        return -LUME_ENOTDIR;
    permission = node_access(f, &nodes[index], LUME_ACCESS_EXECUTE);
    if (permission)
        return permission;
    hold_directory(f, (unsigned)index);
    return 0;
}

int64_t lume_vfs_chdir(struct lume_files *f, const char *input)
{
    char path[LUME_FILES_PATH_MAX];
    int index = resolve(f, input, path);
    return enter_directory(f, index, path);
}

int64_t lume_vfs_fchdir(struct lume_files *f, uint64_t fd)
{
    const struct open_file *file = descriptor(f, fd);
    if (!file)
        return -LUME_EBADF;
    if (file->node < 0 || file->external || file->pipe >= 0)
        return -LUME_ENOTDIR;
    const char *path = nodes[file->node].path;
    if (f->identity.jailed && !equal(path, "/"))
        return -LUME_EACCES;
    return enter_directory(f, file->node, path);
}

int64_t lume_vfs_chroot(struct lume_files *files, const char *input)
{
    if (files->identity.euid || files->identity.jailed)
        return -LUME_EPERM;
    int64_t fd = lume_vfs_open(files, input, LUME_O_RDONLY);
    if (fd < 0)
        return fd;
    struct lume_stat metadata;
    int64_t result = lume_vfs_stat(files, (unsigned)fd, &metadata);
    if (!result && metadata.kind != LUME_NODE_DIR)
        result = -LUME_ENOTDIR;
    if (!result && metadata.inode == node_inode(0))
    {
        (void)lume_vfs_close(files, (unsigned)fd);
        return 0;
    }
    struct lume_dirent entries[8];
    while (!result)
    {
        int64_t bytes = lume_vfs_getdents(files, (unsigned)fd, entries, sizeof(entries));
        if (bytes < 0)
        {
            result = bytes;
            break;
        }
        if (!bytes)
            break;
        for (unsigned i = 0; i < (unsigned)bytes / sizeof(entries[0]); ++i)
            if (!equal(entries[i].name, ".") && !equal(entries[i].name, ".."))
                result = -LUME_ENOTEMPTY;
    }
    (void)lume_vfs_close(files, (unsigned)fd);
    if (!result)
    {
        files->identity.jailed = true;
        hold_directory(files, 0);
    }
    return result;
}

bool lume_vfs_image(const char *p, const void **data, size_t *bytes)
{
    int i = find(p);
    if (i < 0 || !nodes[i].image || nodes[i].kind != LUME_NODE_FILE)
        return false;
    *data = nodes[i].image;
    *bytes = (size_t)nodes[i].bytes;
    return true;
}

int64_t lume_vfs_dup2(struct lume_files *f, uint64_t from, uint64_t to)
{
    struct open_file *d = descriptor(f, from);
    if (!d || to >= LUME_FD_MAX)
        return -LUME_EBADF;
    if (from == to)
        return (int64_t)to;
    if (to >= f->identity.limit[LUME_LIMIT_FILES].soft)
        return -LUME_EBADF;
    if (f->fd[to].object == FD_FIFO_PENDING)
        return -LUME_EBUSY;
    /* Replacement ignores close errors and retires persistent handles to storage. */
    (void)close_deferred(f, to);
    bind_descriptor(f, (unsigned)to, f->fd[from].object, 0);
    open_file_retain(d);
    return (int64_t)to;
}

bool lume_vfs_pipe_wait(struct lume_files *f, uint64_t fd)
{
    struct open_file *d = descriptor(f, fd);
    return d && d->pipe >= 0 && !(d->status & LUME_FD_NONBLOCK);
}

struct lume_wait_queue *lume_vfs_pipe_queue(struct lume_files *f, uint64_t fd, bool writing)
{
    struct open_file *d = descriptor(f, fd);
    if (!d || d->pipe < 0)
        return 0;
    return writing ? &channels[d->pipe].writable : &channels[d->pipe].readable;
}

static unsigned events(struct lume_files *f, int fd, bool sample_console)
{
    if (fd < 0)
        return 0;
    struct open_file *d = descriptor(f, (unsigned)fd);
    if (!d)
        return LUME_POLLNVAL;
    if (d->external)
        return d->external->poll(d->context);
    if (d->pipe < 0)
    {
        if (nodes[d->node].block && nodes[d->node].block->device->state != LUME_DEVICE_READY)
            return LUME_POLLERR;
        return ((d->flags & 3) != LUME_O_WRONLY &&
                        (nodes[d->node].kind != LUME_NODE_CHAR || nodes[d->node].device ||
                         nodes[d->node].attribute ||
                         (sample_console ? lume_console_ready() : poll_input_ready))
                    ? LUME_POLLIN
                    : 0) |
               ((d->flags & 3) ? LUME_POLLOUT : 0);
    }
    struct channel *c = &channels[d->pipe];
    unsigned result = 0, mode = d->flags & 3;
    bool idle = !c->growing && !c->copying;
    if (mode != LUME_O_WRONLY)
    {
        if (!c->writers && (d->node < 0 || c->writer_epoch != d->fifo_writer_epoch))
            result |= LUME_POLLHUP;
        if (idle && (c->count || (d->node < 0 && !c->writers)))
            result |= LUME_POLLIN;
    }
    if (mode != LUME_O_RDONLY)
    {
        if (!c->readers)
            result |= LUME_POLLERR;
        else if (idle && c->capacity - c->count >= LUME_PIPE_ATOMIC)
            result |= LUME_POLLOUT;
    }
    return result;
}

void lume_vfs_poll_prepare(struct lume_files *files, struct lume_poll_watch *watch)
{
    if (watch->count > LUME_FD_LIMIT)
        watch->count = LUME_FD_LIMIT;
    for (unsigned i = 0; i < watch->count; ++i)
    {
        int fd = watch->records[i].fd;
        watch->records[i].revents = 0;
        watch->generations[i] =
            files_ready(files) && fd >= 0 && fd < (int)LUME_FD_MAX && files->fd[fd].object >= 0
                ? files->fd[fd].generation
                : 0;
    }
}

bool lume_vfs_poll_subscribe(struct lume_files *files, const struct lume_poll_watch *watch,
                             unsigned task)
{
    if (task >= LUME_POLL_TASK_LIMIT || watch->count > LUME_FD_LIMIT || !files_ready(files))
        return false;
    lume_vfs_poll_cancel(task);
#if LUME_FILES_DYNAMIC
    unsigned capacity = watch->count * 2u;
    if (capacity > poll_subscriptions[task].capacity)
    {
        struct poll_link *links = lume_kernel_allocate(sizeof(*links) * capacity);
        if (!links)
            return false;
        lume_kernel_free(poll_subscriptions[task].links);
        poll_subscriptions[task].links = links;
        poll_subscriptions[task].capacity = capacity;
    }
#endif
    for (unsigned i = 0; i < watch->count; ++i)
    {
        int fd = watch->records[i].fd;
        if (fd < 0 || fd >= (int)LUME_FD_MAX || files->fd[fd].object < 0 ||
            files->fd[fd].generation != watch->generations[i])
            continue;
        struct open_file *file = open_file_at(files->fd[fd].object);
        struct lume_poll_source *source = 0;
        if (file->external && file->external->source)
            source = file->external->source(file->context);
        else if (!file->external && file->pipe >= 0)
            source = &channels[file->pipe].changed;
        else if (!file->external && nodes[file->node].kind == LUME_NODE_CHAR &&
                 !nodes[file->node].device && !nodes[file->node].group &&
                 (watch->records[i].events & LUME_POLLIN))
            source = &console_source;
        if (!poll_subscribe_source(task, &files->fd[fd].changed) ||
            !poll_subscribe_source(task, source))
        {
            lume_vfs_poll_cancel(task);
            return false;
        }
    }
    return true;
}

unsigned lume_vfs_poll_check(struct lume_files *files, struct lume_poll_watch *watch)
{
    unsigned ready = 0;
    for (unsigned i = 0; i < watch->count && i < LUME_FD_LIMIT; ++i)
    {
        struct lume_pollfd *record = &watch->records[i];
        int fd = record->fd;
        unsigned result = 0;
        if (fd >= 0)
        {
            if (!files_ready(files) || fd >= (int)LUME_FD_MAX || files->fd[fd].object < 0 ||
                files->fd[fd].generation != watch->generations[i])
                result = LUME_POLLNVAL;
            else
                result = events(files, fd, false) &
                         (record->events | LUME_POLLERR | LUME_POLLHUP | LUME_POLLNVAL);
        }
        record->revents = (uint16_t)result;
        ready += result != 0;
    }
    return ready;
}

bool lume_vfs_poll_console(const struct lume_files *files, const struct lume_poll_watch *watch)
{
    if (!files_ready(files))
        return false;
    for (unsigned i = 0; i < watch->count && i < LUME_FD_LIMIT; ++i)
    {
        int fd = watch->records[i].fd;
        if (fd < 0 || fd >= (int)LUME_FD_MAX || !(watch->records[i].events & LUME_POLLIN) ||
            files->fd[fd].object < 0 || files->fd[fd].generation != watch->generations[i])
            continue;
        const struct open_file *file = open_file_at(files->fd[fd].object);
        if (!file->external && file->pipe < 0 && nodes[file->node].kind == LUME_NODE_CHAR &&
            !nodes[file->node].device && !nodes[file->node].group &&
            (file->flags & 3) != LUME_O_WRONLY)
            return true;
    }
    return false;
}

#include "eventfd.h"

int64_t lume_vfs_fd_call(struct lume_files *f, unsigned op, struct lume_fd_request *r)
{
    if (!files_ready(f))
        return -LUME_ENOMEM;
    if (r->version != LUME_FD_VERSION || r->bytes != sizeof(*r))
        return -LUME_EINVAL;
    if (op == LUME_FD_EVENTFD)
        return event_counter_create(f, r);
    struct open_file *d = descriptor(f, (unsigned)r->fd);
    if (op == LUME_FD_PATH)
    {
        if (!d)
            return -LUME_EBADF;
        if (r->other || r->flags || r->timeout_ms)
            return -LUME_EINVAL;
        if (d->node < 0 || nodes[d->node].kind != LUME_NODE_DIR || f->identity.jailed)
            return -LUME_ENOTDIR;
        size_t bytes = length(nodes[d->node].path) + 1;
        if (r->count < bytes)
            return -LUME_ENAMETOOLONG;
        copy((void *)(uintptr_t)r->buffer, nodes[d->node].path, bytes);
        return 0;
    }
    if (op == LUME_FD_GETMODE || op == LUME_FD_SETMODE)
    {
        if (!d)
            return -LUME_EBADF;
        if (r->other || r->buffer || r->count || r->timeout_ms ||
            (op == LUME_FD_GETMODE ? r->flags != 0 : (r->flags & ~LUME_O_APPEND) != 0))
            return -LUME_EINVAL;
        if (op == LUME_FD_GETMODE)
            return d->flags & (3u | LUME_O_APPEND);
        d->flags = (d->flags & ~LUME_O_APPEND) | r->flags;
        return 0;
    }
    if (op == LUME_FD_INFO)
    {
        if (r->fd || r->other || r->flags || r->count || r->timeout_ms)
            return -LUME_EINVAL;
        struct lume_fd_info info = {LUME_FD_VERSION, sizeof(info),    LUME_FD_MAX, 0, 0, 0,
                                    LUME_PIPE_BYTES, LUME_PIPE_ATOMIC};
        for (unsigned i = 0; i < LUME_FD_MAX; ++i)
            info.open += f->fd[i].object >= 0;
        info.objects = open_file_live;
        for (unsigned i = 0; i < channel_slots(); ++i)
            info.pipes += channels[i].readers || channels[i].writers;
        copy((void *)(uintptr_t)r->buffer, &info, sizeof(info));
        return 0;
    }
    if (op == LUME_FD_POLL)
    {
        if (r->count > LUME_FD_LIMIT || r->flags || r->other || r->timeout_ms)
            return -LUME_EINVAL;
        struct lume_pollfd *p = (void *)(uintptr_t)r->buffer;
        unsigned ready = 0;
        for (unsigned i = 0; i < r->count; ++i)
        {
            p[i].revents = (uint16_t)(events(f, p[i].fd, true) &
                                      (p[i].events | LUME_POLLERR | LUME_POLLHUP | LUME_POLLNVAL));
            ready += p[i].revents != 0;
        }
        return ready;
    }
    if (r->buffer || r->count || r->timeout_ms)
        return -LUME_EINVAL;
    if (op == LUME_FD_CLOSE_FROM)
    {
        if (r->fd < 0 || r->fd > (int)LUME_FD_MAX || r->other || r->flags)
            return -LUME_EINVAL;
        for (unsigned i = (unsigned)r->fd; i < LUME_FD_MAX; ++i)
            if (f->fd[i].object >= 0)
                (void)lume_vfs_close(f, i);
        return 0;
    }
    if (op == LUME_FD_SOCKETPAIR || op == LUME_FD_SPAWN)
        return -LUME_ENOSYS;
    if (op == LUME_FD_PIPE)
    {
        if (r->flags & ~(LUME_FD_CLOEXEC | LUME_FD_NONBLOCK) || r->other)
            return -LUME_EINVAL;
        unsigned first = 0, second;
        while (first < f->identity.limit[LUME_LIMIT_FILES].soft && f->fd[first].object != -1)
            ++first;
        second = first + 1;
        while (second < f->identity.limit[LUME_LIMIT_FILES].soft && f->fd[second].object != -1)
            ++second;
        if (second >= f->identity.limit[LUME_LIMIT_FILES].soft)
            return -LUME_EMFILE;
        int channel = allocate_channel();
        if (channel < 0)
            return channel;
        int a = new_object(-1, channel, LUME_O_RDONLY);
        --channels[channel].opening;
        if (a < 0)
        {
            release_channel(&channels[channel]);
            return a;
        }
        bind_descriptor(f, first, a, r->flags & LUME_FD_CLOEXEC);
        int b = new_object(-1, channel, LUME_O_WRONLY);
        if (b < 0)
        {
            (void)lume_vfs_close(f, first);
            return b;
        }
        bind_descriptor(f, second, b, r->flags & LUME_FD_CLOEXEC);
        open_file_at(a)->status = open_file_at(b)->status = r->flags & LUME_FD_NONBLOCK;
        r->fd = (int)first;
        r->other = (int)second;
        return 0;
    }
    if (!d)
        return -LUME_EBADF;
    if (op == LUME_FD_DUP || op == LUME_FD_DUP2)
    {
        if (r->other < 0 || r->other >= (int)LUME_FD_MAX || r->flags & ~LUME_FD_CLOEXEC)
            return -LUME_EINVAL;
        unsigned target = (unsigned)r->other;
        if (op == LUME_FD_DUP)
        {
            while (target < f->identity.limit[LUME_LIMIT_FILES].soft && f->fd[target].object != -1)
                ++target;
            if (target >= f->identity.limit[LUME_LIMIT_FILES].soft)
                return -LUME_EMFILE;
        }
        int result = (int)lume_vfs_dup2(f, (unsigned)r->fd, target);
        if (result >= 0)
            f->fd[target].flags = r->flags;
        return result;
    }
    if (r->other)
        return -LUME_EINVAL;
    switch (op)
    {
    case LUME_FD_AVAILABLE:
    {
        if (r->flags)
            return -LUME_EINVAL;
        if (d->pipe >= 0)
            return channels[d->pipe].count;
        if (d->node < 0 || nodes[d->node].kind != LUME_NODE_FILE || nodes[d->node].block)
            return -LUME_ENOTTY;
        uint64_t size = nodes[d->node].bytes;
        uint64_t remaining = size > d->offset ? size - d->offset : 0;
        return remaining > INT32_MAX ? INT32_MAX : (int64_t)remaining;
    }
    case LUME_FD_BLOCK_SECTOR_SIZE:
        if (r->flags)
            return -LUME_EINVAL;
        if (d->node < 0 || !nodes[d->node].block)
            return -LUME_ENOTTY;
        if (nodes[d->node].block->device->state != LUME_DEVICE_READY)
            return -LUME_EIO;
        return nodes[d->node].block->sector_bytes;
    case LUME_FD_GETFD:
        return r->flags ? -LUME_EINVAL : (int)f->fd[r->fd].flags;
    case LUME_FD_SETFD:
        if (r->flags & ~LUME_FD_CLOEXEC)
            return -LUME_EINVAL;
        f->fd[r->fd].flags = r->flags;
        return 0;
    case LUME_FD_GETFL:
        return r->flags ? -LUME_EINVAL : (int)d->status;
    case LUME_FD_SETFL:
        if (r->flags & ~LUME_FD_NONBLOCK)
            return -LUME_EINVAL;
        d->status = r->flags;
        return 0;
    default:
        return -LUME_ENOSYS;
    }
}

static int64_t mkdir_mode(struct lume_files *f, const char *input, unsigned mode)
{
    char p[LUME_FILES_PATH_MAX] = {0};
    int r = resolve_path(f, input, p, false, true);
    if (r >= 0)
        return -LUME_EEXIST;
    if (r != -LUME_ENOENT || !p[0])
        return r;
    r = create_node(f, p, LUME_NODE_DIR, mode, NULL);
    return r < 0 ? r : 0;
}

int64_t lume_vfs_mkdir(struct lume_files *files, const char *input)
{
    return mkdir_mode(files, input, UINT32_MAX);
}

int64_t lume_vfs_remove(struct lume_files *files, const char *input, bool directory)
{
    char path[LUME_FILES_PATH_MAX];
    int index = resolve_path(files, input, path, false, false);
    if (index < 0)
        return index;
    int permission = sandbox_mutation(files, path, true);
    if (permission)
        return permission;
    struct node *node = &nodes[index];
    if (directory && node->kind != LUME_NODE_DIR)
        return -LUME_ENOTDIR;
    if (!directory && node->kind == LUME_NODE_DIR)
        return -LUME_EISDIR;
    if (!node->writable || node->block || equal(path, "/tmp") || equal(path, "/data"))
        return -LUME_EROFS;
    permission = remove_access(files, node);
    if (permission)
        return permission;
    if (lume_file_mode_namespace_busy(path))
        return -LUME_EBUSY;
    if (directory)
        for (unsigned i = 0; i < count; ++i)
            if ((int)i != index && nodes[i].linked && subtree(nodes[i].path, path))
                return -LUME_ENOTEMPTY;
    struct lume_stat metadata;
    int64_t permissions = stat_node((unsigned)index, &metadata);
    if (permissions < 0)
        return permissions;
    unsigned state = LUME_STORAGE_NAMESPACE_COMMITTED;
    int result = 0;
    if (node->persistent)
    {
        state = LUME_STORAGE_NAMESPACE_UNCHANGED;
        if (storage->remove_state)
            result = storage->remove_state(path, &state);
        else
        {
            if (node->references && node->kind == LUME_NODE_FILE && metadata.links <= 1)
                return -LUME_EBUSY;
            result = storage->remove(path);
            if (!result)
                state = LUME_STORAGE_NAMESPACE_COMMITTED;
        }
    }
    if (state != LUME_STORAGE_NAMESPACE_COMMITTED)
        return result ? result : -LUME_EIO;
    node->mode = (unsigned)permissions & 07777u;
    node->mode_set = true;
    index_remove((unsigned)index);
    node->linked = false;
    if (node->kind == LUME_NODE_PIPE || (node->kind == LUME_NODE_FILE && native_storage(node)))
    {
        node->links = metadata.links ? metadata.links - 1u : 0;
        native_aliases(node, false, true);
    }
    bool persistent = node->persistent;
    release_ram(node);
    return result                            ? result
           : persistent && storage->metadata ? 0
                                             : lume_file_mode_remove(path, persistent);
}

static int find_folded(const char *path)
{
    if (!name_bucket_count)
        return -LUME_ENOENT;
    uint32_t hash = namespace_hash(path);
    for (unsigned link = name_buckets[hash & (name_bucket_count - 1)]; link;
         link = nodes[link - 1].path_next)
        if (lume_path_equal_folded(path, nodes[link - 1].path))
            return (int)link - 1;
    return -LUME_ENOENT;
}

static bool import_fat(const char *path, struct lume_fat_file file, void *unused)
{
    (void)unused;
    char p[LUME_FILES_PATH_MAX], parent[LUME_FILES_PATH_MAX];
    size_t bytes = length(path);
    if (bytes + 1 >= sizeof(p))
        return false;
    p[0] = '/';
    copy(p + 1, path, bytes + 1);
    int existing = find_folded(p);
    if (existing >= 0)
    {
        bool same = file.directory && nodes[existing].kind == LUME_NODE_DIR;
        if (same)
            nodes[existing].pending_fat = true;
        return same;
    }
    copy(parent, p, bytes + 2);
    char *leaf = strrchr(parent, '/');
    size_t leaf_bytes = length(leaf + 1);
    if (!leaf_bytes || leaf_bytes > LUME_FILES_NAME_MAX)
        return false;
    *leaf = 0;
    int directory = find_folded(parent[0] ? parent : "/");
    if (directory < 0 || nodes[directory].kind != LUME_NODE_DIR)
        return false;
    size_t prefix = length(nodes[directory].path);
    if (prefix + (prefix > 1) + leaf_bytes >= sizeof(p))
        return false;
    copy(p, nodes[directory].path, prefix);
    if (prefix > 1)
        p[prefix++] = '/';
    copy(p + prefix, leaf + 1, leaf_bytes + 1);
    if (count == node_capacity)
        return false;
    unsigned index = count;
    if (!add(p, file.directory ? LUME_NODE_DIR : LUME_NODE_FILE, 0, file.size, false))
        return false;
    nodes[index].on_fat = true;
    nodes[index].fat_file = file;
    return true;
}

static void import_finish(unsigned before, bool success)
{
    for (unsigned i = 0; i < before; ++i)
    {
        if (success && nodes[i].pending_fat)
            nodes[i].on_fat = true;
        nodes[i].pending_fat = false;
    }
    if (!success)
        while (count > before)
        {
            struct node *node = &nodes[--count];
            index_remove(count);
            release_node_memory(node);
            *node = (struct node){0};
        }
}

bool lume_vfs_mount_fat(const void *data, size_t bytes)
{
    if (root_fat.data)
        return false;
    unsigned before = count;
    struct lume_fat candidate;
    bool success =
        lume_fat_mount(&candidate, data, bytes) && lume_fat_walk(&candidate, import_fat, 0);
    import_finish(before, success);
    if (success)
        root_fat = candidate;
    return success;
}

static bool import_storage_metadata(struct node *node, bool root)
{
    if (!storage->metadata)
        return true;
    struct lume_stat metadata = {0};
    struct lume_inode_access access;
    if (storage->metadata(node->path, &metadata, &access, NULL) || metadata.inode <= UINT32_MAX)
        return false;
    bool link = !root && node->kind == LUME_NODE_FILE && metadata.kind == LUME_NODE_LINK &&
                storage->readlink;
    bool fifo = !root && node->kind == LUME_NODE_FILE && metadata.kind == LUME_NODE_PIPE &&
                !metadata.size;
    unsigned type = link                          ? 0120000u
                    : fifo                        ? 0010000u
                    : node->kind == LUME_NODE_DIR ? 0040000u
                                                  : 0100000u;
    if ((!link && !fifo && metadata.kind != node->kind) ||
        (!root && metadata.size != node->bytes) || (access.mode & 0170000u) != type)
        return false;
    node->kind = metadata.kind;
    node->storage_inode = metadata.inode;
    node->bytes = metadata.size;
    node->uid = access.uid;
    node->gid = access.gid;
    node->links = metadata.links;
    node->mode = access.mode & 07777u;
    node->mode_set = true;
    return true;
}

static bool import_storage(const char *path, bool directory, uint64_t bytes, void *unused)
{
    (void)unused;
    if (count == node_capacity || find(path) >= 0 || length(path) >= LUME_FILES_PATH_MAX)
        return false;
    unsigned index = count;
    if (!add(path, directory ? LUME_NODE_DIR : LUME_NODE_FILE, 0, 0, !storage->read_only))
        return false;
    nodes[index].bytes = bytes;
    nodes[index].persistent = true;
    return import_storage_metadata(&nodes[index], false);
}

bool lume_vfs_mount_storage_flags(const struct lume_storage_ops *ops, unsigned flags)
{
    if (!ops || storage || (flags & ~LUME_VFS_MOUNT_FLAGS))
        return false;
    int mount = find("/data");
    if (mount >= 0)
    {
        if (nodes[mount].kind != LUME_NODE_DIR || nodes[mount].references)
            return false;
        for (unsigned i = 0; i < count; ++i)
            if ((int)i != mount && nodes[i].linked && subtree(nodes[i].path, "/data"))
                return false;
    }
    else if (count == node_capacity)
        return false;
    unsigned before = count;
    struct node previous = {0};
    if (mount >= 0)
    {
        previous = nodes[mount];
        index_remove((unsigned)mount);
    }
    storage = ops;
    storage_mount_flags = flags;
    bool success;
    if (mount >= 0)
    {
        nodes[mount].persistent = true;
        nodes[mount].writable = !storage->read_only;
        nodes[mount].on_fat = false;
        nodes[mount].mode_set = false;
        nodes[mount].storage_inode = 0;
        index_insert((unsigned)mount);
        success = import_storage_metadata(&nodes[mount], true);
    }
    else
    {
        success = add("/data", LUME_NODE_DIR, 0, 0, !storage->read_only);
        if (success)
        {
            nodes[before].persistent = true;
            success = import_storage_metadata(&nodes[before], true);
        }
    }
    if (success && !ops->next)
        success = ops->walk && ops->walk(import_storage, 0);
    import_finish(before, success);
    if (!success)
    {
        if (mount >= 0)
        {
            index_remove((unsigned)mount);
            nodes[mount] = previous;
            index_insert_hashed((unsigned)mount, previous.path_hash);
        }
        storage = NULL;
        storage_mount_flags = 0;
    }
    if (success && storage->program_cache_owner)
        storage->program_cache_owner(program_cache_clear);
    return success;
}

bool lume_vfs_mount_storage(const struct lume_storage_ops *ops)
{
    return lume_vfs_mount_storage_flags(ops, 0);
}

static int program_reserve(size_t bytes)
{
    if (bytes <= program_capacity)
        return 0;
    int error = program_cache_clear();
    if (error)
        return error;
    void *replacement = program_allocate ? program_allocate(bytes) : NULL;
    if (!replacement)
        return -LUME_E2BIG;
    program_buffer = replacement;
    program_capacity = bytes;
    return 0;
}

static bool program_snapshot_equal(const struct lume_storage_snapshot *a,
                                   const struct lume_storage_snapshot *b)
{
    return a->mount == b->mount && a->object == b->object && a->generation == b->generation &&
           a->bytes == b->bytes;
}

static bool program_cached_image(struct node *node, const void **data, size_t *bytes, int *result)
{
    if (!storage_handles() || !storage->snapshot_handle || !storage->program_cache_owner)
        return false;
    void *handle;
    int error = storage->open(node->path, LUME_O_RDONLY, &handle);
    if (error == -LUME_EMFILE && program_cache.owner == storage && program_cache.handle)
    {
        error = program_cache_clear();
        if (!error)
            error = storage->open(node->path, LUME_O_RDONLY, &handle);
    }
    if (error)
    {
        *result = error;
        return true;
    }
    struct lume_storage_snapshot before, after;
    if (storage->snapshot_handle(handle, &before) || before.bytes > SIZE_MAX)
    {
        (void)storage->close(handle);
        return false;
    }
    *bytes = (size_t)before.bytes;
    if (program_cache.owner == storage && program_cache.handle &&
        program_snapshot_equal(&program_cache.snapshot, &before))
    {
        *result = storage->close(handle);
        *data = program_buffer;
        return true;
    }
    error = program_cache_clear();
    if (!error)
        error = program_reserve(*bytes);
    if (!error)
    {
        int count = storage->read_handle(handle, 0, program_buffer, *bytes);
        if (count < 0 || (size_t)count != *bytes)
            error = -LUME_EIO;
    }
    if (!error && !storage->snapshot_handle(handle, &after) &&
        program_snapshot_equal(&before, &after))
    {
        program_cache.owner = storage;
        program_cache.handle = handle;
        program_cache.snapshot = after;
    }
    else
    {
        int closed = storage->close(handle);
        if (!error)
            error = closed;
    }
    *data = program_buffer;
    *result = error;
    return true;
}

static int program_image(unsigned i, const void **data, size_t *bytes)
{
    struct node *n = &nodes[i];
    if (n->kind != LUME_NODE_FILE)
        return -LUME_ENOEXEC;
    int error;
    if (n->persistent && program_cached_image(n, data, bytes, &error))
        return error;
    error = program_cache_clear();
    if (error)
        return error;
    error = program_reserve((size_t)n->bytes);
    if (error)
        return error;
    *bytes = (size_t)n->bytes;
    if (n->persistent)
    {
        int r = storage->read(n->path, 0, program_buffer, *bytes);
        if (r < 0 || (size_t)r != *bytes)
            return -LUME_EIO;
        *data = program_buffer;
    }
    else if (n->on_fat)
    {
        if (lume_fat_read(&root_fat, n->fat_file, 0, program_buffer, *bytes))
            return -LUME_EIO;
        *data = program_buffer;
    }
    else
        *data = n->image ? n->image : n->data;
    return 0;
}

static int resolve_program(struct lume_files *files, const char *input, char *path)
{
    int index = resolve(files, input, path);
    if (index < 0)
        return index;
    if (nodes[index].persistent && (storage_mount_flags & LUME_VFS_MOUNT_NOEXEC))
        return -LUME_EACCES;
    unsigned flags = files->identity.euid ? 0 : LUME_ACCESS_DAC_OVERRIDE;
    int permission = node_access_with(files, &nodes[index], LUME_ACCESS_EXECUTE, flags);
    return permission ? permission : index;
}

int lume_vfs_program(struct lume_files *f, const char *input, const void **data, size_t *bytes)
{
    char p[LUME_FILES_PATH_MAX];
    int i = resolve_program(f, input, p);
    if (i < 0)
        return i;
    int error = program_image((unsigned)i, data, bytes);
    if (error || *bytes != LUME_BUSYBOX_ALIAS_BYTES)
        return error;
    static const unsigned char alias[] = LUME_BUSYBOX_ALIAS_CONTENT;
    const unsigned char *source = *data;
    for (unsigned n = 0; n < sizeof(alias); ++n)
        if (source[n] != alias[n])
            return 0;
    i = resolve_program(f, LUME_BUSYBOX_ALIAS_TARGET, p);
    return i < 0 ? i : program_image((unsigned)i, data, bytes);
}

static int program_reference(unsigned index)
{
    struct node *node = &nodes[index];
    if (node->kind != LUME_NODE_FILE)
        return -LUME_ENOEXEC;
    int object = new_object((int)index, -1, LUME_O_RDONLY);
    if (object < 0)
        return object;
    if (node->persistent && storage_handles())
    {
        int result =
            storage->open(node->path, LUME_O_RDONLY, &open_file_at(object)->storage_handle);
        if (result)
        {
            --node->references;
            open_file_recycle(open_file_at((unsigned)object));
            return result;
        }
    }
    return object;
}

int lume_vfs_program_hold(struct lume_files *files, const char *input, size_t *bytes)
{
    char path[LUME_FILES_PATH_MAX];
    int index = resolve_program(files, input, path);
    if (index < 0)
        return index;
    int object = program_reference((unsigned)index);
    if (object < 0)
        return object;
    *bytes = (size_t)nodes[index].bytes;
    if (*bytes == LUME_BUSYBOX_ALIAS_BYTES)
    {
        static const unsigned char alias[] = LUME_BUSYBOX_ALIAS_CONTENT;
        unsigned char content[LUME_BUSYBOX_ALIAS_BYTES];
        int64_t result = lume_vfs_pread_object(object, 0, content, sizeof(content));
        if (result < 0)
        {
            lume_vfs_drop(object);
            return (int)result;
        }
        if (result == sizeof(content) && !memcmp(content, alias, sizeof(content)))
        {
            lume_vfs_drop(object);
            index = resolve_program(files, LUME_BUSYBOX_ALIAS_TARGET, path);
            if (index < 0)
                return index;
            object = program_reference((unsigned)index);
            if (object >= 0)
                *bytes = (size_t)nodes[index].bytes;
        }
    }
    return object;
}

bool lume_vfs_exec_access(int object, const char *path, struct lume_inode_access *access)
{
    struct lume_file_identity identity;
    if (lume_vfs_object_identity(object, &identity))
        return false;
    const struct node *node = &nodes[open_file_at((unsigned)object)->node];
    if (!node->executables || !node->linked ||
        (node->persistent && (storage_mount_flags & LUME_VFS_MOUNT_NOSUID)) ||
        !setid_backing(node) || !(node->mode & 06000u) ||
        !(node->mode & 0111u))
        return false;
    /* An applet alias must not supply the credentials of its substituted image. */
    int resolved = find(path);
    if (resolved < 0 || !same_backing(node, &nodes[resolved]))
        return false;
    *access = (struct lume_inode_access){node->uid, node->gid, node->mode};
    return true;
}

/* The caller owns a held open-file reference for the complete positioned read. */
int64_t lume_vfs_pread_object(int object, uint64_t offset, void *data, uint64_t bytes)
{
    if (object < 0 || object >= OPEN_FILES || !open_file_count(open_file_at(object)))
        return -LUME_EBADF;
    struct open_file *file = open_file_at(object);
    if ((file->flags & 3) == LUME_O_WRONLY)
        return -LUME_EACCES;
    if (file->external)
        return file->external->pread ? file->external->pread(file->context, data, bytes, offset)
                                     : -LUME_ESPIPE;
    if (file->pipe >= 0 || nodes[file->node].kind != LUME_NODE_FILE)
        return -LUME_EINVAL;
    struct node *node = &nodes[file->node];
    if (offset >= node->bytes)
        return 0;
    if (bytes > node->bytes - offset)
        bytes = node->bytes - offset;
    if (node->persistent)
        return file->storage_handle
                   ? storage->read_handle(file->storage_handle, offset, data, bytes)
                   : storage->read(node->path, offset, data, bytes);
    if (node->on_fat)
        return lume_fat_read_cursor(&root_fat, node->fat_file, offset, data, bytes,
                                    &file->fat_cursor)
                   ? -LUME_EIO
                   : (int64_t)bytes;
    memcpy(data, (const unsigned char *)(node->image ? node->image : node->data) + offset, bytes);
    return (int64_t)bytes;
}

void lume_vfs_access_mode(struct lume_files *files, unsigned fd, unsigned mode)
{
    struct open_file *file = descriptor(files, fd);
    if (file && file->external && mode <= LUME_O_RDWR)
        file->flags = (file->flags & ~3u) | mode;
}

int lume_vfs_canonical_long(struct lume_files *files, const char *input,
                            char output[LUME_FILES_PATH_MAX])
{
    for (unsigned i = 0; i < LUME_FILES_PATH_MAX; ++i)
        output[i] = 0;
    int result = resolve(files, input, output);
    return result < 0 ? result : 0;
}

int lume_vfs_canonical(struct lume_files *files, const char *input, char output[LUME_PATH_MAX])
{
    char canonical[LUME_FILES_PATH_MAX];
    output[0] = 0;
    int result = lume_vfs_canonical_long(files, input, canonical);
    if (result)
        return result;
    size_t bytes = length(canonical) + 1;
    if (bytes > LUME_PATH_MAX)
        return -LUME_ENAMETOOLONG;
    copy(output, canonical, bytes);
    return 0;
}

static int resolve_at_with(struct lume_files *files, int64_t directory, const char *input,
                           char output[LUME_FILES_PATH_MAX], bool follow_final, bool follow_trailing,
                           unsigned access_flags)
{
    output[0] = 0;
    struct lume_files base;
    if (*input != '/' && directory != LUME_FILES_AT_CWD)
    {
        if (directory < 0 || directory >= LUME_FD_MAX)
            return -LUME_EBADF;
        struct open_file *file = descriptor(files, (uint64_t)directory);
        if (!file)
            return -LUME_EBADF;
        if (file->node < 0 || file->external || file->pipe >= 0 ||
            nodes[file->node].kind != LUME_NODE_DIR)
            return -LUME_ENOTDIR;
        if (!nodes[file->node].linked)
            return -LUME_ENOENT;
        base = *files;
        base.cwd_node = (unsigned)file->node + 1;
        base.cwd_generation = namespace_generation;
        files = &base;
    }
    int result = resolve_path_with(files, input, output, follow_final, follow_trailing, access_flags);
    if (result < 0)
    {
        size_t bytes = length(input), used = length(output);
        if (result == -LUME_ENOENT && used && bytes && input[bytes - 1] == '/')
        {
            if (used + 1 >= LUME_FILES_PATH_MAX)
                return -LUME_ENAMETOOLONG;
            output[used] = '/';
            output[used + 1] = 0;
        }
        return result;
    }
    return result;
}

static int resolve_at(struct lume_files *files, int64_t directory, const char *input,
                      char output[LUME_FILES_PATH_MAX], bool follow_final, bool follow_trailing)
{
    return resolve_at_with(files, directory, input, output, follow_final, follow_trailing, 0);
}

int lume_vfs_canonical_at(struct lume_files *files, int64_t directory, const char *input,
                          char output[LUME_FILES_PATH_MAX])
{
    int result = resolve_at(files, directory, input, output, true, true);
    return result < 0 ? result : lume_sandbox_path(files, output);
}

int lume_vfs_canonical_nofollow_at(struct lume_files *files, int64_t directory, const char *input,
                                   char output[LUME_FILES_PATH_MAX])
{
    int result = resolve_at(files, directory, input, output, false, true);
    return result < 0 ? result : lume_sandbox_path(files, output);
}

int lume_vfs_canonical_entry_at(struct lume_files *files, int64_t directory, const char *input,
                                char output[LUME_FILES_PATH_MAX])
{
    int result = resolve_at(files, directory, input, output, false, false);
    return result < 0 ? result : lume_sandbox_path(files, output);
}

static int64_t create_entry_at(struct lume_files *files, int64_t directory, const char *path,
                               unsigned kind, unsigned mode, const char *target)
{
    size_t bytes = length(path);
    if (!bytes)
        return -LUME_ENOENT;
    char canonical[LUME_FILES_PATH_MAX] = {0};
    int result = resolve_at(files, directory, path, canonical, false, false);
    if (result >= 0)
        return -LUME_EEXIST;
    if (result != -LUME_ENOENT || !canonical[0] || path[bytes - 1] == '/')
        return result;
    result = create_node(files, canonical, kind, mode, target);
    return result < 0 ? result : 0;
}

int64_t lume_vfs_mkfifo_at(struct lume_files *files, int64_t directory, const char *path,
                          unsigned mode)
{
    if (mode & ~0777u)
        return -LUME_EINVAL;
    return create_entry_at(files, directory, path, LUME_NODE_PIPE, mode, NULL);
}

int64_t lume_vfs_symlink_at(struct lume_files *files, int64_t directory, const char *path,
                           const char *target)
{
    size_t bytes = length(target);
    if (!bytes)
        return -LUME_ENOENT;
    if (bytes >= LUME_FILES_PATH_MAX)
        return -LUME_ENAMETOOLONG;
    return create_entry_at(files, directory, path, LUME_NODE_LINK, UINT32_MAX, target);
}

int64_t lume_vfs_link_at(struct lume_files *files, int64_t source_directory, const char *source,
                        int64_t target_directory, const char *target, bool follow)
{
    if (!*source || !*target)
        return -LUME_ENOENT;
    char from[LUME_FILES_PATH_MAX], to[LUME_FILES_PATH_MAX];
    int index = resolve_at(files, source_directory, source, from, follow, true);
    if (index < 0)
        return index;
    int result = lume_sandbox_path(files, from);
    if (!result)
        result = sandbox_mutation(files, from, true);
    if (result)
        return result;
    struct node *node = &nodes[index];
    if (node->kind == LUME_NODE_DIR)
        return -LUME_EPERM;
    if (node->kind != LUME_NODE_FILE && node->kind != LUME_NODE_LINK && node->kind != LUME_NODE_PIPE)
        return -LUME_EOPNOTSUPP;
    result = resolve_at(files, target_directory, target, to, false, false);
    if (result >= 0)
        return -LUME_EEXIST;
    if (result != -LUME_ENOENT || !to[0] || target[length(target) - 1] == '/')
        return result;
    result = lume_path_final_spelling(target, to);
    if (result)
        return result;
    unsigned parent = parent_node(to);
    if (!parent || parent == UINT32_MAX)
        return -LUME_ENOENT;
    const struct node *directory = &nodes[parent - 1];
    result = sandbox_mutation(files, to, true);
    if (!result)
        result = node_access(files, directory, LUME_ACCESS_WRITE | LUME_ACCESS_EXECUTE);
    if (result)
        return result;
    if (node->persistent != directory->persistent)
        return -LUME_EXDEV;
    if (!directory->writable)
        return -LUME_EROFS;
    if (!native_storage(node) || !storage->link)
        return -LUME_EOPNOTSUPP;
    struct lume_stat metadata;
    int64_t mode = stat_node((unsigned)index, &metadata);
    if (mode < 0)
        return mode;
    if (available_node() == node_capacity)
        return -LUME_ENOSPC;
    char *owned = lume_path_duplicate(to);
    if (!owned)
        return -LUME_ENOMEM;
    unsigned reserved = reserve_node(owned, node->kind);
    if (reserved == UINT32_MAX)
    {
        lume_path_release(owned);
        return -LUME_ENOSPC;
    }
    result = storage->link(from, to);
    if (result)
    {
        cancel_node(reserved);
        return result;
    }
    struct node *alias = &nodes[reserved];
    alias->persistent = true;
    alias->storage_inode = node->storage_inode;
    alias->fifo_identity = node->fifo_identity;
    alias->bytes = node->bytes;
    alias->uid = node->uid;
    alias->gid = node->gid;
    alias->mode = node->mode;
    alias->mode_set = node->mode_set;
    alias->links = node->links = metadata.links + 1;
    publish_node(reserved);
    native_aliases(node, false, true);
    return 0;
}

static int64_t stat_at(struct lume_files *files, int64_t directory, const char *input,
                       struct lume_stat *out, bool follow_final, struct stat_details *details)
{
    char canonical[LUME_FILES_PATH_MAX];
    int index = resolve_at(files, directory, input, canonical, follow_final, true);
    if (index == -LUME_ENOENT && terminal_stat && !strncmp(canonical, "/dev/pts/", 9))
        return stat_terminal(files, canonical, out, details);
    return stat_resolved(files, canonical, index, out, details);
}

int64_t lume_vfs_stat_at(struct lume_files *files, int64_t directory, const char *input,
                         struct lume_stat *out)
{
    return stat_at(files, directory, input, out, true, NULL);
}

int64_t lume_vfs_lstat_at(struct lume_files *files, int64_t directory, const char *input,
                          struct lume_stat *out)
{
    return stat_at(files, directory, input, out, false, NULL);
}

int lume_vfs_access_at(struct lume_files *files, int64_t directory, const char *path, unsigned mode,
                       unsigned flags)
{
    if ((mode & ~7u) || (flags & ~(LUME_FILES_ACCESS_NOFOLLOW | LUME_FILES_ACCESS_EFFECTIVE)))
        return -LUME_EINVAL;
    bool effective = flags & LUME_FILES_ACCESS_EFFECTIVE;
    uint32_t uid = effective ? files->identity.euid : files->identity.uid;
    unsigned credentials = effective ? 0 : LUME_ACCESS_REAL_IDS;
    if (!uid)
        credentials |= LUME_ACCESS_DAC_OVERRIDE;
    char canonical[LUME_FILES_PATH_MAX];
    int index = resolve_at_with(files, directory, path, canonical,
                                !(flags & LUME_FILES_ACCESS_NOFOLLOW), true, credentials);
    if (index == -LUME_ENOENT && terminal_stat && !strncmp(canonical, "/dev/pts/", 9))
    {
        int permission = lume_sandbox_path(files, canonical);
        if (permission)
            return permission;
        struct lume_stat metadata;
        struct lume_inode_access access;
        int64_t result = terminal_stat(files, canonical, uid, &metadata, &access);
        return result < 0 ? (int)result
                          : lume_file_access(&files->identity, &access, mode, credentials);
    }
    if (index < 0)
        return index;
    int permission = mode ? lume_sandbox_path(files, canonical)
                          : sandbox_stat(files, canonical, (unsigned)index);
    if (permission)
        return permission;
    const struct node *node = &nodes[index];
    int standard = standard_descriptor(node);
    if (standard >= 0)
        return standard_descriptor_access(files, (unsigned)standard, mode);
    bool writing = mode & LUME_ACCESS_WRITE;
    if (writing && (node->kind == LUME_NODE_FILE || node->kind == LUME_NODE_DIR))
    {
        permission = sandbox_mutation(files, canonical, false);
        if (permission)
            return permission;
    }
    if (mode && uid && privileged_device_io(node, writing))
        return -LUME_EACCES;
    permission = node_access_with(files, node, mode, credentials);
    if (!permission && writing && node->kind != LUME_NODE_PIPE && !node->writable)
        permission = -LUME_EROFS;
    return permission;
}

static int64_t stat_with_details(struct lume_files *files, int64_t directory, const char *path,
                                 unsigned flags, struct lume_files_stat_times *out,
                                 struct lume_file_allocation *allocation)
{
    struct lume_stat metadata = {0};
    struct stat_details details = {.allocation = allocation};
    int64_t mode;
    if (!*path && (flags & LUME_FILES_STAT_EMPTY_PATH))
    {
        if (directory == LUME_FILES_AT_CWD)
            mode = stat_path(files, lume_files_cwd(files), &metadata, &details);
        else
            mode = directory < 0 || directory >= LUME_FD_MAX
                       ? -LUME_EBADF
                       : stat_fd(files, (uint64_t)directory, &metadata, &details);
    }
    else
        mode = stat_at(files, directory, path, &metadata, !(flags & LUME_FILES_STAT_NOFOLLOW),
                       &details);
    if (mode >= 0)
    {
        details.metadata.stat.inode = metadata.inode;
        details.metadata.stat.size = metadata.size;
        details.metadata.stat.kind = metadata.kind;
        details.metadata.stat.links = metadata.links;
        *out = details.metadata;
    }
    return mode;
}

int64_t lume_vfs_stat_times(struct lume_files *files, int64_t directory, const char *path,
                            unsigned flags, struct lume_files_stat_times *out)
{
    return stat_with_details(files, directory, path, flags, out, NULL);
}

int64_t lume_vfs_stat_allocation(struct lume_files *files, int64_t directory, const char *path,
                                 unsigned flags, struct lume_files_stat_allocation *out)
{
    struct lume_files_stat_allocation result = {0};
    int64_t mode = stat_with_details(files, directory, path, flags, &result.metadata,
                                     &result.allocation);
    if (mode >= 0)
        *out = result;
    return mode;
}

int64_t lume_vfs_stat_extended(struct lume_files *files, int64_t directory, const char *path,
                               unsigned flags, struct lume_files_stat *out)
{
    struct lume_files_stat_times result;
    int64_t mode = lume_vfs_stat_times(files, directory, path, flags, &result);
    if (mode >= 0)
        *out = result.stat;
    return mode;
}

int64_t lume_vfs_set_times(struct lume_files *files, int64_t directory, const char *path, unsigned flags,
                           const struct lume_file_time *times)
{
    bool unchanged = times[0].nanoseconds == LUME_FILE_TIME_OMIT &&
                     times[1].nanoseconds == LUME_FILE_TIME_OMIT;
    int index;
    char canonical[LUME_FILES_PATH_MAX];
    if (flags & LUME_FILES_TIMES_FD)
    {
        if (directory < 0 || directory >= LUME_FD_MAX)
            return -LUME_EBADF;
        const struct open_file *file = descriptor(files, (uint64_t)directory);
        if (!file)
            return -LUME_EBADF;
        if (unchanged)
            return 0;
        if (file->node < 0 || file->external)
            return -LUME_EOPNOTSUPP;
        index = file->node;
    }
    else
    {
        index = resolve_at(files, directory, path, canonical, !(flags & LUME_FILES_TIMES_NOFOLLOW),
                           true);
        if (index < 0)
            return index;
        int result = lume_sandbox_path(files, canonical);
        if (result || unchanged)
            return result;
    }
    struct node *node = linked_backing(&nodes[index]);
    int result = sandbox_mutation(files, node->path, false);
    if (result)
        return result;
    if (!node->writable)
        return -LUME_EROFS;
    if (!native_storage(node) || !node->linked || !storage->set_times)
        return -LUME_EOPNOTSUPP;
    if (files->identity.euid && files->identity.euid != node->uid)
    {
        if (times[0].nanoseconds != LUME_FILE_TIME_NOW ||
            times[1].nanoseconds != LUME_FILE_TIME_NOW)
            return -LUME_EPERM;
        result = node_access(files, node, LUME_ACCESS_WRITE);
        if (result)
            return result;
    }
    return storage->set_times(node->path, times);
}

int64_t lume_vfs_readlink_at(struct lume_files *files, int64_t directory, const char *input,
                             void *out, size_t bytes)
{
    if (!bytes || !out)
        return -LUME_EINVAL;
    char canonical[LUME_FILES_PATH_MAX];
    int index = resolve_at(files, directory, input, canonical, false, true);
    if (index < 0)
        return index;
    int permission = sandbox_stat(files, canonical, (unsigned)index);
    if (permission)
        return permission;
    if (nodes[index].kind != LUME_NODE_LINK)
        return -LUME_EINVAL;
    return read_node_link(&nodes[index], out, bytes);
}

int lume_vfs_fd_path(struct lume_files *files, int64_t fd, char output[LUME_FILES_PATH_MAX])
{
    output[0] = 0;
    if (fd < 0 || fd >= LUME_FD_MAX)
        return -LUME_EBADF;
    struct open_file *file = descriptor(files, (uint64_t)fd);
    if (!file)
        return -LUME_EBADF;
    if (file->node < 0 || file->external || file->pipe >= 0 || !nodes[file->node].linked)
        return -LUME_ENOENT;
    if (files->identity.jailed || files->identity.euid)
        return -LUME_EACCES;
    const char *path = nodes[file->node].path;
    int permission = lume_sandbox_path(files, path);
    if (permission)
        return permission;
    copy(output, path, length(path) + 1);
    return 0;
}
