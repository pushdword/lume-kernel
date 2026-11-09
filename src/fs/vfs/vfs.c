#include "lume/vfs.h"
#include "lume/early.h"
#include "alias.h"
#include "lume/wait.h"
#include "lume/devfs.h"
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
#include "lume/worker.h"
#include <string.h>
#include "file_locks.h"

#define NODES 192u
#define CAPACITY 4096u
#define RAM_FILES 32u
static unsigned char ram_data[RAM_FILES][CAPACITY];
static bool ram_used[RAM_FILES];
static uint64_t descriptor_sequence;
static uint64_t poll_source_sequence;
static uint64_t poll_pending[LUME_POLL_WORDS];
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
static int (*terminal_open)(struct lume_files *, const char *, uint64_t);

void lume_vfs_terminal_open(int (*callback)(struct lume_files *, const char *, uint64_t))
{
    terminal_open = callback;
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
    for (unsigned task = 0; task < LUME_POLL_TASK_LIMIT; ++task)
    {
        unsigned word = task / 64u;
        uint64_t bit = UINT64_C(1) << (task % 64u);
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
    files->fd[fd].object = object;
    files->fd[fd].flags = flags;
    files->fd[fd].generation = descriptor_sequence;
}

/* Duplicated and inherited descriptors share one offset and access mode. Pipe endpoints
 * keep their channel alive until the last reference to each open description closes. */
#if defined(__aarch64__) || defined(LUME_ARCH_AARCH64) || defined(LUME_ARCH_AMD64)
#define OPEN_FILES 512u
#else
#define OPEN_FILES 256u
#endif
#define PIPES (2u * LUME_TASK_LIMIT)

static struct open_file
{
    int node, pipe;
    unsigned references, status;
    uint32_t flags;
    unsigned directory_authority_generation;
    bool closing, deferred_close;
    uint64_t offset;
    struct lume_fat_cursor fat_cursor;
    const struct lume_file_ops *external;
    void *context;
    void *storage_handle;
} open_files[OPEN_FILES];

static unsigned storage_reap_count, storage_reap_cursor;
static bool storage_reap_enabled, storage_reaping;

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

static struct channel
{
    unsigned readers, writers, head, count;
    struct lume_wait_queue readable, writable;
    struct lume_poll_source changed;
#if LUME_FILES_DYNAMIC
    unsigned char *data;
#else
    unsigned char data[LUME_PIPE_BYTES];
#endif
} channels[PIPES];

static struct node
{
    char *path;
    uint32_t kind;
    bool writable, linked, on_fat, persistent, pending_fat;
    bool mode_set, free_queued;
    unsigned mode;
    unsigned device;
    unsigned references;
    uint32_t path_hash, path_next, parent;
    struct lume_fat_file fat_file;
    const unsigned char *image;
    uint64_t bytes;
    unsigned char *data;
    struct lume_block *block;
} default_nodes[NODES];
static struct node *nodes = default_nodes;
static unsigned node_capacity = NODES;
static uint32_t *name_buckets;
static unsigned name_bucket_count;

static unsigned count;
static unsigned free_nodes;
static unsigned namespace_generation;
static struct lume_block *block_devices[LUME_DEVICE_LIMIT];
static struct lume_fat root_fat;
static const struct lume_storage_ops *storage;

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

static void release_ram(struct node *n)
{
    if (n->linked || n->references)
        return;
    if (n->data)
        for (unsigned i = 0; i < RAM_FILES; ++i)
            if (n->data == ram_data[i])
                ram_used[i] = false;
    n->data = 0;
    lume_path_release(n->path);
    n->path = NULL;
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

static int find(const char *p)
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

/* Resolve components in order so a regular file followed by /.. remains invalid. */
static int resolve(struct lume_files *f, const char *input, char out[LUME_FILES_PATH_MAX])
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
    if (f->identity.euid)
        return -LUME_EACCES;
    char raw[LUME_FILES_PATH_MAX * 2];
    const char *cwd = lume_files_cwd(f);
    if (*input != '/' && !cwd)
        return -LUME_ENOENT;
    size_t prefix = *input == '/' ? 0 : length(cwd);
    size_t bytes = length(input);
    if (prefix + (prefix != 0) + bytes >= sizeof(raw))
        return -LUME_ENAMETOOLONG;
    if (prefix)
    {
        copy(raw, cwd, prefix);
        raw[prefix++] = '/';
    }
    copy(raw + prefix, input, bytes + 1);
    out[0] = '/';
    out[1] = 0;
    size_t used = 1;
    int index = find("/");
    const char *cursor = raw;
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
        }
    }
    if (index >= 0 && bytes && input[bytes - 1] == '/' && nodes[index].kind != LUME_NODE_DIR)
        return -LUME_ENOTDIR;
    return index;
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
        lume_path_release(n->path);
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
    unsigned slot = LUME_DEVICE_LIMIT;
    for (unsigned i = 0; i < LUME_DEVICE_LIMIT; ++i)
    {
        if (block_devices[i] && equal(block_devices[i]->device->name, name))
            return -LUME_EEXIST;
        if (!block_devices[i] && slot == LUME_DEVICE_LIMIT)
            slot = i;
    }
    if (slot == LUME_DEVICE_LIMIT)
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

void lume_vfs_init(const void *shell, size_t bytes)
{
    (void)program_cache_clear();
    if (storage && storage->program_cache_owner)
        storage->program_cache_owner(NULL);
    if (storage_handles())
        for (unsigned i = 0; i < OPEN_FILES; ++i)
            if ((open_files[i].references || open_files[i].deferred_close) &&
                open_files[i].storage_handle)
                (void)storage->close(open_files[i].storage_handle);
    for (unsigned task = 0; task < LUME_POLL_TASK_LIMIT; ++task)
        lume_vfs_poll_release(task);
    memset(poll_pending, 0, sizeof(poll_pending));
    memset(poll_subscriptions, 0, sizeof(poll_subscriptions));
    console_source = (struct lume_poll_source){0};
    lume_poll_source_reset(&console_source);
    poll_input_ready = false;
    lume_file_modes_reset();
    file_lock_count = 0;
    for (unsigned i = 0; i < OPEN_FILES; ++i)
        open_files[i] = (struct open_file){0};
    storage_reap_count = storage_reap_cursor = 0;
    storage_reap_enabled = storage_reaping = false;
    for (unsigned i = 0; i < PIPES; ++i)
    {
#if LUME_FILES_DYNAMIC
        if (channels[i].data && !lume_page_free((uintptr_t)channels[i].data))
            lume_fail("pipe backing release");
#endif
        channels[i] = (struct channel){0};
    }
    for (unsigned i = 0; i < count; ++i)
    {
        lume_path_release(nodes[i].path);
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
    for (unsigned i = 0; i < RAM_FILES; ++i)
        ram_used[i] = false;
    add_required("/", LUME_NODE_DIR, 0, 0, false);
    add_required("/dev", LUME_NODE_DIR, 0, 0, false);
    add_required("/bin", LUME_NODE_DIR, 0, 0, false);
    add_required("/etc", LUME_NODE_DIR, 0, 0, false);
    add_required("/tmp", LUME_NODE_DIR, 0, 0, true);
    add_required("/dev/console", LUME_NODE_CHAR, 0, 0, true);
    add_required("/dev/null", LUME_NODE_CHAR, 0, 0, true);
    nodes[count - 1].device = 1;
    add_required("/dev/zero", LUME_NODE_CHAR, 0, 0, true);
    nodes[count - 1].device = 2;
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
}

static int new_object(int node, int pipe, uint32_t flags)
{
    for (unsigned i = 0; i < OPEN_FILES; ++i)
        if (!open_files[i].references && !open_files[i].closing)
        {
            open_files[i] =
                (struct open_file){.node = node, .pipe = pipe, .references = 1, .flags = flags};
            if (node >= 0)
                ++nodes[node].references;
            else if (pipe >= 0 && (flags & 3))
                ++channels[pipe].writers;
            else if (pipe >= 0)
                ++channels[pipe].readers;
            return (int)i;
        }
    return -LUME_ENOMEM;
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
    f->identity.limit[LUME_LIMIT_FILES].soft = f->identity.limit[LUME_LIMIT_FILES].hard =
        LUME_FD_MAX;
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
            ++open_files[out->fd[i].object].references;
    }
    if (authorities)
    {
        for (unsigned i = 0; i < LUME_SANDBOX_DIRECTORY_LIMIT; ++i)
            if (authorities->entry[i].active)
                ++open_files[authorities->entry[i].object].references;
        if (authorities->generation == namespace_generation && authorities->x11_active)
            ++open_files[authorities->x11_object].references;
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
    for (unsigned i = 0; i < LUME_FD_MAX; ++i)
        (void)lume_vfs_close(f, i);
    drop_directory(f);
#if LUME_FILES_DYNAMIC
    lume_poll_sources_retire_range(f->fd, sizeof(*f->fd) * LUME_FD_MAX);
    struct lume_fd *table = f->fd;
    f->fd = NULL;
    lume_kernel_free(table);
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
               ? &open_files[f->fd[fd].object]
               : 0;
}

bool lume_vfs_console_io(struct lume_files *files, uint64_t fd)
{
    struct open_file *file = descriptor(files, fd);
    return file && file->references && !file->external && file->pipe < 0 &&
           nodes[file->node].kind == LUME_NODE_CHAR && !nodes[file->node].block &&
           nodes[file->node].device <= 2;
}

bool lume_vfs_pipe_io(struct lume_files *files, uint64_t fd)
{
    struct open_file *file = descriptor(files, fd);
    return file && file->references && !file->external && file->pipe >= 0;
}

bool lume_vfs_memory_io(struct lume_files *files, uint64_t fd)
{
    struct open_file *file = descriptor(files, fd);
    return file && file->references &&
           (file->external ? file->external->memory_io : file->pipe >= 0);
}

bool lume_vfs_storage_io(struct lume_files *files, uint64_t fd)
{
    struct open_file *file = descriptor(files, fd);
    return file && file->references && !file->external && file->pipe < 0 &&
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
        if (object >= 0 && (files->fd[i].flags & LUME_FD_CLOEXEC) && open_files[object].external)
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
    while (fd < files->identity.limit[LUME_LIMIT_FILES].soft && files->fd[fd].object >= 0)
        ++fd;
    if (fd >= files->identity.limit[LUME_LIMIT_FILES].soft)
        return -LUME_EMFILE;
    int object = new_object(-1, -1, LUME_O_RDWR);
    if (object < 0)
        return object;
    open_files[object].external = ops;
    open_files[object].context = context;
    open_files[object].status = flags & LUME_FD_NONBLOCK;
    bind_descriptor(files, fd, object, flags & LUME_FD_CLOEXEC);
    return (int)fd;
}

void *lume_vfs_external(struct lume_files *files, unsigned fd, const struct lume_file_ops *ops)
{
    struct open_file *object = descriptor(files, fd);
    return object && object->external == ops ? object->context : 0;
}

int lume_vfs_hold(struct lume_files *files, unsigned fd)
{
    struct open_file *object = descriptor(files, fd);
    if (!object)
        return -LUME_EBADF;
    ++object->references;
    return files->fd[fd].object;
}

int lume_vfs_install(struct lume_files *files, int object)
{
#if LUME_FILES_DYNAMIC
    if (!files->fd)
        return -LUME_ENOMEM;
#endif
    if (object < 0 || object >= (int)OPEN_FILES || !open_files[object].references)
        return -LUME_EBADF;
    for (unsigned i = 0; i < files->identity.limit[LUME_LIMIT_FILES].soft; ++i)
        if (files->fd[i].object < 0)
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
        if (open_files[entry->object].node != file->node)
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
        return open_files[authorities->x11_object].node == file->node ? 0 : -LUME_EBUSY;
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
           open_files[authorities->x11_object].node == index;
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
            directory_contains((unsigned)open_files[entry->object].node, (unsigned)index))
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

int64_t lume_vfs_open(struct lume_files *f, const char *input, uint64_t flags)
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
        int fd = terminal_open(f, input, flags);
        if (fd != -LUME_ENOSYS)
            return fd;
    }
    const uint64_t allowed = 3 | LUME_O_CREAT | LUME_O_TRUNC | LUME_O_APPEND | LUME_O_EXCL;
    if ((flags & ~allowed) || (flags & 3) == 3 ||
        (!(flags & 3) && (flags & (LUME_O_TRUNC | LUME_O_APPEND))))
        return -LUME_EINVAL;
    unsigned fd = 0;
    while (fd < f->identity.limit[LUME_LIMIT_FILES].soft && f->fd[fd].object >= 0)
        ++fd;
    if (fd >= f->identity.limit[LUME_LIMIT_FILES].soft)
        return -LUME_EMFILE;
    char p[LUME_FILES_PATH_MAX] = {0};
    int index = resolve(f, input, p);
    if (index >= 0 && (flags & LUME_O_CREAT) && (flags & LUME_O_EXCL))
        return -LUME_EEXIST;
    if (index == -LUME_ENOENT && (flags & LUME_O_CREAT) && p[0])
    {
        if (input[length(input) - 1] == '/')
            return -LUME_ENOENT;
        char parent[LUME_FILES_PATH_MAX];
        copy(parent, p, length(p) + 1);
        size_t k = length(parent);
        while (k && parent[k - 1] != '/')
            --k;
        parent[k > 1 ? k - 1 : 1] = 0;
        int directory = find(parent);
        if (directory < 0)
            return directory;
        if (!nodes[directory].writable)
            return -LUME_EROFS;
        if (available_node() == node_capacity)
            return -LUME_ENOSPC;
        char *owned = lume_path_duplicate(p);
        if (!owned)
            return -LUME_ENOMEM;
        unsigned reserved = reserve_node(owned, LUME_NODE_FILE);
        if (reserved == UINT32_MAX)
        {
            lume_path_release(owned);
            return -LUME_ENOSPC;
        }
        if (nodes[directory].persistent)
        {
            int e = storage->create(p, false);
            if (e)
            {
                cancel_node(reserved);
                return e;
            }
        }
        index = (int)reserved;
        nodes[index].persistent = nodes[directory].persistent;
        publish_node((unsigned)index);
    }
    if (index < 0)
        return index;
    struct node *n = &nodes[index];
    if (n->block && n->block->device->state != LUME_DEVICE_READY)
        return -LUME_EIO;
    if ((flags & 3) && n->kind == LUME_NODE_DIR)
        return -LUME_EISDIR;
    if ((flags & 3) && !n->writable)
        return -LUME_EROFS;
    if ((flags & LUME_O_TRUNC) && n->kind == LUME_NODE_FILE)
    {
        if (n->persistent)
        {
            int e = storage->truncate(p);
            if (e)
                return e;
        }
        n->bytes = 0;
    }
    int object = new_object(index, -1, (uint32_t)flags);
    if (object < 0)
        return object;
    if (n->persistent && n->kind == LUME_NODE_FILE && storage_handles())
    {
        int result =
            storage->open(n->path, (unsigned)(flags & 3), &open_files[object].storage_handle);
        if (result)
        {
            --n->references;
            open_files[object] = (struct open_file){0};
            return result;
        }
    }
    bind_descriptor(f, (unsigned)fd, object, 0);
    return fd;
}

static int finish_object(struct open_file *d)
{
    int result = 0;
    if (d->external)
        d->external->close(d->context);
    else if (d->pipe >= 0)
    {
        struct channel *c = &channels[d->pipe];
        if (d->flags & 3)
            --c->writers;
        else
            --c->readers;
        lume_wait_wake(&c->readable);
        lume_wait_wake(&c->writable);
        lume_poll_source_notify(&c->changed);
        if (!c->readers && !c->writers)
        {
#if LUME_FILES_DYNAMIC
            if (c->data && !lume_page_free((uintptr_t)c->data))
                lume_fail("pipe backing release");
#endif
            *c = (struct channel){0};
        }
    }
    else
    {
        if (d->storage_handle)
        {
            result = storage->close(d->storage_handle);
            d->storage_handle = 0;
        }
        --nodes[d->node].references;
        release_ram(&nodes[d->node]);
    }
    d->closing = false;
    return result;
}

static int release_object(struct open_file *d, bool deferred)
{
    if (!d->references)
        return -LUME_EBADF;
    if (--d->references)
        return 0;
    /* A suspended close callback retains its object until device cleanup returns. */
    d->closing = true;
    file_locks_drop((int)(d - open_files));
    if (deferred && storage_reap_enabled && !d->external && d->pipe < 0 && d->storage_handle)
    {
        /* The description retains its node and handle until STORAGE finishes closing it. */
        d->deferred_close = true;
        ++storage_reap_count;
        return 0;
    }
    return finish_object(d);
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
    for (unsigned seen = 0; seen < OPEN_FILES && done < budget && storage_reap_count; ++seen)
    {
        struct open_file *d = open_files + storage_reap_cursor;
        if (++storage_reap_cursor == OPEN_FILES)
            storage_reap_cursor = 0;
        if (!d->deferred_close)
            continue;
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
        (void)release_object(&open_files[object], true);
}

int64_t lume_vfs_close(struct lume_files *f, uint64_t fd)
{
    struct open_file *d = descriptor(f, fd);
    if (!d)
        return -LUME_EBADF;
    bind_descriptor(f, (unsigned)fd, -1, 0);
    return release_object(d, false);
}

int64_t lume_vfs_open_compat(struct lume_files *f, const char *input, uint64_t flags)
{
    char p[LUME_FILES_PATH_MAX];
    int index = resolve(f, input, p);
    if (index >= 0 && (flags & 3) && nodes[index].kind == LUME_NODE_FILE && nodes[index].references)
        return -LUME_EBUSY;
    return lume_vfs_open(f, input, flags);
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
    if (d->pipe < 0 && !nodes[d->node].device && (f->identity.euid || f->identity.jailed))
        return -LUME_EACCES;
    if (d->pipe >= 0)
    {
        struct channel *c = &channels[d->pipe];
        if (!bytes)
            return 0;
        if (!c->count)
            return c->writers ? -LUME_EAGAIN : 0;
        if (bytes > c->count)
            bytes = c->count;
        for (unsigned i = 0; i < bytes; ++i)
            ((unsigned char *)data)[i] = c->data[(c->head + i) % LUME_PIPE_BYTES];
        c->head = (c->head + (unsigned)bytes) % LUME_PIPE_BYTES;
        c->count -= (unsigned)bytes;
        lume_wait_wake(&c->writable);
        lume_poll_source_notify(&c->changed);
        return (int64_t)bytes;
    }
    struct node *n = &nodes[d->node];
    if (n->block)
        return block_io(d, n, data, bytes, false);
    if (n->kind == LUME_NODE_DIR)
        return -LUME_EISDIR;
    if (!bytes)
        return 0;
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
    if (d->pipe < 0 && !nodes[d->node].device && (f->identity.euid || f->identity.jailed))
        return -LUME_EACCES;
    if (d->pipe >= 0)
    {
        struct channel *c = &channels[d->pipe];
        if (!bytes)
            return 0;
        if (!c->readers)
            return -LUME_EPIPE;
        unsigned space = LUME_PIPE_BYTES - c->count;
        if (!space || (bytes <= LUME_PIPE_ATOMIC && bytes > space))
            return -LUME_EAGAIN;
        if (bytes > space)
            bytes = space;
        for (unsigned i = 0; i < bytes; ++i)
            c->data[(c->head + c->count + i) % LUME_PIPE_BYTES] = ((const unsigned char *)data)[i];
        c->count += (unsigned)bytes;
        lume_wait_wake(&c->readable);
        lume_poll_source_notify(&c->changed);
        return (int64_t)bytes;
    }
    struct node *n = &nodes[d->node];
    if (n->block)
        return block_io(d, n, (void *)data, bytes, true);
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
    if (!bytes)
        return 0;
    uint64_t offset = d->flags & LUME_O_APPEND ? n->bytes : d->offset;
    if (offset > f->identity.limit[LUME_LIMIT_FILE_SIZE].soft ||
        bytes > f->identity.limit[LUME_LIMIT_FILE_SIZE].soft - offset)
        return -27;
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
    return (int64_t)bytes;
}

int64_t lume_vfs_write(struct lume_files *f, uint64_t fd, const void *data, uint64_t bytes)
{
    int permission = lume_vfs_sandbox_access(f, fd, true);
    if (permission)
        return permission;
    return write_description(f, descriptor(f, fd), data, bytes);
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
        *positioned = *description;
        positioned->offset = (uint64_t)offset;
        return 0;
    }
    struct node *node = &nodes[description->node];
    if (node->kind == LUME_NODE_DIR)
        return -LUME_EISDIR;
    if (node->kind != LUME_NODE_FILE && !node->block)
        return -LUME_ESPIPE;
    /* A transfer owns its cursor while duplicated descriptors keep their shared offset. */
    *positioned = *description;
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
        .owner = (int)(description - open_files), .node = description->node, .type = request->type};
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
    int owner = (int)(description - open_files);
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
    if (n->kind == LUME_NODE_CHAR)
        return -LUME_ESPIPE;
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
    if (d->pipe >= 0)
    {
        *s = (struct lume_stat){UINT64_C(0x100000) + (unsigned)d->pipe, 0, LUME_NODE_PIPE, 0};
        return 0;
    }
    struct node *n = &nodes[d->node];
    *s = (struct lume_stat){(uint64_t)d->node + 1, n->bytes, n->kind, 0};
    return 0;
}

static int64_t directory_entries(struct lume_files *f, uint64_t fd, void *out, uint64_t bytes,
                                 bool extended)
{
    struct open_file *d = descriptor(f, fd);
    if (!d)
        return -LUME_EBADF;
    if (f->identity.jailed)
        return 0;
    if (f->identity.euid)
        return -LUME_EACCES;
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
    size_t record_bytes = extended ? sizeof(struct lume_files_dirent) : sizeof(struct lume_dirent);
    if (bytes < record_bytes)
        return -LUME_EINVAL;
    size_t prefix = length(n->path);
    uint64_t used = 0;
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
            extended ? LUME_FILES_NAME_MAX + 1 : sizeof(((struct lume_dirent *)0)->name);
        if (name_bytes >= name_capacity)
        {
            --d->offset;
            return used ? (int64_t)used : -LUME_ENAMETOOLONG;
        }
        if (extended)
        {
            struct lume_files_dirent entry = {
                .inode = i + 1, .kind = nodes[i].kind, .name_bytes = (uint32_t)name_bytes};
            copy(entry.name, name, name_bytes + 1);
            copy((unsigned char *)out + used, &entry, sizeof(entry));
        }
        else
        {
            struct lume_dirent entry = {.inode = i + 1, .kind = nodes[i].kind};
            copy(entry.name, name, name_bytes + 1);
            copy((unsigned char *)out + used, &entry, sizeof(entry));
        }
        used += record_bytes;
    }
    return (int64_t)used;
}

int64_t lume_vfs_getdents(struct lume_files *files, uint64_t fd, void *out, uint64_t bytes)
{
    return directory_entries(files, fd, out, bytes, false);
}

int64_t lume_vfs_getdents_long(struct lume_files *files, uint64_t fd, void *out, uint64_t bytes)
{
    return directory_entries(files, fd, out, bytes, true);
}

static unsigned default_mode_at(const struct node *n, const char *path)
{
    unsigned mode = (n->kind == LUME_NODE_DIR     ? 0040555
                     : n->kind == LUME_NODE_CHAR  ? 0020444
                     : n->kind == LUME_NODE_BLOCK ? 0060444
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

static int64_t stat_node(unsigned index, struct lume_stat *out)
{
    const struct node *n = &nodes[index];
    *out = (struct lume_stat){index + 1, n->bytes, n->kind, 0};
    unsigned mode = default_mode(n);
    if (n->mode_set)
        mode = (mode & ~0777u) | n->mode;
    return n->persistent && n->linked ? lume_file_mode_get(n->path, mode, true) : mode;
}

int64_t lume_vfs_stat_mode(struct lume_files *f, uint64_t fd, struct lume_stat *out)
{
    struct open_file *d = descriptor(f, fd);
    if (d && (d->pipe >= 0 || d->external))
    {
        (void)lume_vfs_stat(f, fd, out);
        return d->external ? (d->external->kind == LUME_NODE_FILE     ? 0100600
                              : d->external->kind == LUME_NODE_SOCKET ? 0140600
                                                                      : 0020600)
                           : 0010600;
    }
    return d ? stat_node((unsigned)d->node, out) : -LUME_EBADF;
}

int64_t lume_vfs_stat_inode(uint64_t inode, struct lume_stat *out)
{
    if (!inode || inode > count || !nodes[inode - 1].linked)
        return -LUME_ENOENT;
    return stat_node((unsigned)inode - 1, out);
}

int64_t lume_vfs_stat_path(struct lume_files *f, const char *input, struct lume_stat *out)
{
    char p[LUME_FILES_PATH_MAX];
    int i = resolve(f, input, p);
    if (i >= 0)
    {
        int permission = lume_sandbox_path(f, p);
        if (permission)
            return permission;
    }
    return i < 0 ? i : stat_node((unsigned)i, out);
}

static int volume_node(struct lume_files *files, const struct node *node,
                       struct lume_files_volume *out)
{
    int permission = lume_sandbox_path(files, node->path);
    if (permission)
        return permission;
    if (!node->persistent || !storage || !storage->volume_info)
        return -LUME_EOPNOTSUPP;
    return storage->volume_info(out);
}

int lume_vfs_volume_path(struct lume_files *files, const char *input, struct lume_files_volume *out)
{
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

static int64_t file_mode(struct lume_files *files, unsigned operation,
                         struct lume_file_mode_request *request, const char *input, bool compat)
{
    if ((files->identity.euid || files->identity.jailed) &&
        !(files->identity.jailed && operation == LUME_FILE_MODE_OPEN &&
          request->flags == LUME_O_RDONLY))
        return -LUME_EPERM;
    if (request->version != LUME_FILE_MODE_VERSION || request->bytes != sizeof(*request) ||
        operation > LUME_FILE_MODE_MKDIR ||
        (request->mode != UINT32_MAX && request->mode & ~0777u) ||
        (request->uid != UINT32_MAX && request->uid) ||
        (request->gid != UINT32_MAX && request->gid))
        return -LUME_EINVAL;
    if (request->mode == UINT32_MAX && operation >= LUME_FILE_MODE_OPEN)
        return -LUME_EINVAL;
    char canonical[LUME_FILES_PATH_MAX];
    int index;
    if (operation == LUME_FILE_MODE_FD)
    {
        struct open_file *description = descriptor(files, (unsigned)request->fd);
        if (!description || description->external || description->pipe >= 0)
            return -LUME_EBADF;
        index = description->node;
    }
    else
        index = resolve(files, input, canonical);
    bool created = index == -LUME_ENOENT;
    int fd = -1;
    if (operation == LUME_FILE_MODE_OPEN)
    {
        if (request->descriptor_flags & ~(LUME_FD_CLOEXEC | LUME_FD_NONBLOCK))
            return -LUME_EINVAL;
        fd = (int)(compat ? lume_vfs_open_compat(files, input, request->flags)
                          : lume_vfs_open(files, input, request->flags));
        if (fd < 0)
            return fd;
        files->fd[fd].flags = request->descriptor_flags & LUME_FD_CLOEXEC;
        struct open_file *description = descriptor(files, (unsigned)fd);
        description->status = request->descriptor_flags & LUME_FD_NONBLOCK;
        if (description->external)
            return fd;
        index = description->node;
    }
    else if (operation == LUME_FILE_MODE_MKDIR)
    {
        int result = (int)lume_vfs_mkdir(files, input);
        if (result)
            return result;
        index = resolve(files, input, canonical);
    }
    if (index < 0)
        return index;
    struct node *node = &nodes[index];
    if (request->mode == UINT32_MAX || (operation == LUME_FILE_MODE_OPEN && !created))
        return fd >= 0 ? fd : 0;
    int permission = sandbox_mutation(files, node->path, false);
    if (permission)
    {
        if (fd >= 0)
            (void)lume_vfs_close(files, (unsigned)fd);
        return permission;
    }
    if (!node->writable)
    {
        if (fd >= 0)
            (void)lume_vfs_close(files, (unsigned)fd);
        return -LUME_EROFS;
    }
    unsigned mode = request->mode;
    if (operation == LUME_FILE_MODE_OPEN || operation == LUME_FILE_MODE_MKDIR)
        mode &= ~files->identity.umask;
    int result = 0;
    if (node->persistent && node->linked)
        result = mode == (default_mode(node) & 0777u) ? lume_file_mode_remove(node->path, true)
                                                      : lume_file_mode_set(node->path, mode, true);
    if (result)
    {
        if (fd >= 0)
            (void)lume_vfs_close(files, (unsigned)fd);
        return result;
    }
    node->mode = mode;
    node->mode_set = true;
    return fd >= 0 ? fd : 0;
}

int64_t lume_vfs_file_mode(struct lume_files *files, unsigned operation,
                           struct lume_file_mode_request *request, const char *input)
{
    return file_mode(files, operation, request, input, true);
}

int64_t lume_vfs_file_mode_long(struct lume_files *files, unsigned operation,
                                struct lume_file_mode_request *request, const char *input)
{
    return file_mode(files, operation, request, input, false);
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
    if (f->identity.euid || f->identity.jailed)
        return -LUME_EACCES;
    if (length > INT64_MAX)
        return -LUME_EINVAL;
    if (length > n->bytes && length > f->identity.limit[LUME_LIMIT_FILE_SIZE].soft)
        return -27;
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
    int source = resolve(files, from, a), target = resolve(files, to, b);
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
    if (same && equal(a, b))
        return 0;
    if (!same && target >= 0 && !replace)
        return -LUME_EEXIST;
    struct node *incoming = &nodes[source];
    bool directory = incoming->kind == LUME_NODE_DIR;
    if (incoming->kind != LUME_NODE_FILE && !directory)
        return -LUME_EINVAL;
    if (!incoming->writable || incoming->block || equal(a, "/tmp") || equal(a, "/data"))
        return -LUME_EROFS;
    if (lume_file_mode_namespace_busy(a) || lume_file_mode_namespace_busy(b))
        return -LUME_EBUSY;
    size_t to_bytes = length(to);
    if (!directory && to_bytes && to[to_bytes - 1] == '/')
        return -LUME_ENOTDIR;
    copy(parent, b, length(b) + 1);
    char *last = strrchr(parent, '/');
    if (last == parent)
        parent[1] = 0;
    else
        *last = 0;
    int parent_node = find(parent);
    if (parent_node < 0)
        return parent_node;
    if (nodes[parent_node].kind != LUME_NODE_DIR)
        return -LUME_ENOTDIR;
    if (!nodes[parent_node].writable)
        return -LUME_EROFS;
    if (incoming->persistent != nodes[parent_node].persistent)
        return -18;
    if (directory && !same && subtree(b, a))
        return -LUME_EINVAL;
    unsigned previous_permissions = 0;
    struct node *previous = target >= 0 && !same ? &nodes[target] : NULL;
    struct lume_stat metadata;
    if (previous)
    {
        if (previous->kind != incoming->kind)
            return directory ? -LUME_ENOTDIR : -LUME_EISDIR;
        if (!previous->writable || previous->block || equal(b, "/tmp") || equal(b, "/data"))
            return -LUME_EROFS;
        if (previous->persistent != incoming->persistent)
            return -18;
        if (directory)
            for (unsigned i = 0; i < count; ++i)
                if ((int)i != target && nodes[i].linked && subtree(nodes[i].path, previous->path))
                    return -LUME_ENOTEMPTY;
        int64_t mode = stat_node((unsigned)target, &metadata);
        if (mode < 0)
            return mode;
        previous_permissions = (unsigned)mode & 0777u;
    }
    bool persistent = incoming->persistent;
    if (persistent && incoming->kind == LUME_NODE_FILE && incoming->references &&
        !storage->rename_state)
        return -LUME_EBUSY;
    if (persistent && previous && previous->kind == LUME_NODE_FILE && previous->references &&
        !storage->remove_state)
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
        int64_t permissions = stat_node(i, &metadata);
        if (permissions < 0)
        {
            lume_path_release(owned);
            result = (int)permissions;
            break;
        }
        struct rename_path *entry = &paths[prepared];
        *entry = (struct rename_path){.node = i,
                                      .permissions = (unsigned)permissions & 0777u,
                                      .hash = namespace_hash(owned),
                                      .old_path = nodes[i].path,
                                      .new_path = owned};
        if (persistent)
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
    if (!result && persistent)
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
        if (persistent && state == LUME_STORAGE_NAMESPACE_UNCHANGED)
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
    incoming->parent = (unsigned)parent_node + 1;
    if (previous)
    {
        previous->mode = previous_permissions;
        previous->mode_set = true;
        index_remove((unsigned)target);
        previous->linked = false;
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
    if (!result && persistent)
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

int64_t lume_vfs_chdir(struct lume_files *f, const char *input)
{
    char p[LUME_FILES_PATH_MAX];
    int i = resolve(f, input, p);
    if (i >= 0)
    {
        int permission = lume_sandbox_path(f, p);
        if (permission)
            return permission;
    }
    if (i < 0)
        return i;
    if (nodes[i].kind != LUME_NODE_DIR)
        return -LUME_ENOTDIR;
    hold_directory(f, (unsigned)i);
    return 0;
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
    (void)lume_vfs_close(f, to);
    bind_descriptor(f, (unsigned)to, f->fd[from].object, 0);
    ++d->references;
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
                         (sample_console ? lume_console_ready() : poll_input_ready))
                    ? LUME_POLLIN
                    : 0) |
               ((d->flags & 3) ? LUME_POLLOUT : 0);
    }
    struct channel *c = &channels[d->pipe];
    if (!(d->flags & 3))
        return (c->count || !c->writers ? LUME_POLLIN : 0) | (!c->writers ? LUME_POLLHUP : 0);
    return !c->readers                                      ? LUME_POLLERR
           : LUME_PIPE_BYTES - c->count >= LUME_PIPE_ATOMIC ? LUME_POLLOUT
                                                            : 0;
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
        struct open_file *file = &open_files[files->fd[fd].object];
        struct lume_poll_source *source = 0;
        if (file->external && file->external->source)
            source = file->external->source(file->context);
        else if (!file->external && file->pipe >= 0)
            source = &channels[file->pipe].changed;
        else if (!file->external && nodes[file->node].kind == LUME_NODE_CHAR &&
                 !nodes[file->node].device && (watch->records[i].events & LUME_POLLIN))
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
        const struct open_file *file = &open_files[files->fd[fd].object];
        if (!file->external && file->pipe < 0 && nodes[file->node].kind == LUME_NODE_CHAR &&
            !nodes[file->node].device && (file->flags & 3) != LUME_O_WRONLY)
            return true;
    }
    return false;
}

int64_t lume_vfs_fd_call(struct lume_files *f, unsigned op, struct lume_fd_request *r)
{
    if (!files_ready(f))
        return -LUME_ENOMEM;
    if (r->version != LUME_FD_VERSION || r->bytes != sizeof(*r))
        return -LUME_EINVAL;
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
        for (unsigned i = 0; i < OPEN_FILES; ++i)
            info.objects += open_files[i].references != 0;
        for (unsigned i = 0; i < PIPES; ++i)
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
    if (op == LUME_FD_SOCKETPAIR || op == LUME_FD_SPAWN)
        return -LUME_ENOSYS;
    if (op == LUME_FD_PIPE)
    {
        if (r->flags & ~(LUME_FD_CLOEXEC | LUME_FD_NONBLOCK) || r->other)
            return -LUME_EINVAL;
        unsigned first = 0, second, channel = 0;
        while (first < f->identity.limit[LUME_LIMIT_FILES].soft && f->fd[first].object >= 0)
            ++first;
        second = first + 1;
        while (second < f->identity.limit[LUME_LIMIT_FILES].soft && f->fd[second].object >= 0)
            ++second;
        if (second >= f->identity.limit[LUME_LIMIT_FILES].soft)
            return -LUME_EMFILE;
        while (channel < PIPES && (channels[channel].readers || channels[channel].writers))
            ++channel;
        if (channel == PIPES)
            return -LUME_ENOMEM;
#if LUME_FILES_DYNAMIC
        channels[channel].data = (void *)(uintptr_t)lume_page_alloc(0);
        if (!channels[channel].data)
            return -LUME_ENOMEM;
#endif
        lume_poll_source_reset(&channels[channel].changed);
        int a = new_object(-1, (int)channel, LUME_O_RDONLY);
        if (a < 0)
        {
#if LUME_FILES_DYNAMIC
            if (!lume_page_free((uintptr_t)channels[channel].data))
                lume_fail("pipe backing rollback");
            channels[channel].data = NULL;
#endif
            return a;
        }
        bind_descriptor(f, first, a, r->flags & LUME_FD_CLOEXEC);
        int b = new_object(-1, (int)channel, LUME_O_WRONLY);
        if (b < 0)
        {
            (void)lume_vfs_close(f, first);
            return b;
        }
        bind_descriptor(f, second, b, r->flags & LUME_FD_CLOEXEC);
        open_files[a].status = open_files[b].status = r->flags & LUME_FD_NONBLOCK;
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
            while (target < f->identity.limit[LUME_LIMIT_FILES].soft && f->fd[target].object >= 0)
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

int64_t lume_vfs_mkdir(struct lume_files *f, const char *input)
{
    char p[LUME_FILES_PATH_MAX] = {0};
    int r = resolve(f, input, p);
    if (r >= 0)
        return -LUME_EEXIST;
    if (r != -LUME_ENOENT || !p[0])
        return r;
    int permission = sandbox_mutation(f, p, true);
    if (permission)
        return permission;
    char parent[LUME_FILES_PATH_MAX];
    copy(parent, p, length(p) + 1);
    size_t k = length(parent);
    while (k && parent[k - 1] != '/')
        --k;
    parent[k > 1 ? k - 1 : 1] = 0;
    r = find(parent);
    if (r < 0)
        return r;
    if (!nodes[r].writable)
        return -LUME_EROFS;
    if (available_node() == node_capacity)
        return -LUME_ENOSPC;
    char *owned = lume_path_duplicate(p);
    if (!owned)
        return -LUME_ENOMEM;
    unsigned reserved = reserve_node(owned, LUME_NODE_DIR);
    if (reserved == UINT32_MAX)
    {
        lume_path_release(owned);
        return -LUME_ENOSPC;
    }
    if (nodes[r].persistent)
    {
        int e = storage->create(p, true);
        if (e)
        {
            cancel_node(reserved);
            return e;
        }
    }
    nodes[reserved].persistent = nodes[r].persistent;
    publish_node(reserved);
    return 0;
}

int64_t lume_vfs_remove(struct lume_files *files, const char *input, bool directory)
{
    char path[LUME_FILES_PATH_MAX];
    int index = resolve(files, input, path);
    if (index < 0)
        return index;
    int permission = sandbox_mutation(files, path, true);
    if (permission)
        return permission;
    struct node *node = &nodes[index];
    if (!node->writable || node->block || equal(path, "/tmp") || equal(path, "/data"))
        return -LUME_EROFS;
    if (lume_file_mode_namespace_busy(path))
        return -LUME_EBUSY;
    if (directory && node->kind != LUME_NODE_DIR)
        return -LUME_ENOTDIR;
    if (!directory && node->kind == LUME_NODE_DIR)
        return -LUME_EISDIR;
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
            if (node->references && node->kind == LUME_NODE_FILE)
                return -LUME_EBUSY;
            result = storage->remove(path);
            if (!result)
                state = LUME_STORAGE_NAMESPACE_COMMITTED;
        }
    }
    if (state != LUME_STORAGE_NAMESPACE_COMMITTED)
        return result ? result : -LUME_EIO;
    node->mode = (unsigned)permissions & 0777u;
    node->mode_set = true;
    index_remove((unsigned)index);
    node->linked = false;
    bool persistent = node->persistent;
    release_ram(node);
    return result ? result : lume_file_mode_remove(path, persistent);
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
            lume_path_release(node->path);
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

static bool import_storage(const char *path, bool directory, uint64_t bytes, void *unused)
{
    (void)unused;
    if (count == node_capacity || find(path) >= 0 || length(path) >= LUME_FILES_PATH_MAX)
        return false;
    unsigned index = count;
    if (!add(path, directory ? LUME_NODE_DIR : LUME_NODE_FILE, 0, (size_t)bytes, true))
        return false;
    nodes[index].persistent = true;
    return true;
}

bool lume_vfs_mount_storage(const struct lume_storage_ops *ops)
{
    if (!ops || storage)
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
    bool success;
    if (mount >= 0)
    {
        nodes[mount].persistent = true;
        nodes[mount].writable = true;
        nodes[mount].on_fat = false;
        nodes[mount].mode_set = false;
        index_insert((unsigned)mount);
        success = true;
    }
    else
    {
        success = add("/data", LUME_NODE_DIR, 0, 0, true);
        if (success)
            nodes[before].persistent = true;
    }
    if (success)
        success = ops->walk(import_storage, 0);
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
    }
    if (success && storage->program_cache_owner)
        storage->program_cache_owner(program_cache_clear);
    return success;
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

int lume_vfs_program(struct lume_files *f, const char *input, const void **data, size_t *bytes)
{
    char p[LUME_FILES_PATH_MAX];
    int i = resolve(f, input, p);
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
    i = resolve(f, LUME_BUSYBOX_ALIAS_TARGET, p);
    return i < 0 ? i : program_image((unsigned)i, data, bytes);
}

/* The caller owns a held open-file reference for the complete positioned read. */
int64_t lume_vfs_pread_object(int object, uint64_t offset, void *data, uint64_t bytes)
{
    if (object < 0 || object >= OPEN_FILES || !open_files[object].references)
        return -LUME_EBADF;
    struct open_file *file = &open_files[object];
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
        return lume_fat_read_cursor(&root_fat, node->fat_file, offset, data, bytes, &file->fat_cursor)
                   ? -LUME_EIO
                   : (int64_t)bytes;
    copy(data, (const unsigned char *)(node->image ? node->image : node->data) + offset, bytes);
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

int lume_vfs_canonical_at(struct lume_files *files, int64_t directory, const char *input,
                          char output[LUME_FILES_PATH_MAX])
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
    int result = resolve(files, input, output);
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
    return lume_sandbox_path(files, output);
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
