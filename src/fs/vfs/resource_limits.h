#ifndef LUME_VFS_RESOURCE_LIMITS_H
#define LUME_VFS_RESOURCE_LIMITS_H

static int limit_read(unsigned value, char *data, size_t capacity)
{
    char digits[10];
    unsigned count = 0;
    do
    {
        digits[count++] = (char)('0' + value % 10);
        value /= 10;
    } while (value);
    if (capacity < count + 1)
        return -LUME_EINVAL;
    for (unsigned i = 0; i < count; ++i)
        data[i] = digits[count - i - 1];
    data[count] = '\n';
    return (int)count + 1;
}

static int limit_write(const char *data, size_t bytes, unsigned minimum, unsigned maximum,
                       unsigned used, unsigned *target)
{
    if (bytes && data[bytes - 1] == '\n')
        --bytes;
    if (!bytes)
        return -LUME_EINVAL;
    unsigned value = 0;
    for (size_t i = 0; i < bytes; ++i)
    {
        unsigned digit = (unsigned)(unsigned char)data[i] - '0';
        if (digit > 9 || value > (UINT32_MAX - digit) / 10)
            return -LUME_EINVAL;
        value = value * 10 + digit;
    }
    if (value < minimum || value > maximum)
        return -LUME_EINVAL;
    if (value < used)
        return -LUME_EBUSY;
    *target = value;
    return 0;
}

static int file_max_read(void *context, char *data, size_t bytes)
{
    (void)context;
    return limit_read(open_file_limit, data, bytes);
}

static int file_max_write(void *context, const char *data, size_t bytes)
{
    (void)context;
    return limit_write(data, bytes, 3, OPEN_FILE_LIMIT, open_file_used, &open_file_limit);
}

static int file_used_read(void *context, char *data, size_t bytes)
{
    (void)context;
    return limit_read(open_file_used, data, bytes);
}

static int nofile_default_read(void *context, char *data, size_t bytes)
{
    (void)context;
    return limit_read(nofile_default, data, bytes);
}

static int nofile_default_write(void *context, const char *data, size_t bytes)
{
    (void)context;
    return limit_write(data, bytes, 3, LUME_FD_MAX, 0, &nofile_default);
}

static int nofile_max_read(void *context, char *data, size_t bytes)
{
    (void)context;
    return limit_read(LUME_FD_MAX, data, bytes);
}

#if LUME_FILES_DYNAMIC
static int pipe_pages_read(void *context, char *data, size_t bytes)
{
    (void)context;
    return limit_read(pipe_page_limit, data, bytes);
}

static int pipe_pages_write(void *context, const char *data, size_t bytes)
{
    (void)context;
    return limit_write(data, bytes, 1, PIPE_BUDGET_MAX_PAGES, pipe_pages, &pipe_page_limit);
}

static int pipe_used_read(void *context, char *data, size_t bytes)
{
    (void)context;
    return limit_read(pipe_pages, data, bytes);
}
#endif

static const struct lume_devfs_attribute resource_attributes[] = {
    {"file-max", file_max_read, file_max_write},
    {"file-nr", file_used_read, NULL},
    {"nofile-default", nofile_default_read, nofile_default_write},
    {"nofile-max", nofile_max_read, NULL},
#if LUME_FILES_DYNAMIC
    {"pipe-pages-max", pipe_pages_read, pipe_pages_write},
    {"pipe-pages-used", pipe_used_read, NULL},
#endif
};

static struct lume_devfs_group resource_group = {.name = "limits",
                                                 .attributes = resource_attributes,
                                                 .count = sizeof(resource_attributes) /
                                                          sizeof(resource_attributes[0])};

static void resource_limits_start(void)
{
    nofile_default = LUME_FD_MAX;
#if LUME_FILES_DYNAMIC
    pipe_page_limit = PIPE_BUDGET_PAGES;
#endif
}

#endif
