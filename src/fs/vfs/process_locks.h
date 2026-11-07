#ifndef LUME_VFS_PROCESS_LOCKS_H
#define LUME_VFS_PROCESS_LOCKS_H

static void process_locks_drop(unsigned pid, int node)
{
    for (unsigned i = 0; i < file_lock_count;)
        if (file_locks[i].scope == 1 && file_locks[i].owner == (int)pid &&
            (node < 0 || file_locks[i].node == node))
            file_locks[i] = file_locks[--file_lock_count];
        else
            ++i;
}

static int process_lock_node(struct lume_files *files, uint64_t fd)
{
    struct open_file *description = descriptor(files, fd);
    return description && !description->external && description->pipe < 0 ? description->node : -1;
}

int64_t lume_vfs_posix_lock(struct lume_files *files, unsigned pid, bool query,
                            struct lume_file_lock_request *request)
{
    if (!pid || pid > LUME_TASK_LIMIT || request->version != LUME_FILE_LOCK_VERSION ||
        request->bytes != sizeof(*request) || request->reserved || request->reserved2 ||
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
        .owner = (int)pid, .node = description->node, .type = request->type, .scope = 1};
    int result = file_lock_range(request, base, &range);
    return result ? result : file_locks_apply(range, query, request);
}

int64_t lume_vfs_process_close(struct lume_files *files, uint64_t fd, unsigned pid)
{
    int node = process_lock_node(files, fd);
    if (node >= 0)
        process_locks_drop(pid, node);
    return lume_vfs_close(files, fd);
}

int64_t lume_vfs_process_dup2(struct lume_files *files, uint64_t from, uint64_t to, unsigned pid)
{
    int node = from != to ? process_lock_node(files, to) : -1;
    int64_t result = lume_vfs_dup2(files, from, to);
    if (result >= 0 && node >= 0)
        process_locks_drop(pid, node);
    return result;
}

int64_t lume_vfs_process_fd_call(struct lume_files *files, unsigned operation,
                                 struct lume_fd_request *request, unsigned pid)
{
    int node = operation == LUME_FD_DUP2 && request->fd != request->other
                   ? process_lock_node(files, (uint64_t)request->other)
                   : -1;
    int64_t result = lume_vfs_fd_call(files, operation, request);
    if (result >= 0 && node >= 0)
        process_locks_drop(pid, node);
    return result;
}

void lume_vfs_process_exec(struct lume_files *files, unsigned pid)
{
    if (!files_ready(files))
        return;
    for (unsigned fd = 0; fd < LUME_FD_MAX; ++fd)
        if (files->fd[fd].flags & LUME_FD_CLOEXEC)
        {
            int node = process_lock_node(files, fd);
            if (node >= 0)
                process_locks_drop(pid, node);
        }
}

void lume_vfs_process_release(unsigned pid)
{
    process_locks_drop(pid, -1);
}
#endif
