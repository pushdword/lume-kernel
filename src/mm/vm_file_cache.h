#define FILE_READ_PAGES 16u
#define FILE_CACHE_FILES 64u

static void file_changed(uint64_t, uint64_t, uint64_t);
static void file_load(void *);

static void backing_attach(struct vm_object *object, struct vm_object *source, uint64_t offset)
{
    ++source->references;
    object->backing = source;
    object->backing_offset = offset;
    object->backing_next = source->children;
    source->children = object;
}

uint64_t lume_vm_cache_pages(void)
{
    return cached_pages;
}

bool lume_vm_storage_required(void)
{
    return file_waiters != 0;
}

static bool file_evict(void)
{
    struct vm_object **oldest = NULL;
    for (struct vm_object **position = &file_cache; *position; position = &(*position)->file_next)
        if ((*position)->references == 1 && !(*position)->loading &&
            (!oldest || (*position)->file_age < (*oldest)->file_age))
            oldest = position;
    if (!oldest)
        return false;
    struct vm_object *object = *oldest;
    *oldest = object->file_next;
    object->file_next = NULL;
    drop(object);
    return true;
}

void lume_vm_cache_reclaim(void)
{
    while (file_evict())
        ;
    while (retired)
    {
        reap(NULL);
        lume_worker_checkpoint(0);
    }
}

static struct vm_object *file_object(int held, int *error)
{
    struct lume_file_identity identity;
    *error = lume_vfs_object_identity(held, &identity);
    if (*error)
        return NULL;
    unsigned count = 0;
    for (struct vm_object *object = file_cache; object; object = object->file_next)
    {
        ++count;
        if (object->file_key == identity.key)
        {
            object->file_age = ++file_age;
            return object;
        }
    }
    if (count >= FILE_CACHE_FILES && file_evict())
        reap(NULL);
    struct vm_object *object = new_object(identity.bytes, true);
    if (!object)
    {
        *error = -LUME_ENOMEM;
        return NULL;
    }
    *error = lume_vfs_object_retain(held);
    if (*error)
    {
        drop(object);
        return NULL;
    }
    *error = lume_vfs_object_identity(held, &identity);
    if (*error)
    {
        lume_vfs_drop(held);
        drop(object);
        return NULL;
    }
    object->bytes = identity.bytes;
    object->cached = true;
    cached_pages += lume_kernel_allocation_pages(object);
    object->file = held + 1;
    object->file_key = identity.key;
    object->file_version = identity.version;
    object->file_age = ++file_age;
    object->io_ready = 1;
    object->file_work =
        (struct lume_work){.run = file_load, .context = object, .domain = LUME_WORK_STORAGE};
    object->file_next = file_cache;
    file_cache = object;
    lume_vfs_mapping_notify(file_changed);
    return object;
}

static void backing_revoke(struct vm_object *source)
{
    struct vm_object *object = source->children;
    if (object)
        ++object->references;
    while (object)
    {
        struct vm_object *next = object->backing_next;
        if (next)
            ++next->references;
        while (object->changing)
            if (!lume_worker_pause_ms(1))
                lume_fail("backing invalidation requires worker context");
        object->changing = true;
        for (struct vm_area *area = object->areas; area; area = area->object_next)
            revoke(area->owner, area, 0, UINT64_MAX);
        if (!object->file_limited)
        {
            uint64_t bytes =
                source->bytes > object->backing_offset ? source->bytes - object->backing_offset : 0;
            for (unsigned bucket = 0; bucket < HASH; ++bucket)
                for (struct vm_chunk *entry = object->chunks[bucket]; entry; entry = entry->next)
                    for (unsigned i = 0; i < 512; ++i)
                    {
                        uint64_t offset = (entry->index * 512 + i) * PAGE;
                        if (!entry->pages[i])
                            continue;
                        if (offset >= rounded(bytes))
                        {
                            if (!lume_page_free(entry->pages[i]))
                                lume_fail("truncated private frame release");
                            entry->pages[i] = 0;
                        }
                        else if (offset <= bytes && bytes - offset < PAGE)
                            memset((void *)(uintptr_t)(entry->pages[i] + bytes - offset), 0,
                                   (size_t)(PAGE - (bytes - offset)));
                    }
        }
        object->changing = false;
        drop(object);
        object = next;
        lume_worker_checkpoint(0);
    }
}

static void file_changed(uint64_t key, uint64_t bytes, uint64_t version)
{
    if (!key)
    {
        lume_vm_cache_reclaim();
        if (file_cache)
            lume_fail("namespace reset with active file mappings");
        return;
    }
    struct vm_object *object = file_cache;
    while (object && object->file_key != key)
        object = object->file_next;
    if (!object)
        return;
    ++object->references;
    while (object->changing)
        if (!lume_worker_pause_ms(1))
            lume_fail("file invalidation requires worker context");
    struct lume_file_identity identity;
    if (!lume_vfs_object_identity(object->file - 1, &identity))
    {
        bytes = identity.bytes;
        version = identity.version;
    }
    object->changing = true;
    object->bytes = bytes;
    object->file_version = version;
    object->io_error = 0;
    backing_revoke(object);
    /* Revocation invalidates retained user-buffer tokens before cache frames are freed. */
    for (unsigned bucket = 0; bucket < HASH; ++bucket)
        while (object->chunks[bucket])
        {
            struct vm_chunk *entry = object->chunks[bucket];
            object->chunks[bucket] = entry->next;
            for (unsigned i = 0; i < 512; ++i)
                if (entry->pages[i])
                {
                    if (!lume_page_free(entry->pages[i]))
                        lume_fail("file cache frame release");
                    --cached_pages;
                    if ((i & 15u) == 15u)
                        lume_worker_checkpoint(0);
                }
            cached_pages -= lume_kernel_allocation_pages(entry);
            lume_kernel_free(entry);
        }
    object->changing = false;
    drop(object);
    lume_task_vm_ready();
}

static int file_fill(struct vm_object *object, uint64_t page)
{
    uint64_t offset = page * PAGE;
    if (object->changing)
        return -LUME_EAGAIN;
    if (offset >= object->bytes)
        return -LUME_EIO;
    uint64_t version = object->file_version;
    uint64_t bytes = object->bytes - offset;
    uint64_t maximum = (FILE_READ_PAGES - page % FILE_READ_PAGES) * PAGE;
    if (bytes > maximum)
        bytes = maximum;
    unsigned pages = (unsigned)((bytes + PAGE - 1) / PAGE);
    uint64_t limit = lume_pages_total() / 16;
    if (limit < 4096)
        limit = 4096;
    if (limit > 65536)
        limit = 65536;
    while (cached_pages + pages > limit && file_evict())
        reap(NULL);
    uint64_t physical = lume_pages_alloc_span_uninitialized(pages);
    if (!physical && file_evict())
    {
        for (unsigned i = 0; retired && i < 8; ++i)
            reap(NULL);
        physical = lume_pages_alloc_span_uninitialized(pages);
    }
    if (!physical)
    {
        pages = 1;
        if (bytes > PAGE)
            bytes = PAGE;
        physical = lume_page_alloc_uninitialized(0);
    }
    if (!physical)
        return -LUME_ENOMEM;
    int64_t read =
        lume_vfs_pread_object(object->file - 1, offset, (void *)(uintptr_t)physical, bytes);
    int error = read < 0 ? (int)read : (uint64_t)read != bytes ? -LUME_EIO : 0;
    if (!error && (version != object->file_version || object->changing))
        error = -LUME_EAGAIN;
    if (!error && bytes < (uint64_t)pages * PAGE)
        memset((void *)(uintptr_t)(physical + bytes), 0, (size_t)((uint64_t)pages * PAGE - bytes));
    if (!error)
    {
        struct vm_chunk *entry = chunk(object, page, true);
        if (!entry)
            error = -LUME_ENOMEM;
        else if (version != object->file_version || object->changing)
            error = -LUME_EAGAIN;
        else
        {
            for (unsigned i = 0; i < pages; ++i)
            {
                unsigned slot = (unsigned)((page + i) % 512);
                if (entry->pages[slot])
                {
                    if (!lume_page_free(physical + (uint64_t)i * PAGE))
                        lume_fail("duplicate file cache frame release");
                }
                else
                {
                    entry->pages[slot] = physical + (uint64_t)i * PAGE;
                    ++cached_pages;
                }
            }
            object->file_age = ++file_age;
            return 0;
        }
    }
    for (unsigned i = 0; i < pages; ++i)
        if (!lume_page_free(physical + (uint64_t)i * PAGE))
            lume_fail("unpublished file cache frame release");
    return error;
}

static void file_load(void *context)
{
    struct vm_object *object = context;
    object->io_running = true;
    int error = file_fill(object, object->io_page);
    object->io_error = error == -LUME_EAGAIN ? 0 : error;
    object->io_error_page = object->io_page & ~(uint64_t)(FILE_READ_PAGES - 1);
    object->loading = object->io_running = false;
    object->io_ready = 1;
    lume_task_vm_ready();
    drop(object);
}

static int file_request(struct vm_object *object, uint64_t page)
{
    uint64_t group = page & ~(uint64_t)(FILE_READ_PAGES - 1);
    if (object->io_error && object->io_error_page == group)
    {
        object->io_error = 0;
        return -1;
    }
    if (lume_task_storage_context())
    {
        if (object->loading && !object->io_running &&
            (!object->file_work.queued || lume_work_cancel(&object->file_work)))
        {
            object->loading = false;
            object->io_ready = 1;
            drop(object);
        }
        if (!object->loading)
        {
            object->loading = object->io_running = true;
            object->io_ready = 0;
            int error = file_fill(object, page);
            object->loading = object->io_running = false;
            object->io_ready = 1;
            lume_task_vm_ready();
            return !error ? 1 : error == -LUME_EAGAIN ? 2 : -1;
        }
    }
    if (!object->loading)
    {
        object->loading = true;
        object->io_ready = 0;
        object->io_page = page;
        ++object->references;
        (void)lume_work_submit(&object->file_work);
    }
    return 2;
}

static int backing_page(struct vm_object *object, uint64_t page, uint64_t *physical)
{
    struct vm_object *source = object->backing;
    if (source->changing)
        return 2;
    uint64_t source_page = object->backing_offset / PAGE + page;
    struct vm_chunk *entry = chunk(source, source_page, false);
    *physical = entry ? entry->pages[source_page % 512] : 0;
    if (*physical)
        return 1;
    if (!source->cached)
    {
        struct vm_chunk *prepared = entry ? NULL : allocate(sizeof(*prepared));
        if (source->changing || source_page >= (source->bytes + PAGE - 1) / PAGE)
        {
            lume_kernel_free(prepared);
            return source->changing ? 2 : -1;
        }
        entry = chunk(source, source_page, false);
        if (!entry && prepared)
        {
            unsigned bucket = (unsigned)((source_page / 512) % HASH);
            prepared->index = source_page / 512;
            prepared->next = source->chunks[bucket];
            source->chunks[bucket] = prepared;
            entry = prepared;
            prepared = NULL;
        }
        lume_kernel_free(prepared);
        if (!entry)
            return -1;
        unsigned index = (unsigned)(source_page % 512);
        if (!entry->pages[index])
            entry->pages[index] = lume_page_alloc(0);
        *physical = entry->pages[index];
        return *physical ? 1 : -1;
    }
    int result = file_request(source, source_page);
    if (result == 1)
    {
        entry = chunk(source, source_page, false);
        *physical = entry ? entry->pages[source_page % 512] : 0;
        return *physical ? 1 : -1;
    }
    return result;
}
