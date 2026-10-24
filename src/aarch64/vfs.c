#include "lume/vfs.h"
#include "alias.h"
#include "lume/wait.h"
#include "lume/devfs.h"
#define NODES 192u
#define CAPACITY 4096u
#define RAM_FILES 32u
static unsigned char ram_data[RAM_FILES][CAPACITY];
static bool ram_used[RAM_FILES];

/* Duplicated and inherited descriptors share one offset and access mode. Pipe endpoints
 * keep their channel alive until the last reference to each open description closes. */
#define OPEN_FILES 256u
#define PIPES 8u

static struct open_file
{
    int node, pipe;
    unsigned references, status;
    uint32_t flags;
    uint64_t offset;
} open_files[OPEN_FILES];

static struct channel
{
    unsigned readers, writers, head, count;
    struct lume_wait_queue readable, writable;
    unsigned char data[LUME_PIPE_BYTES];
} channels[PIPES];

static struct node
{
    char path[LUME_PATH_MAX];
    uint32_t kind;
    bool writable, linked, on_fat, persistent;
    unsigned references;
    struct lume_fat_file fat_file;
    const unsigned char *image;
    uint64_t bytes;
    unsigned char *data;
    struct lume_block *block;
} default_nodes[NODES];
static struct node *nodes = default_nodes;
static unsigned node_capacity = NODES;

static unsigned count;
static struct lume_block *block_devices[LUME_DEVICE_LIMIT];
static struct lume_fat root_fat;
static const struct lume_storage_ops *storage;
/* Boot installs dynamically allocated staging before loading programs. */
static unsigned char default_program_buffer[65536];
static void *program_buffer = default_program_buffer;
static size_t program_capacity = sizeof(default_program_buffer);
static void *(*program_allocate)(size_t);

bool lume_vfs_node_buffer(void *buffer, size_t bytes)
{
    if (!buffer || count || (uintptr_t)buffer % _Alignof(struct node) ||
        bytes / sizeof(struct node) < NODES || bytes / sizeof(struct node) > UINT32_MAX)
        return false;
    nodes = buffer;
    node_capacity = (unsigned)(bytes / sizeof(struct node));
    return true;
}

void lume_vfs_program_allocator(void *(*allocate)(size_t))
{
    program_allocate = allocate;
}

void lume_vfs_program_buffer(void *buffer, size_t bytes)
{
    program_buffer = buffer;
    program_capacity = bytes;
}

static void release_ram(struct node *n)
{
    if (n->linked || n->references || !n->data)
        return;
    for (unsigned i = 0; i < RAM_FILES; ++i)
        if (n->data == ram_data[i])
            ram_used[i] = false;
    n->data = 0;
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

static int path(struct lume_files *f, const char *input, char out[LUME_PATH_MAX])
{
    if (!*input)
        return -LUME_ENOENT;
    size_t n = 1;
    out[0] = '/';
    out[1] = 0;
    if (*input != '/')
    {
        n = length(f->cwd);
        copy(out, f->cwd, n + 1);
    }
    while (*input)
    {
        while (*input == '/')
            ++input;
        const char *start = input;
        size_t part = 0;
        while (*input && *input != '/')
        {
            ++input;
            ++part;
        }
        if (!part || (part == 1 && start[0] == '.'))
            continue;
        if (part == 2 && start[0] == '.' && start[1] == '.')
        {
            while (n > 1 && out[n - 1] != '/')
                --n;
            if (n > 1)
                --n;
            out[n] = 0;
            continue;
        }
        if (part > LUME_NAME_MAX || n + part + (n > 1) >= LUME_PATH_MAX)
            return -LUME_ENAMETOOLONG;
        if (n > 1)
            out[n++] = '/';
        copy(out + n, start, part);
        n += part;
        out[n] = 0;
    }
    if (n >= 5 && out[1] == 'd' && out[2] == 'a' && out[3] == 't' && out[4] == 'a' &&
        (n == 5 || out[5] == '/'))
        for (size_t i = 6; i < n; ++i)
            if (out[i] >= 'A' && out[i] <= 'Z')
                out[i] = (char)(out[i] + 32);
    return 0;
}

static int find(const char *p)
{
    for (unsigned i = 0; i < count; ++i)
        if (nodes[i].linked && equal(p, nodes[i].path))
            return (int)i;
    return -LUME_ENOENT;
}

/* Verify every prefix: /regular/../tmp must still fail with ENOTDIR. */
static int resolve(struct lume_files *f, const char *input, char out[LUME_PATH_MAX])
{
    char raw[LUME_PATH_MAX * 2];
    size_t n = 0;
    if (*input != '/')
    {
        n = length(f->cwd);
        copy(raw, f->cwd, n);
        raw[n++] = '/';
    }
    size_t m = length(input);
    if (n + m >= sizeof(raw))
        return -LUME_ENAMETOOLONG;
    copy(raw + n, input, m + 1);
    for (size_t i = 1; i < n + m; ++i)
        if (raw[i] == '/' && raw[i - 1] != '/')
        {
            char prefix[LUME_PATH_MAX];
            char saved = raw[i];
            raw[i] = 0;
            int r = path(f, raw, prefix);
            raw[i] = saved;
            if (r < 0)
                return r;
            r = find(prefix);
            if (r < 0)
                return r;
            if (nodes[r].kind != LUME_NODE_DIR)
                return -LUME_ENOTDIR;
        }
    int r = path(f, input, out);
    if (r < 0)
        return r;
    r = find(out);
    if (r >= 0 && m && input[m - 1] == '/' && nodes[r].kind != LUME_NODE_DIR)
        return -LUME_ENOTDIR;
    return r;
}

static void add(const char *p, unsigned kind, const void *data, size_t bytes, bool writable)
{
    struct node *n = &nodes[count++];
    *n = (struct node){0};
    copy(n->path, p, length(p) + 1);
    n->kind = kind;
    n->image = data;
    n->bytes = bytes;
    n->writable = writable;
    n->linked = true;
}

static void block_path(const struct lume_block *b, char out[LUME_PATH_MAX])
{
    copy(out, "/dev/", 5);
    copy(out + 5, b->device->name, length(b->device->name) + 1);
}

static int add_block(struct lume_block *b)
{
    char p[LUME_PATH_MAX];
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
        add(p, LUME_NODE_BLOCK, 0, (size_t)(b->sectors * b->sector_bytes), !b->read_only);
    }
    else
    {
        nodes[slot] = (struct node){.kind = LUME_NODE_BLOCK,
                                    .writable = !b->read_only,
                                    .linked = true,
                                    .bytes = b->sectors * b->sector_bytes};
        copy(nodes[slot].path, p, length(p) + 1);
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
                    nodes[j].linked = false;
            block_devices[i] = 0;
            return 0;
        }
    return b ? 0 : -LUME_EINVAL;
}

void lume_vfs_init(const void *shell, size_t bytes)
{
    for (unsigned i = 0; i < OPEN_FILES; ++i)
        open_files[i] = (struct open_file){0};
    for (unsigned i = 0; i < PIPES; ++i)
        channels[i] = (struct channel){0};
    count = 0;
    root_fat = (struct lume_fat){0};
    storage = 0;
    for (unsigned i = 0; i < RAM_FILES; ++i)
        ram_used[i] = false;
    add("/", LUME_NODE_DIR, 0, 0, false);
    add("/dev", LUME_NODE_DIR, 0, 0, false);
    add("/bin", LUME_NODE_DIR, 0, 0, false);
    add("/etc", LUME_NODE_DIR, 0, 0, false);
    add("/tmp", LUME_NODE_DIR, 0, 0, true);
    add("/dev/console", LUME_NODE_CHAR, 0, 0, true);
    add("/bin/sh", LUME_NODE_FILE, shell, bytes, false);
    static const char motd[] =
        "LumeOS AArch64 / Raspberry Pi 5\nPublic ABI 1.0; RAM VFS; GPIO UART console.\n";
    add("/etc/motd", LUME_NODE_FILE, motd, sizeof(motd) - 1, false);
    for (unsigned i = 0; i < LUME_DEVICE_LIMIT; ++i)
        if (block_devices[i])
            (void)add_block(block_devices[i]);
}

static int new_object(int node, int pipe, uint32_t flags)
{
    for (unsigned i = 0; i < OPEN_FILES; ++i)
        if (!open_files[i].references)
        {
            open_files[i] =
                (struct open_file){.node = node, .pipe = pipe, .references = 1, .flags = flags};
            if (node >= 0)
                ++nodes[node].references;
            else if (flags & 3)
                ++channels[pipe].writers;
            else
                ++channels[pipe].readers;
            return (int)i;
        }
    return -LUME_ENOMEM;
}

void lume_files_init(struct lume_files *f)
{
    *f = (struct lume_files){0};
    f->cwd[0] = '/';
    for (unsigned i = 0; i < LUME_FD_MAX; ++i)
        f->fd[i].object = -1;
    for (unsigned i = 0; i < 3; ++i)
        f->fd[i].object = new_object(5, -1, i ? LUME_O_WRONLY : LUME_O_RDONLY);
}

void lume_files_clone(struct lume_files *out, const struct lume_files *in)
{
    *out = *in;
    for (unsigned i = 0; i < LUME_FD_MAX; ++i)
        if (out->fd[i].object >= 0)
            ++open_files[out->fd[i].object].references;
}

void lume_files_inherit(struct lume_files *out, const struct lume_files *in)
{
    lume_files_clone(out, in);
    for (unsigned i = 0; i < LUME_FD_MAX; ++i)
        if (out->fd[i].flags & LUME_FD_CLOEXEC)
            (void)lume_vfs_close(out, i);
}

void lume_files_release(struct lume_files *f)
{
    for (unsigned i = 0; i < LUME_FD_MAX; ++i)
        (void)lume_vfs_close(f, i);
}

static struct open_file *descriptor(struct lume_files *f, uint64_t fd)
{
    return fd < LUME_FD_MAX && f->fd[fd].object >= 0 ? &open_files[f->fd[fd].object] : 0;
}

int64_t lume_vfs_open(struct lume_files *f, const char *input, uint64_t flags)
{
    const uint64_t allowed = 3 | LUME_O_CREAT | LUME_O_TRUNC | LUME_O_APPEND | LUME_O_EXCL;
    if ((flags & ~allowed) || (flags & 3) == 3 ||
        (!(flags & 3) && (flags & (LUME_O_TRUNC | LUME_O_APPEND))))
        return -LUME_EINVAL;
    unsigned fd = 0;
    while (fd < LUME_FD_MAX && f->fd[fd].object >= 0)
        ++fd;
    if (fd == LUME_FD_MAX)
        return -LUME_EMFILE;
    char p[LUME_PATH_MAX] = {0};
    int index = resolve(f, input, p);
    if (index >= 0 && (flags & LUME_O_CREAT) && (flags & LUME_O_EXCL))
        return -LUME_EEXIST;
    if (index == -LUME_ENOENT && (flags & LUME_O_CREAT) && p[0])
    {
        if (input[length(input) - 1] == '/')
            return -LUME_ENOENT;
        char parent[LUME_PATH_MAX];
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
        if (count == node_capacity)
            return -LUME_ENOSPC;
        if (nodes[directory].persistent)
        {
            int e = storage->create(p, false);
            if (e)
                return e;
        }
        index = (int)count;
        add(p, LUME_NODE_FILE, 0, 0, true);
        nodes[index].persistent = nodes[directory].persistent;
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
    f->fd[fd] = (struct lume_fd){object, 0};
    return fd;
}

int64_t lume_vfs_close(struct lume_files *f, uint64_t fd)
{
    struct open_file *d = descriptor(f, fd);
    if (!d)
        return -LUME_EBADF;
    if (!--d->references)
    {
        if (d->pipe >= 0)
        {
            struct channel *c = &channels[d->pipe];
            if (d->flags & 3)
                --c->writers;
            else
                --c->readers;
            lume_wait_wake(&c->readable);
            lume_wait_wake(&c->writable);
            if (!c->readers && !c->writers)
                *c = (struct channel){0};
        }
        else
        {
            --nodes[d->node].references;
            release_ram(&nodes[d->node]);
        }
    }
    f->fd[fd] = (struct lume_fd){-1, 0};
    return 0;
}

int64_t lume_vfs_open_compat(struct lume_files *f, const char *input, uint64_t flags)
{
    char p[LUME_PATH_MAX];
    int index = resolve(f, input, p);
    if (index >= 0 && (flags & 3) && nodes[index].kind == LUME_NODE_FILE && nodes[index].references)
        return -LUME_EBUSY;
    return lume_vfs_open(f, input, flags);
}

int64_t lume_vfs_remove_compat(struct lume_files *f, const char *input, bool directory)
{
    char p[LUME_PATH_MAX];
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

int64_t lume_vfs_read(struct lume_files *f, uint64_t fd, void *data, uint64_t bytes)
{
    struct open_file *d = descriptor(f, fd);
    if (!d || (d->flags & 3) == LUME_O_WRONLY)
        return -LUME_EBADF;
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
        int r = storage->read(n->path, d->offset, data, (size_t)bytes);
        if (r < 0)
            return r;
        bytes = (uint64_t)r;
    }
    else if (bytes && n->on_fat)
    {
        if (lume_fat_read(&root_fat, n->fat_file, d->offset, data, (size_t)bytes))
            return -LUME_EIO;
    }
    else if (bytes)
        copy(data, (n->image ? n->image : n->data) + d->offset, (size_t)bytes);
    d->offset += bytes;
    return (int64_t)bytes;
}

int64_t lume_vfs_write(struct lume_files *f, uint64_t fd, const void *data, uint64_t bytes)
{
    struct open_file *d = descriptor(f, fd);
    if (!d || !(d->flags & 3))
        return -LUME_EBADF;
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
        return (int64_t)bytes;
    }
    struct node *n = &nodes[d->node];
    if (n->block)
        return block_io(d, n, (void *)data, bytes, true);
    if (n->kind == LUME_NODE_CHAR)
    {
        if (bytes)
            lume_console_write(data, (size_t)bytes);
        return (int64_t)bytes;
    }
    if (!n->writable)
        return -LUME_EROFS;
    if (!bytes)
        return 0;
    if (d->flags & LUME_O_APPEND)
        d->offset = n->bytes;
    if (n->persistent)
    {
        int r = storage->write(n->path, d->offset, data, (size_t)bytes);
        if (r < 0)
            return r;
        d->offset += (uint64_t)r;
        if (d->offset > n->bytes)
            n->bytes = d->offset;
        return r;
    }
    if (d->offset > CAPACITY || bytes > CAPACITY - d->offset)
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
    while (n->bytes < d->offset)
        n->data[n->bytes++] = 0;
    copy(n->data + d->offset, data, (size_t)bytes);
    d->offset += bytes;
    if (d->offset > n->bytes)
        n->bytes = d->offset;
    return (int64_t)bytes;
}

int64_t lume_vfs_sync(struct lume_files *f, uint64_t fd)
{
    struct open_file *d = descriptor(f, fd);
    if (!d)
        return -LUME_EBADF;
    if (d->pipe >= 0)
        return -LUME_EINVAL;
    struct node *n = &nodes[d->node];
    return n->block ? lume_block_sync(n->block) : 0;
}

int64_t lume_vfs_seek(struct lume_files *f, uint64_t fd, int64_t offset, uint64_t whence)
{
    struct open_file *d = descriptor(f, fd);
    if (!d)
        return -LUME_EBADF;
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
    if (d->pipe >= 0)
    {
        *s = (struct lume_stat){UINT64_C(0x100000) + (unsigned)d->pipe, 0, LUME_NODE_PIPE, 0};
        return 0;
    }
    struct node *n = &nodes[d->node];
    *s = (struct lume_stat){(uint64_t)d->node + 1, n->bytes, n->kind, 0};
    return 0;
}

int64_t lume_vfs_getdents(struct lume_files *f, uint64_t fd, void *out, uint64_t bytes)
{
    struct open_file *d = descriptor(f, fd);
    if (!d)
        return -LUME_EBADF;
    if (d->pipe >= 0)
        return -LUME_ENOTDIR;
    struct node *n = &nodes[d->node];
    if (n->kind != LUME_NODE_DIR)
        return -LUME_ENOTDIR;
    if (bytes < sizeof(struct lume_dirent))
        return -LUME_EINVAL;
    size_t prefix = length(n->path);
    uint64_t used = 0;
    while (d->offset < count && bytes - used >= sizeof(struct lume_dirent))
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
        struct lume_dirent entry = {0};
        entry.inode = i + 1;
        entry.kind = nodes[i].kind;
        copy(entry.name, name, length(name) + 1);
        copy((unsigned char *)out + used, &entry, sizeof(entry));
        used += sizeof(entry);
    }
    return (int64_t)used;
}

static int64_t stat_node(unsigned index, struct lume_stat *out)
{
    const struct node *n = &nodes[index];
    *out = (struct lume_stat){index + 1, n->bytes, n->kind, 0};
    return (n->kind == LUME_NODE_DIR     ? 0040555
            : n->kind == LUME_NODE_CHAR  ? 0020444
            : n->kind == LUME_NODE_BLOCK ? 0060444
                                         : 0100555) |
           (n->writable ? 0222 : 0);
}

int64_t lume_vfs_stat_mode(struct lume_files *f, uint64_t fd, struct lume_stat *out)
{
    struct open_file *d = descriptor(f, fd);
    if (d && d->pipe >= 0)
    {
        (void)lume_vfs_stat(f, fd, out);
        return 0010600;
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
    char p[LUME_PATH_MAX];
    int i = resolve(f, input, p);
    return i < 0 ? i : stat_node((unsigned)i, out);
}

int64_t lume_vfs_truncate(struct lume_files *f, uint64_t fd)
{
    struct open_file *d = descriptor(f, fd);
    if (!d || !(d->flags & 3))
        return -LUME_EBADF;
    if (d->pipe >= 0)
        return -LUME_EINVAL;
    struct node *n = &nodes[d->node];
    if (n->kind != LUME_NODE_FILE)
        return -LUME_EINVAL;
    if (d->offset > n->bytes)
        return -LUME_EINVAL;
    if (n->persistent)
    {
        if (!storage->resize)
            return -LUME_ENOSYS;
        int error = storage->resize(n->path, d->offset);
        if (error)
            return error;
    }
    n->bytes = d->offset;
    return 0;
}

int64_t lume_vfs_rename(struct lume_files *f, const char *from, const char *to)
{
    char a[LUME_PATH_MAX], b[LUME_PATH_MAX] = {0}, parent[LUME_PATH_MAX];
    int source = resolve(f, from, a), destination = resolve(f, to, b);
    if (source < 0)
        return source;
    if (destination == source)
        return 0;
    if (destination >= 0)
        return -LUME_EEXIST;
    if (destination != -LUME_ENOENT || !b[0])
        return destination;
    copy(parent, b, length(b) + 1);
    size_t n = length(parent);
    while (n && parent[n - 1] != '/')
        --n;
    parent[n > 1 ? n - 1 : 1] = 0;
    int directory = find(parent);
    if (directory < 0)
        return directory;
    struct node *file = &nodes[source];
    if (file->kind != LUME_NODE_FILE)
        return -LUME_EISDIR;
    if (!file->writable || !nodes[directory].writable)
        return -LUME_EROFS;
    if (file->references)
        return -LUME_EBUSY;
    if (file->persistent != nodes[directory].persistent)
        return -18;
    if (file->persistent)
    {
        if (!storage->rename)
            return -LUME_ENOSYS;
        int error = storage->rename(a, b);
        if (error)
            return error;
    }
    copy(file->path, b, length(b) + 1);
    return 0;
}

int64_t lume_vfs_chdir(struct lume_files *f, const char *input)
{
    char p[LUME_PATH_MAX];
    int i = resolve(f, input, p);
    if (i < 0)
        return i;
    if (nodes[i].kind != LUME_NODE_DIR)
        return -LUME_ENOTDIR;
    copy(f->cwd, p, length(p) + 1);
    return 0;
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
    (void)lume_vfs_close(f, to);
    f->fd[to] = (struct lume_fd){f->fd[from].object, 0};
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
    if (!d || d->pipe < 0 || (d->status & LUME_FD_NONBLOCK))
        return 0;
    return writing ? &channels[d->pipe].writable : &channels[d->pipe].readable;
}

static unsigned events(struct lume_files *f, int fd)
{
    if (fd < 0)
        return 0;
    struct open_file *d = descriptor(f, (unsigned)fd);
    if (!d)
        return LUME_POLLNVAL;
    if (d->pipe < 0)
    {
        if (nodes[d->node].block && nodes[d->node].block->device->state != LUME_DEVICE_READY)
            return LUME_POLLERR;
        return ((d->flags & 3) != LUME_O_WRONLY &&
                        (nodes[d->node].kind != LUME_NODE_CHAR || lume_console_ready())
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

int64_t lume_vfs_fd_call(struct lume_files *f, unsigned op, struct lume_fd_request *r)
{
    if (r->version != LUME_FD_VERSION || r->bytes != sizeof(*r))
        return -LUME_EINVAL;
    struct open_file *d = descriptor(f, (unsigned)r->fd);
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
            p[i].revents = (uint16_t)(events(f, p[i].fd) &
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
        while (first < LUME_FD_MAX && f->fd[first].object >= 0)
            ++first;
        second = first + 1;
        while (second < LUME_FD_MAX && f->fd[second].object >= 0)
            ++second;
        if (second >= LUME_FD_MAX)
            return -LUME_EMFILE;
        while (channel < PIPES && (channels[channel].readers || channels[channel].writers))
            ++channel;
        if (channel == PIPES)
            return -LUME_ENOMEM;
        int a = new_object(-1, (int)channel, LUME_O_RDONLY);
        if (a < 0)
            return a;
        f->fd[first] = (struct lume_fd){a, r->flags & LUME_FD_CLOEXEC};
        int b = new_object(-1, (int)channel, LUME_O_WRONLY);
        if (b < 0)
        {
            (void)lume_vfs_close(f, first);
            return b;
        }
        f->fd[second] = (struct lume_fd){b, r->flags & LUME_FD_CLOEXEC};
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
            while (target < LUME_FD_MAX && f->fd[target].object >= 0)
                ++target;
            if (target == LUME_FD_MAX)
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
    char p[LUME_PATH_MAX] = {0};
    int r = resolve(f, input, p);
    if (r >= 0)
        return -LUME_EEXIST;
    if (r != -LUME_ENOENT || !p[0])
        return r;
    char parent[LUME_PATH_MAX];
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
    if (count == node_capacity)
        return -LUME_ENOSPC;
    if (nodes[r].persistent)
    {
        int e = storage->create(p, true);
        if (e)
            return e;
    }
    unsigned i = count;
    add(p, LUME_NODE_DIR, 0, 0, true);
    nodes[i].persistent = nodes[r].persistent;
    return 0;
}

int64_t lume_vfs_remove(struct lume_files *f, const char *input, bool directory)
{
    char p[LUME_PATH_MAX];
    int i = resolve(f, input, p);
    if (i < 0)
        return i;
    struct node *n = &nodes[i];
    if (!n->writable || n->block || equal(p, "/tmp") || equal(p, "/data"))
        return -LUME_EROFS;
    if (directory && n->kind != LUME_NODE_DIR)
        return -LUME_ENOTDIR;
    if (!directory && n->kind == LUME_NODE_DIR)
        return -LUME_EISDIR;
    size_t len = length(p);
    if (directory)
        for (unsigned k = 0; k < count; ++k)
        {
            if (!nodes[k].linked)
                continue;
            size_t j = 0;
            while (j < len && nodes[k].path[j] == p[j])
                ++j;
            if (j == len && nodes[k].path[j] == '/')
                return -LUME_ENOTEMPTY;
        }
    if (n->persistent)
    {
        if (n->references)
            return -LUME_EBUSY;
        int e = storage->remove(p);
        if (e)
            return e;
    }
    n->linked = false;
    release_ram(n);
    return 0; /* RAM open descriptors retain the unlinked node. */
}

static bool import_fat(const char *path, struct lume_fat_file file, void *unused)
{
    (void)unused;
    char p[LUME_PATH_MAX];
    size_t n = length(path);
    if (n + 1 >= sizeof(p))
        return false;
    p[0] = '/';
    copy(p + 1, path, n + 1);
    int i = find(p);
    if (i >= 0)
        return file.directory && nodes[i].kind == LUME_NODE_DIR;
    if (count == node_capacity)
        return false;
    i = (int)count;
    add(p, file.directory ? LUME_NODE_DIR : LUME_NODE_FILE, 0, file.size, false);
    nodes[i].on_fat = true;
    nodes[i].fat_file = file;
    return true;
}

bool lume_vfs_mount_fat(const void *data, size_t bytes)
{
    unsigned before = count;
    if (!lume_fat_mount(&root_fat, data, bytes) || !lume_fat_walk(&root_fat, import_fat, 0))
    {
        count = before;
        root_fat = (struct lume_fat){0};
        return false;
    }
    return true;
}

static bool import_storage(const char *path, bool directory, uint64_t bytes, void *unused)
{
    (void)unused;
    if (count == node_capacity || find(path) >= 0)
        return false;
    unsigned i = count;
    add(path, directory ? LUME_NODE_DIR : LUME_NODE_FILE, 0, (size_t)bytes, true);
    nodes[i].persistent = true;
    return true;
}

bool lume_vfs_mount_storage(const struct lume_storage_ops *ops)
{
    if (!ops || storage || count == node_capacity)
        return false;
    unsigned before = count;
    storage = ops;
    add("/data", LUME_NODE_DIR, 0, 0, true);
    nodes[before].persistent = true;
    if (!ops->walk(import_storage, 0))
    {
        count = before;
        storage = 0;
        return false;
    }
    return true;
}

static int program_image(unsigned i, const void **data, size_t *bytes)
{
    struct node *n = &nodes[i];
    if (n->kind != LUME_NODE_FILE)
        return -LUME_ENOEXEC;
    if (n->bytes > program_capacity)
    {
        void *replacement = program_allocate ? program_allocate((size_t)n->bytes) : 0;
        if (!replacement)
            return -LUME_E2BIG;
        program_buffer = replacement;
        program_capacity = (size_t)n->bytes;
    }
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
    char p[LUME_PATH_MAX];
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
