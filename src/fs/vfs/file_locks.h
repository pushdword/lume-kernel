#ifndef LUME_VFS_FILE_LOCKS_H
#define LUME_VFS_FILE_LOCKS_H
#include "lume/file_lock.h"

#define FILE_LOCK_LIMIT LUME_FILE_LOCK_LIMIT

struct file_lock_record
{
    uint64_t first, last;
    int owner, node;
    unsigned type, scope;
};

static struct file_lock_record file_locks[FILE_LOCK_LIMIT];
static struct file_lock_record file_lock_stage[FILE_LOCK_LIMIT];
static unsigned file_lock_count;

static void file_locks_drop(int owner)
{
    for (unsigned i = 0; i < file_lock_count;)
        if (file_locks[i].scope != 1 && file_locks[i].owner == owner)
            file_locks[i] = file_locks[--file_lock_count];
        else
            ++i;
}

static bool file_locks_overlap(const struct file_lock_record *a, const struct file_lock_record *b)
{
    return a->first <= b->last && b->first <= a->last;
}

static bool file_lock_same_owner(const struct file_lock_record *a, const struct file_lock_record *b)
{
    return a->scope == b->scope && a->owner == b->owner;
}

static int file_lock_range(struct lume_file_lock_request *request, uint64_t base,
                           struct file_lock_record *range)
{
    if (base > INT64_MAX)
        return -75;
    uint64_t start;
    if (request->start < 0)
    {
        uint64_t magnitude = (uint64_t)(-(request->start + 1)) + 1;
        if (magnitude > base)
            return -LUME_EINVAL;
        start = base - magnitude;
    }
    else
    {
        if ((uint64_t)request->start > INT64_MAX - base)
            return -75;
        start = base + (uint64_t)request->start;
    }
    if (request->length < 0)
    {
        uint64_t magnitude = (uint64_t)(-(request->length + 1)) + 1;
        if (magnitude > start)
            return -LUME_EINVAL;
        range->first = start - magnitude;
        range->last = start - 1;
    }
    else
    {
        range->first = start;
        if (!request->length)
            range->last = INT64_MAX;
        else
        {
            if ((uint64_t)request->length - 1 > INT64_MAX - start)
                return -75;
            range->last = start + (uint64_t)request->length - 1;
        }
    }
    return 0;
}

static bool file_lock_append(unsigned *count, struct file_lock_record record)
{
    if (*count == FILE_LOCK_LIMIT)
        return false;
    file_lock_stage[(*count)++] = record;
    return true;
}

static int file_locks_apply(struct file_lock_record range, bool query,
                            struct lume_file_lock_request *request)
{
    const struct file_lock_record *conflict = NULL;
    for (unsigned i = 0; i < file_lock_count; ++i)
    {
        const struct file_lock_record *held = file_locks + i;
        if (held->node != range.node || (held->scope == 2) != (range.scope == 2) ||
            file_lock_same_owner(held, &range) || !file_locks_overlap(held, &range) ||
            (held->type != LUME_FILE_LOCK_WRITE && range.type != LUME_FILE_LOCK_WRITE))
            continue;
        if (!conflict || held->first < conflict->first)
            conflict = held;
    }
    if (query)
    {
        request->type = conflict ? conflict->type : LUME_FILE_LOCK_UNLOCK;
        if (conflict)
        {
            request->whence = LUME_SEEK_SET;
            request->start = (int64_t)conflict->first;
            request->length =
                conflict->last == INT64_MAX ? 0 : (int64_t)(conflict->last - conflict->first + 1);
            request->pid = conflict->scope ? conflict->owner : -1;
        }
        return 0;
    }
    if (conflict && range.type != LUME_FILE_LOCK_UNLOCK)
        return -LUME_EAGAIN;

    /* Adjacent locks of one type form one range before replacement is staged. */
    if (range.type != LUME_FILE_LOCK_UNLOCK)
    {
        bool changed;
        do
        {
            changed = false;
            for (unsigned i = 0; i < file_lock_count; ++i)
            {
                const struct file_lock_record *held = file_locks + i;
                if (!file_lock_same_owner(held, &range) || held->node != range.node ||
                    held->type != range.type || held->first > range.last + 1 ||
                    range.first > held->last + 1)
                    continue;
                if (held->first < range.first)
                {
                    range.first = held->first;
                    changed = true;
                }
                if (held->last > range.last)
                {
                    range.last = held->last;
                    changed = true;
                }
            }
        } while (changed);
    }

    unsigned count = 0;
    for (unsigned i = 0; i < file_lock_count; ++i)
    {
        struct file_lock_record held = file_locks[i];
        if (!file_lock_same_owner(&held, &range) || held.node != range.node ||
            !file_locks_overlap(&held, &range))
        {
            if (!file_lock_append(&count, held))
                return -37;
            continue;
        }
        if (held.first < range.first)
        {
            struct file_lock_record left = held;
            left.last = range.first - 1;
            if (!file_lock_append(&count, left))
                return -37;
        }
        if (held.last > range.last)
        {
            struct file_lock_record right = held;
            right.first = range.last + 1;
            if (!file_lock_append(&count, right))
                return -37;
        }
    }
    if (range.type != LUME_FILE_LOCK_UNLOCK && !file_lock_append(&count, range))
        return -37;
    memcpy(file_locks, file_lock_stage, count * sizeof(file_locks[0]));
    file_lock_count = count;
    return 0;
}
#endif
