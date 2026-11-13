#ifndef LUME_VFS_OPEN_FILES_H
#define LUME_VFS_OPEN_FILES_H

struct open_file
{
    unsigned index, next_free, next_reap;
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
    unsigned attribute_bytes;
    bool attribute_cached;
    char attribute_data[LUME_DEVFS_ATTRIBUTE_BYTES];
};

#if LUME_FILES_DYNAMIC
#define OPEN_FILE_LIMIT 65536u
#define OPEN_FILE_DEFAULT 16384u
#define OPEN_FILE_GROUP 16u
static struct open_file *open_file_groups[OPEN_FILE_LIMIT / OPEN_FILE_GROUP];
static unsigned open_file_capacity;
#define OPEN_FILES open_file_capacity
_Static_assert(sizeof(struct open_file) * OPEN_FILE_GROUP <= 4000,
               "description groups must fit in one allocation page");
#else
#define OPEN_FILE_LIMIT 256u
#define OPEN_FILE_DEFAULT OPEN_FILE_LIMIT
#define OPEN_FILES OPEN_FILE_LIMIT
static struct open_file open_file_storage[OPEN_FILE_LIMIT];
#endif

static unsigned open_file_free, open_file_live, open_file_used;
static unsigned open_file_limit = OPEN_FILE_DEFAULT;

static struct open_file *open_file_at(unsigned index)
{
#if LUME_FILES_DYNAMIC
    return open_file_groups[index / OPEN_FILE_GROUP] + index % OPEN_FILE_GROUP;
#else
    return open_file_storage + index;
#endif
}

static void open_file_recycle(struct open_file *file)
{
    unsigned index = file->index;
    --open_file_used;
    if (file->references)
        --open_file_live;
    *file = (struct open_file){.index = index, .next_free = open_file_free};
    open_file_free = index + 1;
}

static void open_file_reset(void)
{
#if LUME_FILES_DYNAMIC
    for (unsigned i = 0; i < open_file_capacity / OPEN_FILE_GROUP; ++i)
    {
        lume_kernel_free(open_file_groups[i]);
        open_file_groups[i] = NULL;
    }
    open_file_capacity = 0;
#else
    for (unsigned i = 0; i < OPEN_FILE_LIMIT; ++i)
        open_file_storage[i] = (struct open_file){.index = i, .next_free = i + 2};
    open_file_storage[OPEN_FILE_LIMIT - 1].next_free = 0;
#endif
    open_file_live = open_file_used = 0;
    open_file_limit = OPEN_FILE_DEFAULT;
    open_file_free = LUME_FILES_DYNAMIC ? 0 : 1;
}

/* The caller serializes reservations. Single-page growth cannot suspend and
 * published descriptions keep their addresses until their close callback returns. */
static int open_file_reserve(void)
{
    if (open_file_used >= open_file_limit)
        return -LUME_ENOMEM;
#if LUME_FILES_DYNAMIC
    if (!open_file_free)
    {
        if (open_file_capacity == OPEN_FILE_LIMIT)
            return -LUME_ENOMEM;
        struct open_file *group = lume_kernel_allocate(sizeof(*group) * OPEN_FILE_GROUP);
        if (!group)
            return -LUME_ENOMEM;
        unsigned first = open_file_capacity;
        for (unsigned i = 0; i < OPEN_FILE_GROUP; ++i)
            group[i] = (struct open_file){.index = first + i, .next_free = first + i + 2};
        group[OPEN_FILE_GROUP - 1].next_free = 0;
        open_file_groups[first / OPEN_FILE_GROUP] = group;
        open_file_capacity += OPEN_FILE_GROUP;
        open_file_free = first + 1;
    }
#endif
    if (!open_file_free)
        return -LUME_ENOMEM;
    unsigned index = open_file_free - 1;
    struct open_file *file = open_file_at(index);
    open_file_free = file->next_free;
    *file = (struct open_file){.index = index, .references = 1};
    ++open_file_live;
    ++open_file_used;
    return (int)index;
}

#endif
