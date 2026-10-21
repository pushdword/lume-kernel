#include "lume/vfs.h"
#include "alias.h"
#define NODES 192u
#define CAPACITY 4096u
#define RAM_FILES 32u
static unsigned char ram_data[RAM_FILES][CAPACITY];
static bool ram_used[RAM_FILES];

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
} default_nodes[NODES];
static struct node *nodes = default_nodes;
static unsigned node_capacity = NODES;

static unsigned count;
static struct lume_fat root_fat;
static const struct lume_storage_ops *storage;
static unsigned char default_program_buffer[262144];
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

void lume_vfs_init(const void *shell, size_t bytes)
{
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
}

void lume_files_init(struct lume_files *f)
{
    *f = (struct lume_files){0};
    f->cwd[0] = '/';
    for (unsigned i = 0; i < LUME_FD_MAX; ++i)
        f->fd[i].node = -1;
    for (unsigned i = 0; i < 3; ++i)
        f->fd[i] = (struct lume_fd){5, i ? LUME_O_WRONLY : LUME_O_RDONLY, 0};
    nodes[5].references += 3;
}

void lume_files_clone(struct lume_files *out, const struct lume_files *in)
{
    *out = *in;
    for (unsigned i = 0; i < LUME_FD_MAX; ++i)
        if (out->fd[i].node >= 0)
            ++nodes[out->fd[i].node].references;
}

void lume_files_release(struct lume_files *f)
{
    for (unsigned i = 0; i < LUME_FD_MAX; ++i)
        if (f->fd[i].node >= 0)
        {
            if (nodes[f->fd[i].node].references)
                --nodes[f->fd[i].node].references;
            release_ram(&nodes[f->fd[i].node]);
            f->fd[i].node = -1;
        }
}

static struct lume_fd *descriptor(struct lume_files *f, uint64_t fd)
{
    return fd < LUME_FD_MAX && f->fd[fd].node >= 0 ? &f->fd[fd] : 0;
}

int64_t lume_vfs_open(struct lume_files *f, const char *input, uint64_t flags)
{
    const uint64_t allowed = 3 | LUME_O_CREAT | LUME_O_TRUNC | LUME_O_APPEND | LUME_O_EXCL;
    if ((flags & ~allowed) || (flags & 3) == 3 ||
        (!(flags & 3) && (flags & (LUME_O_TRUNC | LUME_O_APPEND))))
        return -LUME_EINVAL;
    unsigned fd = 0;
    while (fd < LUME_FD_MAX && f->fd[fd].node >= 0)
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
    if ((flags & 3) && n->kind == LUME_NODE_DIR)
        return -LUME_EISDIR;
    if ((flags & 3) && !n->writable)
        return -LUME_EROFS;
    if (flags & LUME_O_TRUNC)
    {
        if (n->persistent)
        {
            int e = storage->truncate(p);
            if (e)
                return e;
        }
        n->bytes = 0;
    }
    ++n->references;
    f->fd[fd] = (struct lume_fd){index, (uint32_t)flags, 0};
    return fd;
}

int64_t lume_vfs_close(struct lume_files *f, uint64_t fd)
{
    struct lume_fd *d = descriptor(f, fd);
    if (!d)
        return -LUME_EBADF;
    if (nodes[d->node].references)
        --nodes[d->node].references;
    release_ram(&nodes[d->node]);
    d->node = -1;
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

int64_t lume_vfs_read(struct lume_files *f, uint64_t fd, void *data, uint64_t bytes)
{
    struct lume_fd *d = descriptor(f, fd);
    if (!d || (d->flags & 3) == LUME_O_WRONLY)
        return -LUME_EBADF;
    struct node *n = &nodes[d->node];
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
    struct lume_fd *d = descriptor(f, fd);
    if (!d || !(d->flags & 3))
        return -LUME_EBADF;
    struct node *n = &nodes[d->node];
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

int64_t lume_vfs_seek(struct lume_files *f, uint64_t fd, int64_t offset, uint64_t whence)
{
    struct lume_fd *d = descriptor(f, fd);
    if (!d)
        return -LUME_EBADF;
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
    struct lume_fd *d = descriptor(f, fd);
    if (!d)
        return -LUME_EBADF;
    struct node *n = &nodes[d->node];
    *s = (struct lume_stat){(uint64_t)d->node + 1, n->bytes, n->kind, 0};
    return 0;
}

int64_t lume_vfs_getdents(struct lume_files *f, uint64_t fd, void *out, uint64_t bytes)
{
    struct lume_fd *d = descriptor(f, fd);
    if (!d)
        return -LUME_EBADF;
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
    return (n->kind == LUME_NODE_DIR    ? 0040555
            : n->kind == LUME_NODE_CHAR ? 0020444
                                        : 0100555) |
           (n->writable ? 0222 : 0);
}

int64_t lume_vfs_stat_mode(struct lume_files *f, uint64_t fd, struct lume_stat *out)
{
    struct lume_fd *d = descriptor(f, fd);
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
    struct lume_fd *d = descriptor(f, fd);
    if (!d || !(d->flags & 3))
        return -LUME_EBADF;
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
    struct lume_fd *d = descriptor(f, from);
    if (!d || to >= LUME_FD_MAX)
        return -LUME_EBADF;
    if (from == to)
        return (int64_t)to;
    if (f->fd[to].node >= 0 && nodes[f->fd[to].node].references)
    {
        --nodes[f->fd[to].node].references;
        release_ram(&nodes[f->fd[to].node]);
    }
    f->fd[to] = *d;
    ++nodes[d->node].references;
    return (int64_t)to;
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
    if (!n->writable || equal(p, "/tmp") || equal(p, "/data"))
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
