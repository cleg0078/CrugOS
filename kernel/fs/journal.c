#include <crug/journal.h>
#include <crug/ext4.h>
#include <crug/block.h>
#include <crug/heap.h>
#include <crug/string.h>
#include <crug/kernel.h>
#include "ext4_internal.h"

#define JBD2_DESCRIPTOR_BLOCK 1u
#define JBD2_COMMIT_BLOCK 2u
#define JBD2_SUPERBLOCK_V2 4u
#define JBD2_FLAG_SAME_UUID 0x0002u
#define JBD2_FLAG_LAST_TAG 0x0008u
#define JOURNAL_MAX_PENDING 256u
#define JOURNAL_UUID_LEN 16u

struct journal_buf {
    struct journal_buf *next;
    u64 block;
    u8 *data;
};

struct journal {
    struct ext4_fs *fs;
    u32 block_size;
    u32 maxlen;
    u32 first;
    u32 sequence;
    u32 start;
    u32 depth;
    u32 pending_count;
    u32 recovered;
    int active;
    struct journal_buf *pending;
    struct journal_buf *tail;
    u8 *sb;
    u8 *scratch;
};

static u32 be32(const u8 *p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) |
           ((u32)p[2] << 8) | (u32)p[3];
}

static void put_be32(u8 *p, u32 v)
{
    p[0] = (u8)(v >> 24);
    p[1] = (u8)(v >> 16);
    p[2] = (u8)(v >> 8);
    p[3] = (u8)v;
}

static int journal_map(struct journal *j, u32 logical, u64 *phys)
{
    return ext4_journal_map_block(j->fs, logical, phys);
}

static int journal_read_log(struct journal *j, u32 logical, void *buf)
{
    u64 phys;

    if (journal_map(j, logical, &phys) != 0)
        return -1;
    return ext4_raw_read_block(j->fs, phys, buf);
}

static int journal_write_log(struct journal *j, u32 logical, const void *buf)
{
    u64 phys;

    if (journal_map(j, logical, &phys) != 0)
        return -1;
    return ext4_raw_write_block(j->fs, phys, buf);
}

static u32 journal_next(struct journal *j, u32 pos)
{
    pos++;
    if (pos >= j->maxlen)
        pos = j->first;
    return pos;
}

static void journal_free_pending(struct journal *j)
{
    struct journal_buf *b = j->pending;

    while (b) {
        struct journal_buf *next = b->next;

        kfree(b->data);
        kfree(b);
        b = next;
    }
    j->pending = 0;
    j->tail = 0;
    j->pending_count = 0;
}

static int journal_write_super(struct journal *j, u32 start, u32 sequence)
{
    u8 *sb = j->sb;

    if (journal_read_log(j, 0, sb) != 0)
        return -1;
    if (be32(sb) != JOURNAL_MAGIC || be32(sb + 4) != JBD2_SUPERBLOCK_V2)
        return -1;
    put_be32(sb + 24, sequence);
    put_be32(sb + 28, start);
    if (journal_write_log(j, 0, sb) != 0)
        return -1;
    j->start = start;
    j->sequence = sequence;
    return block_flush(j->fs->bd);
}

int journal_is_active(struct ext4_fs *fs)
{
    struct journal *j;

    if (!fs || !fs->journal)
        return 0;
    j = (struct journal *)fs->journal;
    return j->active;
}

int journal_is_clean(struct ext4_fs *fs)
{
    struct journal *j;

    if (!fs || !fs->journal)
        return 1;
    j = (struct journal *)fs->journal;
    return j->start == 0;
}

u32 journal_recovered_blocks(struct ext4_fs *fs)
{
    struct journal *j;

    if (!fs || !fs->journal)
        return 0;
    j = (struct journal *)fs->journal;
    return j->recovered;
}

int journal_lookup(struct ext4_fs *fs, u64 block, void *buf)
{
    struct journal *j;
    struct journal_buf *b;

    if (!fs || !fs->journal || !buf)
        return 0;
    j = (struct journal *)fs->journal;
    for (b = j->pending; b; b = b->next) {
        if (b->block == block) {
            memcpy(buf, b->data, j->block_size);
            return 1;
        }
    }
    return 0;
}

int journal_queue(struct ext4_fs *fs, u64 block, const void *buf)
{
    struct journal *j;
    struct journal_buf *b;

    if (!fs || !fs->journal || !buf)
        return -1;
    j = (struct journal *)fs->journal;
    if (block == 0 || block >= fs->blocks_count)
        return -1;
    for (b = j->pending; b; b = b->next) {
        if (b->block == block) {
            memcpy(b->data, buf, j->block_size);
            return 0;
        }
    }
    if (j->pending_count >= JOURNAL_MAX_PENDING)
        return -1;
    b = (struct journal_buf *)kzalloc(sizeof(*b));
    if (!b)
        return -1;
    b->data = (u8 *)kmalloc(j->block_size);
    if (!b->data) {
        kfree(b);
        return -1;
    }
    b->block = block;
    memcpy(b->data, buf, j->block_size);
    if (j->tail)
        j->tail->next = b;
    else
        j->pending = b;
    j->tail = b;
    j->pending_count++;
    return 0;
}

int journal_begin(struct ext4_fs *fs)
{
    struct journal *j;

    if (!fs || !fs->journal)
        return 0;
    j = (struct journal *)fs->journal;
    if (j->depth == 0)
        j->active = 1;
    j->depth++;
    return 0;
}

static u32 descriptor_capacity(struct journal *j)
{
    return (j->block_size - 12u) / (8u + JOURNAL_UUID_LEN);
}
static int journal_commit_internal(struct journal *j, int skip_checkpoint)
{
    struct journal_buf *b;
    u32 seq = j->sequence;
    u32 pos = j->first;
    u32 cap = descriptor_capacity(j);
    u32 n = j->pending_count;
    u32 chunks;
    u32 i;
    u8 *desc = j->scratch;

    if (n == 0)
        return 0;
    if (cap == 0)
        return -1;
    /* Recovery validates and applies one descriptor per transaction. Keep the
     * writer inside that shape instead of emitting descriptor chunks. */
    if (n > cap)
        return -1;
    chunks = 1;
    if (n + chunks + 1 > j->maxlen - 1)
        return -1;
    if (block_flush(j->fs->bd) != 0)
        return -1;
    if (journal_write_super(j, j->first, seq) != 0)
        return -1;
    b = j->pending;
    while (b) {
        u32 count = n > cap ? cap : n;

        memset(desc, 0, j->block_size);
        put_be32(desc, JOURNAL_MAGIC);
        put_be32(desc + 4, JBD2_DESCRIPTOR_BLOCK);
        put_be32(desc + 8, seq);
        for (i = 0; i < count; i++) {
            u32 off = 12 + i * (8u + JOURNAL_UUID_LEN);
            u32 flags = (i + 1 == count) ? JBD2_FLAG_LAST_TAG : 0;

            put_be32(desc + off, (u32)b->block);
            put_be32(desc + off + 4, flags);
            memcpy(desc + off + 8, j->sb + 48, JOURNAL_UUID_LEN);
            b = b->next;
        }
        if (journal_write_log(j, pos, desc) != 0)
            return -1;
        pos = journal_next(j, pos);
        b = j->pending;
        for (i = 0; i < n && i < count; i++) {
            if (journal_write_log(j, pos, b->data) != 0)
                return -1;
            pos = journal_next(j, pos);
            b = b->next;
        }
        n -= count;
    }
    memset(desc, 0, j->block_size);
    put_be32(desc, JOURNAL_MAGIC);
    put_be32(desc + 4, JBD2_COMMIT_BLOCK);
    put_be32(desc + 8, seq);
    if (journal_write_log(j, pos, desc) != 0)
        return -1;
    pos = journal_next(j, pos);
    if (block_flush(j->fs->bd) != 0)
        return -1;
    if (skip_checkpoint)
        return 0;
    for (b = j->pending; b; b = b->next) {
        if (ext4_raw_write_block(j->fs, b->block, b->data) != 0)
            return -1;
    }
    if (block_flush(j->fs->bd) != 0)
        return -1;
    if (journal_write_super(j, 0, seq + 1) != 0)
        return -1;
    j->sequence = seq + 1;
    j->start = 0;
    journal_free_pending(j);
    return 0;
}

int journal_commit(struct ext4_fs *fs)
{
    struct journal *j;

    if (!fs || !fs->journal)
        return 0;
    j = (struct journal *)fs->journal;
    if (j->depth == 0)
        return 0;
    j->depth--;
    if (j->depth > 0)
        return 0;
    j->active = 0;
    return journal_commit_internal(j, 0);
}

int journal_recover(struct ext4_fs *fs)
{
    struct journal *j;
    u64 *targets;
    u32 pos;
    u32 seq;
    u32 iter;
    u32 i;

    if (!fs || !fs->journal)
        return -1;
    j = (struct journal *)fs->journal;
    if (j->start == 0)
        return 0;
    targets = (u64 *)kmalloc(256 * sizeof(u64));
    if (!targets)
        return -1;
    pos = j->start;
    seq = j->sequence;
    for (iter = 0; iter < 1024 && pos; iter++) {
        u32 ntags = 0;
        u32 off;
        u32 cpos;
        int last = 0;
        u8 *desc = j->scratch;

        if (journal_read_log(j, pos, desc) != 0)
            break;
        if (be32(desc) != JOURNAL_MAGIC ||
            be32(desc + 4) != JBD2_DESCRIPTOR_BLOCK ||
            be32(desc + 8) != seq)
            break;
        off = 12;
        while (off + 8 <= j->block_size) {
            u32 block = be32(desc + off);
            u32 flags = be32(desc + off + 4);

            off += 8;
            if (block == 0)
                break;
            if (!(flags & JBD2_FLAG_SAME_UUID)) {
                if (off + JOURNAL_UUID_LEN > j->block_size)
                    break;
                off += JOURNAL_UUID_LEN;
            }
            if (block < j->fs->blocks_count && ntags < 256)
                targets[ntags++] = block;
            if (flags & JBD2_FLAG_LAST_TAG) {
                last = 1;
                break;
            }
        }
        if (ntags == 0 || !last)
            break;
        cpos = pos;
        for (i = 0; i <= ntags; i++)
            cpos = journal_next(j, cpos);
        if (journal_read_log(j, cpos, desc) != 0)
            break;
        if (be32(desc) != JOURNAL_MAGIC ||
            be32(desc + 4) != JBD2_COMMIT_BLOCK ||
            be32(desc + 8) != seq)
            break;
        for (i = 0; i < ntags; i++) {
            u32 dpos = pos;
            u32 k;

            for (k = 0; k <= i; k++)
                dpos = journal_next(j, dpos);
            if (journal_read_log(j, dpos, j->scratch) != 0)
                break;
            if (ext4_raw_write_block(j->fs, targets[i], j->scratch) != 0)
                break;
            j->recovered++;
        }
        if (i != ntags)
            break;
        pos = journal_next(j, cpos);
        seq++;
    }
    kfree(targets);
    if (journal_write_super(j, 0, seq) != 0)
        return -1;
    j->start = 0;
    j->sequence = seq;
    return 0;
}

int journal_test_dirty(struct ext4_fs *fs, u64 block, const void *buf, usize len)
{
    struct journal *j;
    int ret;

    if (!fs || !fs->journal || !buf)
        return -1;
    j = (struct journal *)fs->journal;
    if (j->depth != 0 || j->pending_count != 0 || len != j->block_size)
        return -1;
    j->active = 1;
    if (journal_queue(fs, block, buf) != 0) {
        j->active = 0;
        return -1;
    }
    ret = journal_commit_internal(j, 1);
    j->active = 0;
    journal_free_pending(j);
    return ret;
}

int journal_open(struct ext4_fs *fs, int rw)
{
    struct journal *j;
    u64 count;
    u32 magic;
    u32 type;
    u32 bs;

    if (!fs || !(fs->feature_compat & 0x0004u))
        return -1;
    j = (struct journal *)kzalloc(sizeof(*j));
    if (!j)
        return -1;
    j->fs = fs;
    j->block_size = fs->block_size;
    count = ext4_journal_block_count(fs);
    if (count < 4 || count > 0x100000u) {
        kfree(j);
        return -1;
    }
    j->maxlen = (u32)count;
    j->sb = (u8 *)kmalloc(j->block_size);
    j->scratch = (u8 *)kmalloc(j->block_size);
    if (!j->sb || !j->scratch) {
        kfree(j->sb);
        kfree(j->scratch);
        kfree(j);
        return -1;
    }
    fs->journal = j;
    if (journal_read_log(j, 0, j->sb) != 0) {
        journal_close(fs);
        return -1;
    }
    magic = be32(j->sb);
    type = be32(j->sb + 4);
    bs = be32(j->sb + 12);
    if (magic != JOURNAL_MAGIC || type != JBD2_SUPERBLOCK_V2 ||
        bs != j->block_size || be32(j->sb + 16) != j->maxlen) {
        journal_close(fs);
        return -1;
    }
    j->first = be32(j->sb + 20);
    j->sequence = be32(j->sb + 24);
    j->start = be32(j->sb + 28);
    if (j->first < 1 || j->first >= j->maxlen ||
        (j->start != 0 && (j->start < j->first || j->start >= j->maxlen))) {
        journal_close(fs);
        return -1;
    }
    if (rw) {
        if (journal_recover(fs) != 0) {
            journal_close(fs);
            return -1;
        }
    } else if (j->start != 0) {
        journal_close(fs);
        return -1;
    }
    return 0;
}

void journal_close(struct ext4_fs *fs)
{
    struct journal *j;

    if (!fs || !fs->journal)
        return;
    j = (struct journal *)fs->journal;
    journal_free_pending(j);
    kfree(j->sb);
    kfree(j->scratch);
    kfree(j);
    fs->journal = 0;
}
