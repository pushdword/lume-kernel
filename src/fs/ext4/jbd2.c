#include "checksum.h"
#include "jbd2.h"
#include <limits.h>
#include <string.h>

#define JBD2_MAGIC UINT32_C(0xc03b3998)
#define JBD2_DESCRIPTOR 1u
#define JBD2_COMMIT 2u
#define JBD2_SUPER_V2 4u
#define JBD2_REVOKE_BLOCK 5u
#define JBD2_REVOKE 1u
#define JBD2_64BIT 2u
#define JBD2_CSUM_V2 8u
#define JBD2_CSUM_V3 16u
#define JBD2_CSUM (JBD2_CSUM_V2 | JBD2_CSUM_V3)
#define JBD2_FAST_COMMIT 32u
#define JBD2_ESCAPE 1u
#define JBD2_SAME_UUID 2u
#define JBD2_LAST_TAG 8u
#define JBD2_READ_AHEAD_BYTES (128u * 1024u)

static uint32_t get_be32(const unsigned char *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static unsigned get_be16(const unsigned char *p)
{
    return (unsigned)p[0] << 8 | p[1];
}

static uint32_t get_le32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static unsigned get_le16(const unsigned char *p)
{
    return (unsigned)p[0] | (unsigned)p[1] << 8;
}

static void put_be32(unsigned char *p, uint32_t value)
{
    p[0] = (unsigned char)(value >> 24);
    p[1] = (unsigned char)(value >> 16);
    p[2] = (unsigned char)(value >> 8);
    p[3] = (unsigned char)value;
}

static void put_be16(unsigned char *p, unsigned value)
{
    p[0] = (unsigned char)(value >> 8);
    p[1] = (unsigned char)value;
}

static void put_le16(unsigned char *p, unsigned value)
{
    p[0] = (unsigned char)value;
    p[1] = (unsigned char)(value >> 8);
}

static void put_le32(unsigned char *p, uint32_t value)
{
    put_le16(p, value);
    put_le16(p + 2, value >> 16);
}

static unsigned char *journal_super(struct lume_ext4_writer *w)
{
    return w->journal.window + (size_t)LUME_EXT4_JBD2_LOG_BLOCKS * w->fs->block_bytes;
}

static uint32_t super_checksum(unsigned char *s)
{
    uint32_t saved = get_be32(s + 0xfc);
    put_be32(s + 0xfc, 0);
    uint32_t checksum = lume_ext4_crc32c(UINT32_MAX, s, 1024);
    put_be32(s + 0xfc, saved);
    return checksum;
}

int lume_ext4_jbd2_open(struct lume_ext4_writer *w, const unsigned char *uuid,
                        const unsigned char *user, bool pending)
{
    struct lume_ext4_journal *j = &w->journal;
    size_t bytes = w->fs->block_bytes;
    j->window = w->memory + (size_t)LUME_EXT4_WRITE_BLOCKS * bytes;
    unsigned char *s = journal_super(w);
    if (j->io.read(j->io.context, j->superblock * bytes, s, bytes))
        return LUME_EXT4_IO;
    if (get_be32(s) != JBD2_MAGIC || get_be32(s + 0xc) != bytes || memcmp(s + 0x30, uuid, 16))
        return LUME_EXT4_INVALID;
    uint32_t features = get_be32(s + 0x28);
    j->start = get_be32(s + 0x1c);
    if (get_be32(s + 4) != JBD2_SUPER_V2 || (!pending && j->start) || get_be32(s + 0x20) ||
        get_be32(s + 0x24) ||
        (features & ~(JBD2_REVOKE | JBD2_64BIT | JBD2_CSUM | JBD2_FAST_COMMIT)) ||
        get_be32(s + 0x2c) || get_be32(s + 0x40) != 1 || get_be32(s + 0x44) ||
        (w->fs->blocks > (UINT64_C(1) << 32) && !(features & JBD2_64BIT)))
        return LUME_EXT4_UNSUPPORTED;
    if ((features & JBD2_CSUM) == JBD2_CSUM ||
        ((features & JBD2_CSUM) && (s[0x50] != 4 || get_be32(s + 0xfc) != super_checksum(s))))
        return LUME_EXT4_INVALID;
    if (user && memcmp(s + 0x100, user, 16))
        return LUME_EXT4_INVALID;
    j->first = get_be32(s + 0x14);
    j->end = get_be32(s + 0x10);
    uint32_t fast_blocks = get_be32(s + 0x54);
    if (j->end > j->blocks || ((features & JBD2_FAST_COMMIT) && !fast_blocks))
        return LUME_EXT4_INVALID;
    if (fast_blocks)
    {
        if (!(w->fs->compat & EXT4_FAST_COMMIT) || !w->fast_buffer)
            return LUME_EXT4_UNSUPPORTED;
        if (fast_blocks < 2 || fast_blocks >= j->end)
            return LUME_EXT4_INVALID;
        j->fast_end = j->end;
        j->end -= fast_blocks;
        j->fast_first = j->end + 1;
    }
    j->head = get_be32(s + 0x58);
    if (j->start)
        j->head = j->start;
    else if (!j->head)
        j->head = j->first;
    if (j->first <= (j->external ? j->superblock : 0) || j->first >= j->end || j->head < j->first ||
        j->head >= j->end)
        return LUME_EXT4_INVALID;
    if (j->end - j->first <= LUME_EXT4_JBD2_LOG_BLOCKS)
        return LUME_EXT4_UNSUPPORTED;
    j->sequence = get_be32(s + 0x18) + !j->start;
    j->features = features;
    j->seed = lume_ext4_crc32c(UINT32_MAX, s + 0x30, 16);
    return 0;
}

static bool write_journal_super(struct lume_ext4_writer *w, uint32_t start)
{
    struct lume_ext4_journal *j = &w->journal;
    unsigned char *s = journal_super(w);
    put_be32(s + 0x18, j->sequence);
    put_be32(s + 0x1c, start);
    put_be32(s + 0x28, start ? j->features : j->features & ~JBD2_FAST_COMMIT);
    if (!start)
        put_be32(s + 0x58, j->head);
    if (j->features & JBD2_CSUM)
        put_be32(s + 0xfc, super_checksum(s));
    size_t bytes = w->fs->block_bytes;
    return !j->io.write(j->io.context, j->superblock * bytes, s, bytes) &&
           !j->io.flush(j->io.context);
}

static void log_header(unsigned char *block, unsigned bytes, uint32_t type, uint32_t sequence)
{
    memset(block, 0, bytes);
    put_be32(block, JBD2_MAGIC);
    put_be32(block + 4, type);
    put_be32(block + 8, sequence);
}

static bool transfer_log(struct lume_ext4_writer *w, uint32_t logical, unsigned count, bool write)
{
    struct lume_ext4_journal *j = &w->journal;
    unsigned char *buffer = j->window;
    while (count)
    {
        uint64_t physical;
        unsigned span;
        if (lume_ext4_journal_map(w, logical, &physical, &span))
            return false;
        if (span > j->end - logical)
            span = j->end - logical;
        if (span > count)
            span = count;
        size_t bytes = (size_t)span * w->fs->block_bytes;
        uint64_t offset = physical * w->fs->block_bytes;
        if (write ? j->io.write(j->io.context, offset, buffer, bytes)
                  : j->io.read(j->io.context, offset, buffer, bytes))
            return false;
        buffer += bytes;
        count -= span;
        logical += span;
        if (logical == j->end)
            logical = j->first;
    }
    return true;
}

static unsigned journal_tag_bytes(uint32_t features)
{
    if (features & JBD2_CSUM_V3)
        return 16;
    /* A v2 tag adds two bytes to the legacy layout. */
    return 8 + (features & JBD2_64BIT ? 4 : 0) + (features & JBD2_CSUM_V2 ? 2 : 0);
}

static unsigned serialize(struct lume_ext4_writer *w, unsigned data_phase)
{
    struct lume_ext4_journal *j = &w->journal;
    unsigned bytes = w->fs->block_bytes;
    bool checksum = (j->features & JBD2_CSUM) != 0;
    unsigned tag_bytes = journal_tag_bytes(j->features);
    unsigned descriptor_tail = bytes - (checksum ? 4 : 0);
    const unsigned char *uuid = journal_super(w) + 0x30;
    unsigned char *next = j->window;
    unsigned char *descriptor = NULL, *tag = NULL, *last = NULL;
    uint32_t payload_seed = j->seed;
    unsigned char sequence[4];
    put_be32(sequence, j->sequence);
    if (checksum)
        payload_seed = lume_ext4_crc32c(j->seed, sequence, sizeof sequence);
    for (unsigned i = 0; i < w->used; ++i)
    {
        const struct lume_ext4_write_block *entry = &w->block[i];
        if (entry->number != w->fs->first_data_block &&
            (!entry->dirty || entry->phase == data_phase))
            continue;
        if (!descriptor)
        {
            descriptor = next;
            next += bytes;
            log_header(descriptor, bytes, JBD2_DESCRIPTOR, j->sequence);
            tag = descriptor + 12;
        }
        bool first = tag == descriptor + 12;
        unsigned flags = first ? 0 : JBD2_SAME_UUID;
        memcpy(next, entry->data, bytes);
        if (get_be32(next) == JBD2_MAGIC)
        {
            put_be32(next, 0);
            flags |= JBD2_ESCAPE;
        }
        put_be32(tag, (uint32_t)entry->number);
        put_be32(tag + 4, flags);
        if (j->features & JBD2_64BIT)
            put_be32(tag + 8, (uint32_t)(entry->number >> 32));
        if (checksum)
        {
            uint32_t sum = lume_ext4_crc32c(payload_seed, next, bytes);
            if (j->features & JBD2_CSUM_V3)
                put_be32(tag + 12, sum);
            else
                put_be16(tag + 4, sum);
        }
        last = tag;
        tag += tag_bytes;
        if (first)
        {
            memcpy(tag, uuid, 16);
            tag += 16;
        }
        next += bytes;
        if ((size_t)(tag - descriptor) + tag_bytes > descriptor_tail)
        {
            last[7] |= JBD2_LAST_TAG;
            if (checksum)
                put_be32(descriptor + bytes - 4, lume_ext4_crc32c(j->seed, descriptor, bytes));
            descriptor = NULL;
        }
    }
    if (descriptor)
    {
        last[7] |= JBD2_LAST_TAG;
        if (checksum)
            put_be32(descriptor + bytes - 4, lume_ext4_crc32c(j->seed, descriptor, bytes));
    }
    log_header(next, bytes, JBD2_COMMIT, j->sequence);
    put_be32(next + 0x34, w->timestamp);
    if (checksum)
        put_be32(next + 0x10, lume_ext4_crc32c(j->seed, next, bytes));
    next += bytes;
    memset(next, 0, bytes);
    return (unsigned)((next - j->window) / bytes) + 1;
}

static uint32_t log_position(const struct lume_ext4_journal *j, uint32_t start, unsigned count)
{
    unsigned remaining = j->end - start;
    return count < remaining ? start + count : j->first + (count - remaining);
}

bool lume_ext4_jbd2_publish(struct lume_ext4_writer *w, unsigned data_phase)
{
    struct lume_ext4_journal *j = &w->journal;
    unsigned count = serialize(w, data_phase);
    j->features &= ~JBD2_FAST_COMMIT;
    /* Keep the log undiscoverable until the commit and all payload blocks are durable. */
    if (!transfer_log(w, j->head, count, true) || j->io.flush(j->io.context) ||
        !write_journal_super(w, j->head))
        return false;
    j->head = log_position(j, j->head, count);
    return true;
}

int lume_ext4_jbd2_publish_recovery(struct lume_ext4_writer *w)
{
    struct lume_ext4_journal *j = &w->journal;
    /* The earlier log remains authoritative until the journal superblock changes.
     * Use a later sequence so its scan cannot see this replacement as a continuation. */
    j->sequence += 2;
    j->features &= ~JBD2_FAST_COMMIT;
    unsigned count = serialize(w, UINT_MAX);
    if (count >= j->end - j->first - j->replay_blocks)
        return LUME_EXT4_NO_SPACE;
    if (!transfer_log(w, j->head, count, true) || j->io.flush(j->io.context) ||
        !write_journal_super(w, j->head))
        return LUME_EXT4_IO;
    j->head = log_position(j, j->head, count);
    return 0;
}

bool lume_ext4_jbd2_checkpoint(struct lume_ext4_writer *w)
{
    /* The caller has flushed every home block. */
    ++w->journal.sequence;
    return write_journal_super(w, 0);
}

static bool valid_checksum(unsigned char *p, unsigned bytes, unsigned offset, uint32_t seed)
{
    uint32_t stored = get_be32(p + offset);
    put_be32(p + offset, 0);
    uint32_t actual = lume_ext4_crc32c(seed, p, bytes);
    put_be32(p + offset, stored);
    return stored == actual;
}

static bool same_super(const unsigned char *current, const unsigned char *logged)
{
    /* Geometry and features stay fixed while the orphan head and mount path can change. */
    return !memcmp(current, logged, 0xc) && !memcmp(current + 0x14, logged + 0x14, 0x30 - 0x14) &&
           !memcmp(current + 0x34, logged + 0x34, 0x88 - 0x34) &&
           !memcmp(current + 0xc8, logged + 0xc8, EXT4_SB_LAST_ORPHAN - 0xc8) &&
           !memcmp(current + 0xec, logged + 0xec, 0x158 - 0xec) &&
           !memcmp(current + 0x15c, logged + 0x15c, 0x3fc - 0x15c);
}

static bool replay_super(const struct lume_ext4 *fs, const unsigned char *current,
                         unsigned char *data)
{
    unsigned offset = fs->block_bytes == 1024 ? 0 : 1024;
    const unsigned char *before = current - offset;
    unsigned tail = fs->block_bytes - offset - 1024;
    unsigned char *s = data + offset;
    uint64_t free = get_le32(s + 0xc);
    uint32_t orphan = get_le32(s + EXT4_SB_LAST_ORPHAN);
    if (fs->incompat & 0x80)
        free |= (uint64_t)get_le32(s + 0x158) << 32;
    return free <= fs->blocks && get_le32(s + 0x10) <= fs->inodes &&
           (!orphan || (orphan >= get_le32(s + 0x54) && orphan <= fs->inodes)) &&
           !memcmp(before, data, offset) && !memcmp(current + 1024, s + 1024, tail) &&
           same_super(current, s) && lume_ext4_crc32c(UINT32_MAX, s, 0x3fc) == get_le32(s + 0x3fc);
}

enum replay_pass
{
    SCAN_LOG,
    COLLECT_REVOKES,
    WRITE_HOME
};

struct revoke_entry
{
    uint64_t physical;
    unsigned transaction;
};

struct replay
{
    struct lume_ext4_writer *writer;
    struct revoke_entry *revokes;
    unsigned slots, revoked;
    unsigned char *superblock, *descriptor;
    unsigned first, buffered, ring_blocks;
    bool verified_window;
    const unsigned char *run_data;
    uint64_t run_first;
    unsigned run_blocks;
};

static int flush_run(struct replay *r)
{
    if (!r->run_blocks)
        return 0;
    struct lume_ext4_writer *w = r->writer;
    size_t bytes = w->fs->block_bytes;
    if (w->write(w->fs->context, r->run_first * bytes, r->run_data, r->run_blocks * bytes))
        return LUME_EXT4_IO;
    r->run_blocks = 0;
    return 0;
}

static int read_log(struct replay *r, unsigned position, unsigned char **data)
{
    struct lume_ext4_writer *w = r->writer;
    struct lume_ext4_journal *j = &w->journal;
    if (position >= r->ring_blocks)
        return LUME_EXT4_INVALID;
    if (position < r->first || position - r->first >= r->buffered)
    {
        int result = flush_run(r);
        if (result)
            return result;
        unsigned count = r->ring_blocks - position;
        unsigned read_ahead = JBD2_READ_AHEAD_BYTES / w->fs->block_bytes;
        if (count > read_ahead)
            count = read_ahead;
        uint32_t logical = log_position(j, j->start, position);
        if (!transfer_log(w, logical, count, false))
            return LUME_EXT4_IO;
        r->first = position;
        r->buffered = count;
        r->verified_window = false;
    }
    *data = j->window + (size_t)(position - r->first) * w->fs->block_bytes;
    return 0;
}

static bool valid_target(const struct replay *r, uint64_t physical)
{
    const struct lume_ext4_writer *w = r->writer;
    return physical >= w->fs->first_data_block && physical < w->fs->blocks &&
           !lume_ext4_journal_overlaps(w, physical, 1);
}

static struct revoke_entry *revoke_slot(const struct replay *r, uint64_t physical)
{
    uint32_t key = (uint32_t)(physical ^ (physical >> 32));
    key ^= key >> 16;
    key *= UINT32_C(0x7feb352d);
    key ^= key >> 15;
    unsigned mask = r->slots - 1;
    unsigned index = key & mask;
    while (r->revokes[index].transaction && r->revokes[index].physical != physical)
        index = (index + 1) & mask;
    return &r->revokes[index];
}

static int revoke_record(struct replay *r, unsigned char *record, unsigned transaction,
                         enum replay_pass pass)
{
    const struct lume_ext4_writer *w = r->writer;
    const struct lume_ext4_journal *j = &w->journal;
    unsigned size = j->features & JBD2_64BIT ? 8 : 4;
    unsigned bytes = w->fs->block_bytes;
    bool checksum = (j->features & JBD2_CSUM) != 0;
    unsigned end = get_be32(record + 12);
    if (!(j->features & JBD2_REVOKE))
        return LUME_EXT4_INVALID;
    if (end < 16 || end > bytes - (checksum ? 4 : 0) || (end - 16) % size)
        return LUME_EXT4_INVALID;
    if (!r->verified_window && checksum && !valid_checksum(record, bytes, bytes - 4, j->seed))
        return LUME_EXT4_INVALID;
    if (pass == WRITE_HOME)
        return 0;
    for (unsigned offset = 16; offset < end; offset += size)
    {
        uint64_t physical = get_be32(record + offset);
        if (size == 8)
            physical = physical << 32 | get_be32(record + offset + 4);
        if (!valid_target(r, physical) || physical == w->fs->first_data_block)
            return LUME_EXT4_INVALID;
        if (pass == COLLECT_REVOKES)
        {
            struct revoke_entry *entry = revoke_slot(r, physical);
            if (!entry->transaction && r->revoked++ == r->slots / 2)
                return LUME_EXT4_UNSUPPORTED;
            *entry = (struct revoke_entry){physical, transaction};
        }
    }
    return 0;
}

static int queue_home(struct replay *r, uint64_t physical, unsigned char *payload, bool escaped)
{
    const struct lume_ext4 *fs = r->writer->fs;
    unsigned bytes = fs->block_bytes;
    if (escaped)
        put_be32(payload, JBD2_MAGIC);
    if (r->run_blocks && (physical != r->run_first + r->run_blocks ||
                          payload != r->run_data + (size_t)r->run_blocks * bytes))
    {
        int result = flush_run(r);
        if (result)
            return result;
    }
    if (!r->run_blocks)
    {
        r->run_first = physical;
        r->run_data = payload;
    }
    ++r->run_blocks;
    if (physical == fs->first_data_block)
        memcpy(r->superblock, payload + (bytes == 1024 ? 0 : 1024), 1024);
    return 0;
}

static bool valid_payload(struct replay *r, uint64_t physical, unsigned char *payload, bool escaped)
{
    const struct lume_ext4 *fs = r->writer->fs;
    if (escaped && get_be32(payload))
        return false;
    if (physical != fs->first_data_block)
        return true;
    if (escaped)
        put_be32(payload, JBD2_MAGIC);
    bool valid = replay_super(fs, r->superblock, payload);
    if (escaped)
        put_be32(payload, 0);
    return valid;
}

static bool valid_tag_checksum(uint32_t features, const unsigned char *tag, uint32_t seed,
                               const void *payload, size_t bytes)
{
    if (!(features & JBD2_CSUM))
        return true;
    uint32_t sum = lume_ext4_crc32c(seed, payload, bytes);
    return features & JBD2_CSUM_V3 ? get_be32(tag + 12) == sum
                                   : get_be16(tag + 4) == (uint16_t)sum;
}

static int descriptor_record(struct replay *r, unsigned char *record, unsigned *position,
                             unsigned limit, unsigned transaction, enum replay_pass pass,
                             bool *damaged)
{
    const struct lume_ext4_writer *w = r->writer;
    const struct lume_ext4_journal *j = &w->journal;
    unsigned bytes = w->fs->block_bytes;
    bool checksum = (j->features & JBD2_CSUM) != 0;
    if (!r->verified_window && checksum && !valid_checksum(record, bytes, bytes - 4, j->seed))
        return LUME_EXT4_INVALID;

    /* Loading payloads can replace the read window. */
    memcpy(r->descriptor, record, bytes);
    unsigned tag_size = journal_tag_bytes(j->features);
    unsigned tail = bytes - (checksum ? 4 : 0);
    const unsigned char *uuid = journal_super(r->writer) + 0x30;
    uint32_t seed = checksum ? lume_ext4_crc32c(j->seed, r->descriptor + 8, 4) : 0;
    for (unsigned offset = 12; offset + tag_size <= tail;)
    {
        const unsigned char *tag = r->descriptor + offset;
        uint32_t flags = j->features & JBD2_CSUM_V2 ? get_be16(tag + 6) : get_be32(tag + 4);
        offset += tag_size;
        if (flags & ~(JBD2_ESCAPE | JBD2_LAST_TAG | JBD2_SAME_UUID))
            return LUME_EXT4_UNSUPPORTED;
        if (!(flags & JBD2_SAME_UUID))
        {
            /* Linux can emit a zero tag UUID for an external journal. */
            static const unsigned char empty_uuid[16];
            const unsigned char *tag_uuid = r->descriptor + offset;
            if (offset + 16 > tail ||
                (memcmp(tag_uuid, uuid, 16) && memcmp(tag_uuid, r->superblock + 0xd0, 16) &&
                 (!j->external || memcmp(tag_uuid, empty_uuid, 16))))
                return LUME_EXT4_INVALID;
            offset += 16;
        }
        else if (tag == r->descriptor + 12)
            return LUME_EXT4_INVALID;
        if (*position >= limit)
            return LUME_EXT4_INVALID;
        unsigned payload_position = (*position)++;
        uint64_t physical = get_be32(tag);
        if (j->features & JBD2_64BIT)
            physical |= (uint64_t)get_be32(tag + 8) << 32;
        if (!valid_target(r, physical))
            return LUME_EXT4_INVALID;

        bool escaped = (flags & JBD2_ESCAPE) != 0;
        if (pass == SCAN_LOG && (checksum || escaped || physical == w->fs->first_data_block))
        {
            unsigned char *payload;
            int result = read_log(r, payload_position, &payload);
            if (result)
                return result;
            if (!valid_tag_checksum(j->features, tag, seed, payload, bytes) ||
                !valid_payload(r, physical, payload, escaped))
                *damaged = true;
        }
        if (pass == WRITE_HOME &&
            (!r->revoked || revoke_slot(r, physical)->transaction < transaction))
        {
            unsigned char *payload;
            int result = read_log(r, payload_position, &payload);
            if (result)
                return result;
            if (!r->verified_window &&
                (!valid_tag_checksum(j->features, tag, seed, payload, bytes) ||
                 !valid_payload(r, physical, payload, escaped)))
                return LUME_EXT4_INVALID;
            result = queue_home(r, physical, payload, escaped);
            if (result)
                return result;
        }
        if (flags & JBD2_LAST_TAG)
            return 0;
    }
    return LUME_EXT4_INVALID;
}

struct committed_log
{
    unsigned blocks;
    uint32_t next_sequence;
    bool has_revokes;
};

static int fast_block_io(struct lume_ext4_writer *w, uint32_t logical, bool write)
{
    struct lume_ext4_journal *j = &w->journal;
    uint64_t physical;
    unsigned span;
    int result = lume_ext4_journal_map(w, logical, &physical, &span);
    if (result)
        return result;
    size_t bytes = w->fs->block_bytes;
    uint64_t offset = physical * bytes;
    result = write ? j->io.write(j->io.context, offset, w->fast_buffer, bytes)
                   : j->io.read(j->io.context, offset, w->fast_buffer, bytes);
    return result ? LUME_EXT4_IO : 0;
}

bool lume_ext4_jbd2_publish_inode(struct lume_ext4_writer *w, uint32_t number,
                                  const unsigned char *inode, unsigned length)
{
    struct lume_ext4_journal *j = &w->journal;
    unsigned bytes = w->fs->block_bytes;
    unsigned char *block = w->fast_buffer;
    memset(block, 0, bytes);
    if (j->fast_first + 1 < j->fast_end && fast_block_io(w, j->fast_first + 1, true))
        return false;
    put_le16(block, LUME_EXT4_FC_HEAD);
    put_le16(block + 2, 8);
    put_le32(block + 8, j->sequence);
    put_le16(block + 12, LUME_EXT4_FC_INODE);
    put_le16(block + 14, length + 4);
    put_le32(block + 16, number);
    memcpy(block + 20, inode, length);
    unsigned tail = 20 + length;
    put_le16(block + tail, LUME_EXT4_FC_TAIL);
    put_le16(block + tail + 2, bytes - tail - 4);
    put_le32(block + tail + 4, j->sequence);
    put_le32(block + tail + 8, lume_ext4_crc32c(0, block, tail + 8));
    memset(j->window, 0, bytes);
    /* An empty full-commit slot anchors the sequence of the fast commit. */
    if (!transfer_log(w, j->head, 1, true) || fast_block_io(w, j->fast_first, true) ||
        j->io.flush(j->io.context))
        return false;
    j->features |= JBD2_FAST_COMMIT;
    return write_journal_super(w, j->head);
}

static bool fast_length(const struct lume_ext4 *fs, unsigned tag, unsigned length)
{
    switch (tag)
    {
    case LUME_EXT4_FC_ADD_RANGE:
        return length == 16;
    case LUME_EXT4_FC_DEL_RANGE:
        return length == 12;
    case LUME_EXT4_FC_CREATE:
    case LUME_EXT4_FC_LINK:
    case LUME_EXT4_FC_UNLINK:
        return length > 8 && length <= 8 + 255;
    case LUME_EXT4_FC_INODE:
        return length >= 4 + 128 && length <= 4u + fs->inode_bytes;
    case LUME_EXT4_FC_PAD:
        return true;
    case LUME_EXT4_FC_TAIL:
        return length >= 8;
    case LUME_EXT4_FC_HEAD:
        return length == 8;
    default:
        return false;
    }
}

static int scan_fast(struct lume_ext4_writer *w, uint32_t sequence, uint32_t limit,
                     uint32_t *committed, lume_ext4_fast_visitor visit)
{
    uint32_t first = w->journal.fast_first;
    unsigned bytes = w->fs->block_bytes;
    unsigned char *block = w->fast_buffer;
    uint32_t crc = 0;
    *committed = first;
    for (uint32_t index = first; index < limit; ++index)
    {
        int result = fast_block_io(w, index, false);
        if (result)
            return result;
        if (index == first && get_le16(block) != LUME_EXT4_FC_HEAD)
            return 0;
        for (unsigned offset = 0; offset <= bytes - 4;)
        {
            const unsigned char *record = block + offset;
            unsigned tag = get_le16(record), length = get_le16(record + 2);
            unsigned total = 4 + length;
            bool head = index == first && offset == 0;
            if (total > bytes - offset || !fast_length(w->fs, tag, length) ||
                (!head && tag == LUME_EXT4_FC_HEAD))
                return *committed != first || !tag ? 0 : LUME_EXT4_INVALID;
            if (tag == LUME_EXT4_FC_HEAD)
            {
                if (get_le32(record + 4))
                    return LUME_EXT4_UNSUPPORTED;
                if (get_le32(record + 8) != sequence)
                    return 0;
            }
            if (tag == LUME_EXT4_FC_TAIL)
            {
                uint32_t tail_crc = lume_ext4_crc32c(crc, record, 8);
                if (offset + total != bytes || get_le32(record + 4) != sequence ||
                    get_le32(record + 8) != tail_crc)
                    return *committed != first ? 0 : LUME_EXT4_INVALID;
                *committed = index + 1;
                crc = 0;
            }
            else
            {
                crc = lume_ext4_crc32c(crc, record, total);
                if (visit && tag != LUME_EXT4_FC_HEAD && tag != LUME_EXT4_FC_PAD)
                {
                    result = visit(w, tag, record + 4, length);
                    if (result)
                        return result;
                }
            }
            offset += total;
        }
    }
    return 0;
}

int lume_ext4_jbd2_visit_fast(struct lume_ext4_writer *w, lume_ext4_fast_visitor visit)
{
    struct lume_ext4_journal *j = &w->journal;
    uint32_t end;
    int result = scan_fast(w, j->sequence + 1, j->fast_committed, &end, visit);
    return result ? result : end == j->fast_committed ? 0 : LUME_EXT4_INVALID;
}

static int scan_log(struct replay *r, unsigned limit, enum replay_pass pass,
                    struct committed_log *committed)
{
    struct lume_ext4_journal *j = &r->writer->journal;
    unsigned bytes = r->writer->fs->block_bytes;
    unsigned position = 0, transaction = 1;
    uint32_t sequence = j->sequence;
    bool damaged = false, has_revokes = false;
    while (position < limit)
    {
        unsigned char *record;
        int result = read_log(r, position++, &record);
        if (result)
            return result;
        if (get_be32(record) != JBD2_MAGIC || get_be32(record + 8) != sequence)
            return pass == SCAN_LOG ? 0 : LUME_EXT4_INVALID;
        switch (get_be32(record + 4))
        {
        case JBD2_DESCRIPTOR:
            result = descriptor_record(r, record, &position, limit, transaction, pass, &damaged);
            break;
        case JBD2_REVOKE_BLOCK:
            has_revokes = true;
            result = revoke_record(r, record, transaction, pass);
            break;
        case JBD2_COMMIT:
            if (damaged || (!r->verified_window && (j->features & JBD2_CSUM) &&
                            !valid_checksum(record, bytes, 0x10, j->seed)))
                return LUME_EXT4_INVALID;
            if (pass == SCAN_LOG)
                *committed = (struct committed_log){position, sequence + 1, has_revokes};
            ++transaction;
            ++sequence;
            damaged = false;
            break;
        default:
            return LUME_EXT4_UNSUPPORTED;
        }
        if (result)
            return result;
    }
    return pass == SCAN_LOG ? LUME_EXT4_INVALID : 0;
}

int lume_ext4_jbd2_replay(struct lume_ext4_writer *w, unsigned char *superblock)
{
    struct lume_ext4_journal *j = &w->journal;
    if (!j->start)
        return 0;

    /* Slot zero holds the primary superblock. Recovery can borrow the remaining
     * staging slots until the caller scans the group descriptors. */
    size_t bytes = w->fs->block_bytes;
    uintptr_t first = (uintptr_t)(w->memory + 2 * bytes);
    size_t alignment = _Alignof(struct revoke_entry);
    first = (first + alignment - 1) & ~(uintptr_t)(alignment - 1);
    uintptr_t end = (uintptr_t)(w->memory + LUME_EXT4_WRITE_BLOCKS * bytes);
    unsigned available = (unsigned)((end - first) / sizeof(struct revoke_entry));
    unsigned slots = 1;
    while (slots <= available / 2)
        slots *= 2;
    struct replay r = {.writer = w,
                       .revokes = (struct revoke_entry *)first,
                       .slots = slots,
                       .superblock = superblock,
                       .descriptor = w->memory + bytes,
                       .ring_blocks = j->end - j->first};
    struct committed_log committed = {.next_sequence = j->sequence};
    int result = scan_log(&r, r.ring_blocks, SCAN_LOG, &committed);
    if (result)
        return result;
    if (j->features & JBD2_FAST_COMMIT)
    {
        result = scan_fast(w, committed.next_sequence, j->fast_end, &j->fast_committed, NULL);
        if (result)
            return result;
    }
    /* The final scan window can be reused. A refill requires checksum checks. */
    r.verified_window = true;
    if (committed.blocks)
    {
        if (committed.has_revokes)
        {
            memset(r.revokes, 0, (size_t)slots * sizeof(*r.revokes));
            result = scan_log(&r, committed.blocks, COLLECT_REVOKES, NULL);
        }
        if (!result)
            result = scan_log(&r, committed.blocks, WRITE_HOME, NULL);
        if (!result)
            result = flush_run(&r);
        if (result)
            return result;
        if (w->flush(w->fs->context))
            return LUME_EXT4_IO;
    }
    j->head = log_position(j, j->start, committed.blocks);
    j->replay_blocks = committed.blocks;
    j->sequence = committed.next_sequence - 1;
    return 0;
}
