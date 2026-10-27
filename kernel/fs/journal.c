#include <crug/journal.h>
#include <crug/ext4.h>
#include <crug/block.h>
#include <crug/crc32c.h>
#include <crug/heap.h>
#include <crug/string.h>
#include <crug/kernel.h>
#include "ext4_internal.h"

#define JBD2_DESCRIPTOR_BLOCK 1u
#define JBD2_COMMIT_BLOCK 2u
#define JBD2_SUPERBLOCK_V2 4u
#define JBD2_FLAG_SAME_UUID 0x0002u
#define JBD2_FLAG_LAST_TAG 0x0008u
#define JBD2_INCOMPAT_REVOKE 0x0001u
#define JBD2_INCOMPAT_64BIT 0x0002u
#define JBD2_INCOMPAT_ASYNC_COMMIT 0x0004u
#define JBD2_INCOMPAT_CSUM_V2 0x0008u
#define JBD2_INCOMPAT_CSUM_V3 0x0010u
#define JBD2_COMPAT_CHECKSUM 0x0001u
#define JBD2_CRC32C_CHKSUM 4u
#define JOURNAL_SB_MAGIC_OFFSET 0x00u
#define JOURNAL_SB_COMPAT_OFFSET 0x24u
#define JOURNAL_SB_INCOMPAT_OFFSET 0x28u
#define JOURNAL_SB_UUID_OFFSET 0x30u
#define JOURNAL_SB_CSUM_TYPE_OFFSET 0x50u
#define JOURNAL_SB_CSUM_OFFSET 0xFCu
#define JOURNAL_SB_CSUM_LEN 0x400u
#define JOURNAL_MAX_PENDING 256u
#define JOURNAL_UUID_LEN 16u
#define JOURNAL_DESC_HDR_LEN 12u
#define JOURNAL_COMMIT_CSUM_OFFSET 0x10u
#define JOURNAL_MAX_REPLAY 4096u

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
    u32 csum_seed;
    u32 tag_bytes;
    int csum;
    int csum3;
    int has64;
    int skip_checkpoint;
    int active;
    struct journal_buf *pending;
    struct journal_buf *tail;
    u8 *sb;
    u8 *scratch;
};

/* Per-transaction replay mapping: home block, log position and the tag
   checksum that was recorded for the data block at that position. */
struct journal_txn {
    u64 block[JOURNAL_MAX_PENDING];
    u32 lpos[JOURNAL_MAX_PENDING];
    u32 csum[JOURNAL_MAX_PENDING];
    u32 count;
};

static u16 rd_be16(const u8 *p)
{
    return (u16)(((u32)p[0] << 8) | (u32)p[1]);
}

static u32 be32(const u8 *p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) |
           ((u32)p[2] << 8) | (u32)p[3];
}

static void put_be16(u8 *p, u16 v)
{
    p[0] = (u8)(v >> 8);
    p[1] = (u8)v;
}

static void put_be32(u8 *p, u32 v)
{
    p[0] = (u8)(v >> 24);
    p[1] = (u8)(v >> 16);
    p[2] = (u8)(v >> 8);
    p[3] = (u8)v;
}

/* Checksum of the journal superblock: crc32c over the 0x400-byte struct
   with s_checksum itself treated as zero. */
static u32 journal_sb_checksum(const u8 *sb)
{
    static const u8 zero[4] = { 0, 0, 0, 0 };
    u32 c = crc32c(CRC32C_SEED_ONES, sb, JOURNAL_SB_CSUM_OFFSET);

    c = crc32c(c, zero, 4);
    return crc32c(c, sb + JOURNAL_SB_CSUM_OFFSET + 4,
                  JOURNAL_SB_CSUM_LEN - JOURNAL_SB_CSUM_OFFSET - 4);
}

static u32 descr_csum_size(struct journal *j)
{
    return j->csum ? 4u : 0u;
}

/* Data-block tag checksum: the transaction sequence in big-endian order
   followed by the whole data block. */
static u32 journal_tag_checksum(struct journal *j, u32 seq, const u8 *data)
{
    u8 seq_bytes[4];

    put_be32(seq_bytes, seq);
    return crc32c(crc32c(j->csum_seed, seq_bytes, 4), data, j->block_size);
}

static u32 journal_put_tag(struct journal *j, u8 *desc, u32 off, u64 block,
                           u32 flags, u32 seq, const u8 *data)
{
    u32 csum = j->csum ? journal_tag_checksum(j, seq, data) : 0u;
    u32 high = (u32)(block >> 32);

    put_be32(desc + off, (u32)block);
    if (j->csum3) {
        put_be32(desc + off + 4, flags);
        put_be32(desc + off + 8, high);
        put_be32(desc + off + 12, csum);
    } else {
        if (j->csum)
            put_be16(desc + off + 4, (u16)csum);
        put_be16(desc + off + 6, (u16)flags);
        if (j->has64)
            put_be32(desc + off + 8, high);
    }
    return off + j->tag_bytes;
}

static u32 journal_tag_csum_field(struct journal *j, const u8 *desc, u32 off)
{
    if (!j->csum)
        return 0u;
    return j->csum3 ? be32(desc + off + 12) : (u32)rd_be16(desc + off + 4);
}

static void journal_put_descr_csum(struct journal *j, u8 *desc)
{
    u8 *tail = desc + j->block_size - 4;

    memset(tail, 0, 4);
    put_be32(tail, crc32c(j->csum_seed, desc, j->block_size));
}

static int journal_check_descr_csum(struct journal *j, u8 *desc, u32 pos)
{
    u8 *tail = desc + j->block_size - 4;
    u32 stored;
    u32 calc;

    if (!j->csum)
        return 0;
    stored = be32(tail);
    memset(tail, 0, 4);
    calc = crc32c(j->csum_seed, desc, j->block_size);
    put_be32(tail, stored);
    if (stored != calc) {
        kprintf("journal: descriptor checksum mismatch at %u\n", pos);
        return -1;
    }
    return 0;
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
    if (j->csum) {
        put_be32(sb + JOURNAL_SB_CSUM_OFFSET, 0);
        put_be32(sb + JOURNAL_SB_CSUM_OFFSET, journal_sb_checksum(sb));
    }
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
    /* Block 0 carries the ext4 superblock and is journaled with the rest
       of the transaction so that a replay restores the free counts. */
    if (block >= fs->blocks_count)
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
    u32 avail;

    if (j->tag_bytes == 0 || j->block_size < JOURNAL_DESC_HDR_LEN +
            JOURNAL_UUID_LEN + descr_csum_size(j))
        return 0;
    avail = j->block_size - JOURNAL_DESC_HDR_LEN - JOURNAL_UUID_LEN -
            descr_csum_size(j);
    return avail / j->tag_bytes;
}

static int journal_commit_internal(struct journal *j, int skip_checkpoint)
{
    struct journal_buf *b;
    struct journal_buf *d;
    const u8 *uuid = j->sb + JOURNAL_SB_UUID_OFFSET;
    u32 seq = j->sequence;
    u32 pos = j->first;
    u32 cap = descriptor_capacity(j);
    u32 left = j->pending_count;
    u32 total = j->pending_count;
    u32 chunks;
    u8 *desc = j->scratch;

    if (left == 0)
        return 0;
    if (cap == 0 || left > JOURNAL_MAX_PENDING)
        return -1;
    chunks = (left + cap - 1) / cap;
    if (chunks + left + 1 > j->maxlen - 1)
        return -1;
    if (block_flush(j->fs->bd) != 0)
        return -1;
    if (journal_write_super(j, j->first, seq) != 0)
        return -1;
    b = j->pending;
    d = j->pending;
    while (left > 0) {
        u32 count = left > cap ? cap : left;
        u32 off = JOURNAL_DESC_HDR_LEN;
        u32 i;
        int first_tag = 1;

        memset(desc, 0, j->block_size);
        put_be32(desc, JOURNAL_MAGIC);
        put_be32(desc + 4, JBD2_DESCRIPTOR_BLOCK);
        put_be32(desc + 8, seq);
        for (i = 0; i < count; i++) {
            u32 flags = 0;

            if (i + 1 == count)
                flags |= JBD2_FLAG_LAST_TAG;
            if (!first_tag)
                flags |= JBD2_FLAG_SAME_UUID;
            if (!b)
                return -1;
            off = journal_put_tag(j, desc, off, b->block, flags, seq,
                                  b->data);
            if (first_tag) {
                memcpy(desc + off, uuid, JOURNAL_UUID_LEN);
                off += JOURNAL_UUID_LEN;
                first_tag = 0;
            }
            b = b->next;
        }
        if (j->csum)
            journal_put_descr_csum(j, desc);
        if (journal_write_log(j, pos, desc) != 0)
            return -1;
        pos = journal_next(j, pos);
        for (i = 0; i < count; i++) {
            if (!d)
                return -1;
            if (journal_write_log(j, pos, d->data) != 0)
                return -1;
            pos = journal_next(j, pos);
            d = d->next;
        }
        left -= count;
    }
    memset(desc, 0, j->block_size);
    put_be32(desc, JOURNAL_MAGIC);
    put_be32(desc + 4, JBD2_COMMIT_BLOCK);
    put_be32(desc + 8, seq);
    if (j->csum)
        put_be32(desc + JOURNAL_COMMIT_CSUM_OFFSET,
                 crc32c(j->csum_seed, desc, j->block_size));
    if (journal_write_log(j, pos, desc) != 0)
        return -1;
    pos = journal_next(j, pos);
    if (block_flush(j->fs->bd) != 0)
        return -1;
    if (skip_checkpoint) {
        kprintf("CRUGOS: journal committed %u block(s) in %u descriptor(s)\n",
                total, chunks);
        journal_free_pending(j);
        return 0;
    }
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
    int skip;

    if (!fs || !fs->journal)
        return 0;
    j = (struct journal *)fs->journal;
    if (j->depth == 0)
        return 0;
    j->depth--;
    if (j->depth > 0)
        return 0;
    j->active = 0;
    skip = j->skip_checkpoint;
    j->skip_checkpoint = 0;
    return journal_commit_internal(j, skip);
}

/* Read the descriptors of one transaction, verifying every checksum, and
   fill map. Returns 1 when a validated commit block was reached, 0 when the
   log ended first (a partial transaction is discarded, as JBD2 does) and
   -1 on corruption. */
static int journal_read_txn(struct journal *j, u32 *pos_inout, u32 seq,
                            struct journal_txn *map)
{
    u32 pos = *pos_inout;
    u32 iter;

    map->count = 0;
    for (iter = 0; iter < JOURNAL_MAX_REPLAY; iter++) {
        u8 *desc = j->scratch;
        u32 dp;
        u32 off;
        int last = 0;
        int more = 0;

        if (journal_read_log(j, pos, desc) != 0)
            return -1;
        if (be32(desc) != JOURNAL_MAGIC ||
            be32(desc + 4) != JBD2_DESCRIPTOR_BLOCK ||
            be32(desc + 8) != seq)
            return 0;
        if (journal_check_descr_csum(j, desc, pos) != 0)
            return -1;
        dp = journal_next(j, pos);
        off = JOURNAL_DESC_HDR_LEN;
        for (;;) {
            u32 flags;
            u64 block;
            u32 toff;

            if (off + j->tag_bytes >
                j->block_size - descr_csum_size(j))
                return -1;
            toff = off;
            block = be32(desc + off);
            if (j->has64)
                block |= (u64)be32(desc + off + 8) << 32;
            flags = j->csum3 ? be32(desc + off + 4)
                             : (u32)rd_be16(desc + off + 6);
            off += j->tag_bytes;
            if (!(flags & JBD2_FLAG_SAME_UUID)) {
                if (off + JOURNAL_UUID_LEN > j->block_size)
                    return -1;
                off += JOURNAL_UUID_LEN;
            }
            if (block >= j->fs->blocks_count) {
                kprintf("journal: invalid log block %llu\n",
                        (unsigned long long)block);
                return -1;
            }
            if (map->count >= JOURNAL_MAX_PENDING) {
                kprintf("journal: transaction too large\n");
                return -1;
            }
            map->block[map->count] = block;
            map->lpos[map->count] = dp;
            map->csum[map->count] = journal_tag_csum_field(j, desc, toff);
            map->count++;
            dp = journal_next(j, dp);
            if (flags & JBD2_FLAG_LAST_TAG) {
                last = 1;
                break;
            }
        }
        if (!last)
            return -1;
        pos = dp;
        if (journal_read_log(j, pos, desc) != 0)
            return -1;
        if (be32(desc) == JOURNAL_MAGIC &&
            be32(desc + 4) == JBD2_DESCRIPTOR_BLOCK &&
            be32(desc + 8) == seq) {
            more = 1;
        } else if (be32(desc) == JOURNAL_MAGIC &&
                   be32(desc + 4) == JBD2_COMMIT_BLOCK &&
                   be32(desc + 8) == seq) {
            if (j->csum) {
                u32 stored = be32(desc + JOURNAL_COMMIT_CSUM_OFFSET);
                u32 calc;

                put_be32(desc + JOURNAL_COMMIT_CSUM_OFFSET, 0);
                calc = crc32c(j->csum_seed, desc, j->block_size);
                put_be32(desc + JOURNAL_COMMIT_CSUM_OFFSET, stored);
                if (stored != calc) {
                    kprintf("journal: commit checksum mismatch\n");
                    return -1;
                }
            }
            *pos_inout = pos;
            return 1;
        }
        if (!more)
            return 0;
    }
    return -1;
}

int journal_recover(struct ext4_fs *fs)
{
    struct journal *j;
    struct journal_txn *map;
    u32 pos;
    u32 seq;
    u32 iter;
    u32 i;

    if (!fs || !fs->journal)
        return -1;
    j = (struct journal *)fs->journal;
    if (j->start == 0)
        return 0;
    map = (struct journal_txn *)kmalloc(sizeof(*map));
    if (!map)
        return -1;
    pos = j->start;
    seq = j->sequence;
    for (iter = 0; iter < JOURNAL_MAX_REPLAY; iter++) {
        int ret;

        ret = journal_read_txn(j, &pos, seq, map);
        if (ret < 0) {
            kfree(map);
            return -1;
        }
        if (ret == 0) {
            /* End of the committed log; anything after it belongs to a
               transaction that never committed and is discarded. */
            break;
        }
        for (i = 0; i < map->count; i++) {
            if (journal_read_log(j, map->lpos[i], j->scratch) != 0) {
                kfree(map);
                return -1;
            }
            if (j->csum &&
                journal_tag_checksum(j, seq, j->scratch) != map->csum[i]) {
                kprintf("journal: data checksum mismatch\n");
                kfree(map);
                return -1;
            }
        }
        for (i = 0; i < map->count; i++) {
            if (journal_read_log(j, map->lpos[i], j->scratch) != 0) {
                kfree(map);
                return -1;
            }
            if (ext4_raw_write_block(j->fs, map->block[i],
                                     j->scratch) != 0) {
                kfree(map);
                return -1;
            }
            j->recovered++;
        }
        pos = journal_next(j, pos);
        seq++;
    }
    kfree(map);
    if (journal_write_super(j, 0, seq) != 0)
        return -1;
    j->start = 0;
    j->sequence = seq;
    return 0;
}

int journal_test_dirty(struct ext4_fs *fs, u64 block, const void *buf,
                       usize len)
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

int journal_test_skip_checkpoint(struct ext4_fs *fs, int on)
{
    struct journal *j;

    if (!fs || !fs->journal)
        return -1;
    j = (struct journal *)fs->journal;
    j->skip_checkpoint = on ? 1 : 0;
    return 0;
}

int journal_open(struct ext4_fs *fs, int rw)
{
    struct journal *j;
    u64 count;
    u32 magic;
    u32 type;
    u32 bs;
    u32 incompat;
    u32 compat;

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
    incompat = be32(j->sb + JOURNAL_SB_INCOMPAT_OFFSET);
    compat = be32(j->sb + JOURNAL_SB_COMPAT_OFFSET);
    /* Revoke and async-commit journals need features this writer does
       not implement; refuse them instead of corrupting the log. */
    if (incompat & (JBD2_INCOMPAT_REVOKE | JBD2_INCOMPAT_ASYNC_COMMIT)) {
        journal_close(fs);
        return -1;
    }
    j->csum3 = (incompat & JBD2_INCOMPAT_CSUM_V3) != 0;
    if (incompat & JBD2_INCOMPAT_CSUM_V2) {
        if (j->csum3) {
            journal_close(fs);
            return -1;
        }
        j->csum = 1;
    }
    if (incompat & JBD2_INCOMPAT_CSUM_V3)
        j->csum = 1;
    if (j->csum && (compat & JBD2_COMPAT_CHECKSUM)) {
        journal_close(fs);
        return -1;
    }
    j->has64 = (incompat & JBD2_INCOMPAT_64BIT) != 0;
    j->tag_bytes = j->csum3 ? 16u
                 : 8u + (j->csum ? 2u : 0u) + (j->has64 ? 4u : 0u);
    if (j->csum) {
        if (j->sb[JOURNAL_SB_CSUM_TYPE_OFFSET] != JBD2_CRC32C_CHKSUM ||
            be32(j->sb + JOURNAL_SB_CSUM_OFFSET) !=
                journal_sb_checksum(j->sb)) {
            journal_close(fs);
            return -1;
        }
        j->csum_seed = crc32c(CRC32C_SEED_ONES,
                              j->sb + JOURNAL_SB_UUID_OFFSET,
                              JOURNAL_UUID_LEN);
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
