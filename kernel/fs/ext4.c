#include <crug/ext4.h>
#include <crug/string.h>
#include <crug/heap.h>
#include <crug/bitmap.h>
#include <crug/crc32c.h>
#include <crug/kernel.h>
#include <crug/journal.h>
#include <crug/harden.h>
#include "ext4_inode.h"
#include "ext4_internal.h"

#define EXT4_MAGIC 0xEF53u
#define EXT4_EXT_MAGIC 0xF30Au

/* An inline extent root lives inside the 60-byte i_block area; a leaf
   node occupies a whole filesystem block with a 12-byte header in front. */
#define EXT4_INLINE_EXTENT_BYTES 60u
#define EXT4_MAX_EXTENT_DEPTH 5u

#define EXT4_FEATURE_COMPAT_HAS_JOURNAL 0x0004u
#define EXT4_FEATURE_COMPAT_DIR_INDEX 0x0040u
#define EXT4_FEATURE_INCOMPAT_FILETYPE 0x0002u
#define EXT4_FEATURE_INCOMPAT_EXTENTS 0x0040u
#define EXT4_FEATURE_INCOMPAT_64BIT 0x0080u
#define EXT4_FEATURE_INCOMPAT_FLEX_BG 0x0200u
#define EXT4_FEATURE_INCOMPAT_CSUM_SEED 0x2000u
#define EXT4_FEATURE_INCOMPAT_RECOVER 0x0004u
#define EXT4_FEATURE_RO_SPARSE_SUPER 0x0001u
#define EXT4_FEATURE_RO_LARGE_FILE 0x0002u
#define EXT4_FEATURE_RO_HUGE_FILE 0x0008u
#define EXT4_FEATURE_RO_DIR_NLINK 0x0020u
#define EXT4_FEATURE_RO_EXTRA_ISIZE 0x0040u
#define EXT4_FEATURE_RO_METADATA_CSUM 0x0400u

#define EXT4_BG_INODE_UNINIT 0x0001u
#define EXT4_BG_BLOCK_UNINIT 0x0002u

#define EXT4_SB_CHECKSUM_OFFSET 0x3FCu
#define EXT4_SB_UUID_OFFSET 0x68u
#define EXT4_SB_CSUM_SEED_OFFSET 0x270u
#define EXT4_INODE_GENERATION_OFFSET 0x64u
#define EXT4_INODE_CSUM_LO_OFFSET 0x7Cu
#define EXT4_INODE_CSUM_HI_OFFSET 0x82u
#define EXT4_DIR_TAIL_SIZE 12u

static u16 rd16(const u8 *p)
{
    return (u16)p[0] | (u16)((u16)p[1] << 8);
}

static u32 rd32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) |
           ((u32)p[3] << 24);
}

static void wr16(u8 *p, u16 v)
{
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
}

static void wr32(u8 *p, u32 v)
{
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
    p[2] = (u8)(v >> 16);
    p[3] = (u8)(v >> 24);
}

/* ---- metadata_csum ------------------------------------------------- */

static u32 crc32c_le32(u32 seed, u32 value)
{
    u8 b[4];

    b[0] = (u8)(value & 0xFFu);
    b[1] = (u8)((value >> 8) & 0xFFu);
    b[2] = (u8)((value >> 16) & 0xFFu);
    b[3] = (u8)((value >> 24) & 0xFFu);
    return crc32c(seed, b, 4);
}

static int ext4_has_csum(struct ext4_fs *fs)
{
    return (fs->feature_ro_compat & EXT4_FEATURE_RO_METADATA_CSUM) != 0;
}

static u32 ext4_sb_checksum(const u8 *sb)
{
    return crc32c(CRC32C_SEED_ONES, sb, EXT4_SB_CHECKSUM_OFFSET);
}

static u32 ext4_gdt_checksum(struct ext4_fs *fs, u32 group, const u8 *desc)
{
    u32 crc = crc32c_le32(fs->csum_seed, group);

    return crc32c(crc, desc, fs->desc_size);
}

static void ext4_gdt_set_csum(struct ext4_fs *fs, u32 group, u8 *desc)
{
    if (!ext4_has_csum(fs))
        return;
    wr16(desc + 0x1E, 0);
    wr16(desc + 0x1E, (u16)(ext4_gdt_checksum(fs, group, desc) & 0xFFFFu));
}

static int ext4_gdt_check_csum(struct ext4_fs *fs, u32 group, const u8 *desc)
{
    u8 copy[64];
    u32 crc;

    if (!ext4_has_csum(fs))
        return 0;
    if (fs->desc_size > sizeof(copy))
        return -1;
    memcpy(copy, desc, fs->desc_size);
    wr16(copy + 0x1E, 0);
    crc = ext4_gdt_checksum(fs, group, copy) & 0xFFFFu;
    return rd16(desc + 0x1E) == (u16)crc ? 0 : -1;
}

static u32 ext4_inode_seed(struct ext4_fs *fs, u32 ino, const u8 *raw)
{
    u32 crc = crc32c_le32(fs->csum_seed, ino);

    return crc32c_le32(crc, rd32(raw + EXT4_INODE_GENERATION_OFFSET));
}

static void ext4_inode_set_csum(struct ext4_fs *fs, u32 ino, u8 *raw)
{
    u32 crc;

    if (!ext4_has_csum(fs))
        return;
    wr16(raw + EXT4_INODE_CSUM_LO_OFFSET, 0);
    if (fs->inode_size >= EXT4_INODE_CSUM_HI_OFFSET + 2)
        wr16(raw + EXT4_INODE_CSUM_HI_OFFSET, 0);
    crc = crc32c(ext4_inode_seed(fs, ino, raw), raw, fs->inode_size);
    wr16(raw + EXT4_INODE_CSUM_LO_OFFSET, (u16)(crc & 0xFFFFu));
    if (fs->inode_size >= EXT4_INODE_CSUM_HI_OFFSET + 2)
        wr16(raw + EXT4_INODE_CSUM_HI_OFFSET, (u16)(crc >> 16));
}

static int ext4_inode_check_csum(struct ext4_fs *fs, u32 ino, const u8 *raw)
{
    u32 crc;
    u32 stored;
    u8 *copy;
    int ret;

    if (!ext4_has_csum(fs))
        return 0;
    copy = (u8 *)kmalloc(fs->inode_size);
    if (!copy)
        return -1;
    memcpy(copy, raw, fs->inode_size);
    stored = rd16(raw + EXT4_INODE_CSUM_LO_OFFSET);
    if (fs->inode_size >= EXT4_INODE_CSUM_HI_OFFSET + 2)
        stored |= (u32)rd16(raw + EXT4_INODE_CSUM_HI_OFFSET) << 16;
    wr16(copy + EXT4_INODE_CSUM_LO_OFFSET, 0);
    if (fs->inode_size >= EXT4_INODE_CSUM_HI_OFFSET + 2)
        wr16(copy + EXT4_INODE_CSUM_HI_OFFSET, 0);
    crc = crc32c(ext4_inode_seed(fs, ino, copy), copy, fs->inode_size);
    ret = (crc == stored) ? 0 : -1;
    kfree(copy);
    return ret;
}

static usize ext4_dir_usable(struct ext4_fs *fs)
{
    return ext4_has_csum(fs) ? fs->block_size - EXT4_DIR_TAIL_SIZE
                             : fs->block_size;
}

static void ext4_dir_set_tail(struct ext4_fs *fs, u32 ino, u32 generation,
                              u8 *block)
{
    u8 *tail;
    u32 crc;

    if (!ext4_has_csum(fs))
        return;
    tail = block + fs->block_size - EXT4_DIR_TAIL_SIZE;
    wr32(tail + 0, 0);
    wr16(tail + 4, (u16)EXT4_DIR_TAIL_SIZE);
    tail[6] = 0;
    tail[7] = 0xDE;
    crc = crc32c_le32(crc32c_le32(fs->csum_seed, ino), generation);
    crc = crc32c(crc, block, fs->block_size - EXT4_DIR_TAIL_SIZE);
    wr32(tail + 8, crc);
}

static int ext4_dir_check_tail(struct ext4_fs *fs, u32 ino, u32 generation,
                               const u8 *block)
{
    const u8 *tail;
    u32 crc;

    if (!ext4_has_csum(fs))
        return 0;
    tail = block + fs->block_size - EXT4_DIR_TAIL_SIZE;
    if (rd32(tail + 0) != 0 || rd16(tail + 4) != EXT4_DIR_TAIL_SIZE ||
        tail[6] != 0 || tail[7] != 0xDE)
        return -1;
    crc = crc32c_le32(crc32c_le32(fs->csum_seed, ino), generation);
    crc = crc32c(crc, block, fs->block_size - EXT4_DIR_TAIL_SIZE);
    return crc == rd32(tail + 8) ? 0 : -1;
}

static u32 extent_tail_offset(const struct ext4_extent_header *h)
{
    return (u32)(sizeof(*h) + (u32)h->max * sizeof(struct ext4_extent));
}

static void extent_block_set_csum(struct ext4_fs *fs, u32 ino,
                                  const struct ext4_inode *inode, u8 *block)
{
    struct ext4_extent_header *h =
        (struct ext4_extent_header *)(void *)block;
    u32 off;
    u32 seed;
    u32 crc;

    if (!ext4_has_csum(fs))
        return;
    off = extent_tail_offset(h);
    if (off + 4 > fs->block_size)
        return;
    seed = crc32c_le32(crc32c_le32(fs->csum_seed, ino), inode->generation);
    crc = crc32c(seed, block, off);
    wr32(block + off, crc);
}

static int extent_block_check_csum(struct ext4_fs *fs, u32 ino,
                                   const struct ext4_inode *inode,
                                   const u8 *block)
{
    const struct ext4_extent_header *h =
        (const struct ext4_extent_header *)(const void *)block;
    u32 off;
    u32 seed;
    u32 crc;

    if (!ext4_has_csum(fs))
        return 0;
    off = extent_tail_offset(h);
    if (off + 4 > fs->block_size)
        return -1;
    seed = crc32c_le32(crc32c_le32(fs->csum_seed, ino), inode->generation);
    crc = crc32c(seed, block, off);
    return crc == rd32(block + off) ? 0 : -1;
}

static int read_block(struct ext4_fs *fs, u64 block, void *buf)
{
    if (block == 0)
        return -1;
    if (fs->journal && journal_lookup(fs, block, buf))
        return 0;
    return block_read(fs->bd, block * fs->sectors_per_block,
                      fs->sectors_per_block, buf);
}

static int write_data_block(struct ext4_fs *fs, u64 block, const void *buf)
{
    if (block == 0)
        return -1;
    return block_write(fs->bd, block * fs->sectors_per_block,
                       fs->sectors_per_block, buf);
}

static int read_group_desc(struct ext4_fs *fs, u32 group, u8 *desc)
{
    u8 *buf;
    u64 offset;
    u64 block;
    u32 in_block;
    int ret = -1;

    if (group >= fs->groups)
        return -1;
    offset = (u64)group * fs->desc_size;
    block = fs->gdt_block + offset / fs->block_size;
    in_block = (u32)(offset % fs->block_size);
    if (in_block + fs->desc_size > fs->block_size)
        return -1;
    if (fs->gdt) {
        memcpy(desc, fs->gdt + offset, fs->desc_size);
        return 0;
    }
    buf = (u8 *)kmalloc(fs->block_size);
    if (!buf)
        return -1;
    if (read_block(fs, block, buf) == 0) {
        memcpy(desc, buf + in_block, fs->desc_size);
        ret = 0;
    }
    kfree(buf);
    return ret;
}

static int read_inode(struct ext4_fs *fs, u32 ino, struct ext4_inode *inode)
{
    u8 desc[64];
    u8 *buf;
    u32 group;
    u32 index;
    u64 table;
    u64 offset;
    u64 block;
    u32 in_block;
    int ret = -1;

    if (ino == 0 || ino > fs->inodes_count)
        return -1;
    group = (ino - 1) / fs->inodes_per_group;
    index = (ino - 1) % fs->inodes_per_group;
    if (read_group_desc(fs, group, desc) != 0)
        return -1;
    table = rd32(desc + 8);
    if (fs->desc_size >= 64)
        table |= (u64)rd16(desc + 0x28) << 32;
    offset = (u64)index * fs->inode_size;
    block = table + offset / fs->block_size;
    in_block = (u32)(offset % fs->block_size);
    if (in_block + fs->inode_size > fs->block_size)
        return -1;
    buf = (u8 *)kmalloc(fs->block_size);
    if (!buf)
        return -1;
    if (read_block(fs, block, buf) == 0 &&
        ext4_inode_check_csum(fs, ino, buf + in_block) == 0) {
        memset(inode, 0, sizeof(*inode));
        /* Only the first 128 bytes of the inode are modelled below.
           ext4 inodes may be larger (256+ bytes); copying inode_size
           overflows this struct and corrupts the caller's stack. */
        memcpy(inode, buf + in_block, sizeof(*inode));
        ret = 0;
    }
    kfree(buf);
    return ret;
}

static int extent_map(struct ext4_fs *fs, const u8 *node, u32 node_bytes,
                      u32 lblk, u64 *phys, int depth)
{
    const struct ext4_extent_header *h;
    u32 i;

    if (depth > EXT4_MAX_EXTENT_DEPTH)
        return -1;
    h = (const struct ext4_extent_header *)node;
    /* node_bytes is what actually exists at `node`: 60 bytes for the inline
       i_block root, block_size - 12 for an on-disk leaf block. Validating
       against it (not just block_size) stops a crafted inline root from
       making the entry loop read past the inode. */
    if (!harden_extent_header_ok(node, node_bytes))
        return -1;
    if (h->depth == 0) {
        for (i = 0; i < h->entries; i++) {
            const struct ext4_extent *e =
                (const struct ext4_extent *)(node + 12 + i * 12);
            u32 start = e->ee_block;
            u32 len = e->ee_len & 0x7FFFu;

            if (len == 0)
                continue;
            if (lblk < start || lblk >= start + len)
                continue;
            if (e->ee_len & 0x8000u)
                return 1;
            *phys = ((u64)e->ee_start_hi << 32) | e->ee_start_lo;
            *phys += (u64)(lblk - start);
            return 0;
        }
        return 1;
    }
    for (i = 0; i < h->entries; i++) {
        const struct ext4_extent_idx *idx =
            (const struct ext4_extent_idx *)(node + 12 + i * 12);
        const struct ext4_extent_idx *next =
            (i + 1 < h->entries)
                ? (const struct ext4_extent_idx *)(node + 12 + (i + 1) * 12)
                : 0;
        u64 child;
        u8 *buf;
        int ret;

        if (lblk < idx->ei_block)
            return 1;
        if (next && lblk >= next->ei_block)
            continue;
        child = ((u64)idx->ei_leaf_hi << 32) | idx->ei_leaf_lo;
        buf = (u8 *)kmalloc(fs->block_size);
        if (!buf)
            return -1;
        if (read_block(fs, child, buf) != 0) {
            kfree(buf);
            return -1;
        }
        ret = extent_map(fs, buf, fs->block_size - 12u, lblk, phys,
                         depth + 1);
        kfree(buf);
        return ret;
    }
    return 1;
}

static int bmap(struct ext4_fs *fs, const struct ext4_inode *inode, u32 lblk,
                u64 *phys)
{
    u32 ptrs = fs->block_size / 4u;
    u8 *buf;
    u64 block;
    int ret;

    if (inode->flags & EXT4_EXTENTS_FL)
        return extent_map(fs, (const u8 *)inode->block,
                          EXT4_INLINE_EXTENT_BYTES, lblk, phys, 0);
    if (lblk < 12) {
        if (inode->block[lblk] == 0)
            return 1;
        *phys = inode->block[lblk];
        return 0;
    }
    if (lblk < 12 + ptrs) {
        u32 idx = lblk - 12;

        if (inode->block[12] == 0)
            return 1;
        buf = (u8 *)kmalloc(fs->block_size);
        if (!buf)
            return -1;
        if (read_block(fs, inode->block[12], buf) != 0) {
            kfree(buf);
            return -1;
        }
        block = rd32(buf + idx * 4);
        kfree(buf);
        if (block == 0)
            return 1;
        *phys = block;
        return 0;
    }
    ret = -1;
    return ret;
}

int ext4_raw_read_block(struct ext4_fs *fs, u64 block, void *buf)
{
    if (!fs || block == 0 || block >= fs->blocks_count)
        return -1;
    return block_read(fs->bd, block * fs->sectors_per_block,
                      fs->sectors_per_block, buf);
}

/* Block 0 is a real filesystem block (the superblock), so unlike
   ext4_raw_read_block this accepts it; the journal checkpoints the
   superblock through here. */
int ext4_raw_write_block(struct ext4_fs *fs, u64 block, const void *buf)
{
    if (!fs || block >= fs->blocks_count)
        return -1;
    return block_write(fs->bd, block * fs->sectors_per_block,
                       fs->sectors_per_block, buf);
}

int ext4_journal_map_block(struct ext4_fs *fs, u64 logical, u64 *phys)
{
    struct ext4_inode inode;
    int ret;

    if (!fs || !phys || read_inode(fs, 8, &inode) != 0)
        return -1;
    ret = bmap(fs, &inode, (u32)logical, phys);
    return ret == 0 ? 0 : -1;
}

u64 ext4_journal_block_count(struct ext4_fs *fs)
{
    struct ext4_inode inode;
    u64 size;

    if (!fs || read_inode(fs, 8, &inode) != 0)
        return 0;
    size = (u64)inode.size_lo | ((u64)inode.size_high << 32);
    return size / fs->block_size;
}

int ext4_file_block(struct ext4_fs *fs, const char *path, u32 lblk, u64 *phys)
{
    struct ext4_inode inode;
    u32 ino;
    int ret;

    if (!fs || !path || !phys || ext4_lookup(fs, path, &ino) != 0)
        return -1;
    if (read_inode(fs, ino, &inode) != 0)
        return -1;
    ret = bmap(fs, &inode, lblk, phys);
    return ret == 0 ? 0 : -1;
}

static int dir_walk(struct ext4_fs *fs, u32 ino, const char *target,
                    char *list, usize cap, usize *used)
{
    struct ext4_inode inode;
    u8 *buf;
    u64 size;
    u64 pos;
    u32 limit;
    int found = 0;
    int failed = 0;

    if (read_inode(fs, ino, &inode) != 0)
        return -1;
    if ((inode.mode & 0xF000u) != 0x4000u)
        return -1;
    size = (u64)inode.size_lo | ((u64)inode.size_high << 32);
    limit = (u32)ext4_dir_usable(fs);
    buf = (u8 *)kmalloc(fs->block_size);
    if (!buf)
        return -1;
    for (pos = 0; pos < size; pos += fs->block_size) {
        u64 phys = 0;
        u32 off;
        int ret = bmap(fs, &inode, (u32)(pos / fs->block_size), &phys);

        if (ret < 0) {
            failed = 1;
            break;
        }
        if (ret > 0)
            continue;
        if (read_block(fs, phys, buf) != 0) {
            failed = 1;
            break;
        }
        if (ext4_dir_check_tail(fs, ino, inode.generation, buf) != 0) {
            failed = 1;
            break;
        }
        off = 0;
        while (off + 8 <= limit && pos + off + 8 <= size) {
            u32 entry = rd32(buf + off);
            u16 rec = rd16(buf + off + 4);
            u32 name_len;
            const char *name;

            if (rec < 8 || (rec & 3u) != 0 || off + rec > limit)
                break;
            if (fs->feature_incompat & EXT4_FEATURE_INCOMPAT_FILETYPE) {
                name_len = buf[off + 6];
                name = (const char *)(buf + off + 8);
            } else {
                name_len = rd16(buf + off + 6);
                name = (const char *)(buf + off + 8);
            }
            if (name_len > rec - 8u)
                break;
            if (entry != 0 && name_len > 0) {
                if (target) {
                    if (strlen(target) == name_len &&
                        memcmp(name, target, name_len) == 0) {
                        found = entry;
                        kfree(buf);
                        return found;
                    }
                } else if (list && used) {
                    if (*used + name_len + 1 < cap) {
                        memcpy(list + *used, name, name_len);
                        *used += name_len;
                        list[(*used)++] = '\n';
                    }
                }
            }
            off += rec;
        }
    }
    kfree(buf);
    if (failed)
        return -1;
    if (target)
        return 0;
    if (list && used && *used < cap)
        list[*used] = 0;
    return 0;
}

static int ext4_probe(struct ext4_fs *fs, struct block_device *bd)
{
    u8 sb[1024];
    u32 log_block;
    u32 incompat;
    u32 compat;
    u32 ro;
    u32 blocks_hi;
    u16 state;
    struct ext4_inode root;

    if (!fs || !bd)
        return -1;
    memset(fs, 0, sizeof(*fs));
    if (block_read(bd, 2, 2, sb) != 0)
        return -1;
    if (rd16(sb + 0x38) != EXT4_MAGIC)
        return -1;
    /* EXT2_VALID_FS: a clean filesystem has state 1. A journaled
       filesystem may be state 0 after a crash; mount recovery repairs
       it before the filesystem is used. */
    state = rd16(sb + 0x3A);
    if (state != 1 && state != 0)
        return -1;
    log_block = rd32(sb + 0x18);
    if (log_block > 2)
        return -1;
    fs->bd = bd;
    fs->block_size = 1024u << log_block;
    fs->sectors_per_block = fs->block_size / 512u;
    fs->blocks_count = rd32(sb + 0x04);
    blocks_hi = rd32(sb + 0x150);
    fs->inodes_count = rd32(sb + 0x00);
    fs->first_data_block = rd32(sb + 0x14);
    fs->blocks_per_group = rd32(sb + 0x20);
    fs->inodes_per_group = rd32(sb + 0x28);
    fs->inode_size = rd32(sb + 0x58);
    fs->feature_compat = rd32(sb + 0x5C);
    fs->feature_incompat = rd32(sb + 0x60);
    fs->feature_ro_compat = rd32(sb + 0x64);
    fs->desc_size = (fs->feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT)
                        ? rd16(sb + 0xFE)
                        : 32u;
    fs->root_ino = EXT4_ROOT_INO;
    if (fs->inode_size == 0)
        fs->inode_size = 128;
    if (fs->inode_size < 128 || fs->inode_size > fs->block_size ||
        (fs->inode_size & (fs->inode_size - 1)) != 0)
        return -1;
    if (fs->desc_size != 32 && fs->desc_size != 64)
        return -1;
    /* read_group_desc() copies into a 64-byte descriptor buffer. */
    if (blocks_hi != 0)
        return -1;
    /* Group geometry becomes a bitmap index and a checksum length later on;
       a crafted superblock with an absurd group size would read past the
       4 KiB bitmap or index the wrong group. Bound it at mount time. */
    if (!harden_ext4_geometry_ok(fs->block_size, fs->blocks_per_group,
                                 fs->inodes_per_group, fs->blocks_count,
                                 fs->inodes_count)) {
        kprintf("CRUGOS: ext4 mount rejected (geometry)\n");
        return -1;
    }
    if (fs->first_data_block != (fs->block_size == 1024u ? 1u : 0u))
        return -1;
    incompat = fs->feature_incompat;
    if (incompat & ~(EXT4_FEATURE_INCOMPAT_FILETYPE |
                     EXT4_FEATURE_INCOMPAT_EXTENTS |
                     EXT4_FEATURE_INCOMPAT_64BIT |
                     EXT4_FEATURE_INCOMPAT_FLEX_BG |
                     EXT4_FEATURE_INCOMPAT_CSUM_SEED |
                     EXT4_FEATURE_INCOMPAT_RECOVER))
        return -1;
    compat = fs->feature_compat;
    if (compat & EXT4_FEATURE_COMPAT_DIR_INDEX)
        return -1;
    ro = fs->feature_ro_compat;
    if (ro & ~(EXT4_FEATURE_RO_SPARSE_SUPER | EXT4_FEATURE_RO_LARGE_FILE |
               EXT4_FEATURE_RO_HUGE_FILE | EXT4_FEATURE_RO_DIR_NLINK |
               EXT4_FEATURE_RO_EXTRA_ISIZE | EXT4_FEATURE_RO_METADATA_CSUM))
        return -1;
    /* needs_recovery is an incompat flag (EXT3_FEATURE_INCOMPAT_RECOVER),
       and it is only meaningful when the filesystem has a journal. */
    if ((incompat & EXT4_FEATURE_INCOMPAT_RECOVER) &&
        !(compat & EXT4_FEATURE_COMPAT_HAS_JOURNAL))
        return -1;
    fs->state = state;
    fs->groups = (fs->blocks_count + fs->blocks_per_group - 1) /
                 fs->blocks_per_group;
    fs->gdt_block = (u64)fs->first_data_block + 1;
    if (ro & EXT4_FEATURE_RO_METADATA_CSUM) {
        u32 stored = rd32(sb + EXT4_SB_CHECKSUM_OFFSET);

        if (stored != ext4_sb_checksum(sb))
            return -1;
        fs->csum_seed = (incompat & EXT4_FEATURE_INCOMPAT_CSUM_SEED)
                            ? rd32(sb + EXT4_SB_CSUM_SEED_OFFSET)
                            : crc32c(CRC32C_SEED_ONES,
                                     sb + EXT4_SB_UUID_OFFSET, 16);
    }
    if (read_inode(fs, fs->root_ino, &root) != 0)
        return -1;
    if ((root.mode & 0xF000u) != 0x4000u)
        return -1;
    /* The root inode is read during mount and used immediately; a crafted
       inline extent root must be rejected before its entries are walked. */
    if ((root.flags & EXT4_EXTENTS_FL) &&
        !harden_extent_header_ok((const u8 *)(const void *)root.block,
                                 EXT4_INLINE_EXTENT_BYTES)) {
        kprintf("CRUGOS: ext4 mount rejected (root extent)\n");
        return -1;
    }
    return 0;
}

static void ext4_release(struct ext4_fs *fs)
{
    if (fs) {
        journal_close(fs);
        kfree(fs->gdt);
        memset(fs, 0, sizeof(*fs));
    }
}

int ext4_lookup(struct ext4_fs *fs, const char *path, u32 *ino)
{
    u32 current = EXT4_ROOT_INO;
    char name[256];

    if (!fs || !path || !ino)
        return -1;
    while (*path == '/')
        path++;
    while (*path) {
        usize len = 0;

        while (*path && *path != '/') {
            if (len + 1 >= sizeof(name))
                return -1;
            name[len++] = *path++;
        }
        name[len] = 0;
        while (*path == '/')
            path++;
        if (len == 0)
            continue;
        {
            int found = dir_walk(fs, current, name, 0, 0, 0);
            if (found <= 0)
                return -1;
            current = (u32)found;
        }
    }
    *ino = current;
    return 0;
}

int ext4_read_file(struct ext4_fs *fs, const char *path, void *buf, usize cap,
                   u64 *out_size)
{
    struct ext4_inode inode;
    u32 ino;
    u64 size;
    u64 copied = 0;
    u8 *block;

    if (!fs || !buf || !out_size)
        return -1;
    if (ext4_lookup(fs, path, &ino) != 0)
        return -1;
    if (read_inode(fs, ino, &inode) != 0)
        return -1;
    if ((inode.mode & 0xF000u) != 0x8000u)
        return -1;
    size = (u64)inode.size_lo | ((u64)inode.size_high << 32);
    *out_size = size;
    if (size > cap)
        size = cap;
    block = (u8 *)kmalloc(fs->block_size);
    if (!block)
        return -1;
    while (copied < size) {
        u32 lblk = (u32)(copied / fs->block_size);
        u32 offset = (u32)(copied % fs->block_size);
        u64 phys = 0;
        usize chunk = fs->block_size - offset;
        int ret;

        if (chunk > size - copied)
            chunk = (usize)(size - copied);
        ret = bmap(fs, &inode, lblk, &phys);
        if (ret < 0) {
            kfree(block);
            return -1;
        }
        if (ret == 0) {
            if (read_block(fs, phys, block) != 0) {
                kfree(block);
                return -1;
            }
            memcpy((u8 *)buf + copied, block + offset, chunk);
        } else {
            memset((u8 *)buf + copied, 0, chunk);
        }
        copied += chunk;
    }
    kfree(block);
    return 0;
}

int ext4_list(struct ext4_fs *fs, const char *path, char *out, usize cap)
{
    u32 ino;
    usize used = 0;

    if (!fs || !out || cap == 0)
        return -1;
    if (ext4_lookup(fs, path, &ino) != 0)
        return -1;
    out[0] = 0;
    if (dir_walk(fs, ino, 0, out, cap, &used) != 0)
        return -1;
    return (int)used;
}

int ext4_selftest(void)
{
    if (sizeof(struct ext4_inode) != 128)
        return 0;
    if (sizeof(struct ext4_extent_header) != 12)
        return 0;
    if (sizeof(struct ext4_extent) != 12)
        return 0;
    return 1;
}

/* ---- writable ext4 ------------------------------------------------ */

#define EXT4_FT_REG_FILE 1u
#define EXT4_FT_DIR      2u
#define EXT4_S_IFREG 0100000u
#define EXT4_S_IFDIR 0040000u
#define EXT4_INODE_EXTRA_ISIZE 32u

static int write_block(struct ext4_fs *fs, u64 block, const void *buf)
{
    /* Block 0 is the superblock: only a journaled transaction may queue it,
       because the free counters in it must be committed atomically with the
       bitmap blocks they describe. */
    if (block == 0 && !journal_is_active(fs))
        return -1;
    if (journal_is_active(fs))
        return journal_queue(fs, block, buf);
    return write_data_block(fs, block, buf);
}

static int read_super(struct ext4_fs *fs, u8 *sb)
{
    return block_read(fs->bd, 2, 2, sb);
}

/* Copy the in-memory allocation counters into sb and refresh the
   superblock checksum. */
static void ext4_sb_prepare(struct ext4_fs *fs, u8 *sb)
{
    wr32(sb + 0x0C, fs->free_blocks);
    wr32(sb + 0x10, fs->free_inodes);
    wr32(sb + 0x2C, 0);
    wr32(sb + 0x30, 0);
    if (ext4_has_csum(fs))
        wr32(sb + EXT4_SB_CHECKSUM_OFFSET, ext4_sb_checksum(sb));
}

/* Queue the superblock into the running transaction so a replay restores
   the free counters that belong to the committed bitmap and inode blocks. */
static int sb_journal(struct ext4_fs *fs)
{
    u8 sb[1024];

    if (!fs->journal || !journal_is_active(fs))
        return 0;
    if (read_super(fs, sb) != 0)
        return -1;
    ext4_sb_prepare(fs, sb);
    return journal_queue(fs, 0, sb);
}

static int write_super_state(struct ext4_fs *fs, u16 state)
{
    u8 sb[1024];

    if (read_super(fs, sb) != 0)
        return -1;
    wr16(sb + 0x3A, state);
    /* A mount that reaches this point has replayed the journal, and a clean
       unmount leaves a clean one, so NEEDS_RECOVERY never survives us. */
    wr32(sb + 0x60, rd32(sb + 0x60) & ~EXT4_FEATURE_INCOMPAT_RECOVER);
    ext4_sb_prepare(fs, sb);
    return block_write(fs->bd, 2, 2, sb);
}

int ext4_test_mark_dirty(struct ext4_fs *fs)
{
    u8 sb[1024];

    if (!fs || !fs->bd)
        return -1;
    if (read_super(fs, sb) != 0)
        return -1;
    wr16(sb + 0x3A, 0);
    wr32(sb + 0x60, rd32(sb + 0x60) | EXT4_FEATURE_INCOMPAT_RECOVER);
    ext4_sb_prepare(fs, sb);
    return block_write(fs->bd, 2, 2, sb);
}

static int load_gdt(struct ext4_fs *fs)
{
    u64 bytes;
    u64 blocks;
    u64 i;

    if (fs->groups == 0)
        return -1;
    bytes = (u64)fs->groups * fs->desc_size;
    blocks = (bytes + fs->block_size - 1) / fs->block_size;
    fs->gdt = (u8 *)kmalloc((usize)(blocks * fs->block_size));
    if (!fs->gdt)
        return -1;
    memset(fs->gdt, 0, (usize)(blocks * fs->block_size));
    for (i = 0; i < blocks; i++) {
        u32 g;
        u8 *base = fs->gdt + i * fs->block_size;

        if (read_block(fs, fs->gdt_block + i, base) != 0)
            return -1;
        for (g = 0; g < fs->groups; g++) {
            u64 off = (u64)g * fs->desc_size;

            if (off / fs->block_size != i)
                continue;
            if (ext4_gdt_check_csum(fs, g, base + (off % fs->block_size)) != 0)
                return -1;
        }
    }
    return 0;
}

static u8 *gdt_desc(struct ext4_fs *fs, u32 group)
{
    return fs->gdt + (u64)group * fs->desc_size;
}

static int gdt_flush_group(struct ext4_fs *fs, u32 group)
{
    u64 offset = (u64)group * fs->desc_size;
    u64 block_index = offset / fs->block_size;

    ext4_gdt_set_csum(fs, group, gdt_desc(fs, group));
    return write_block(fs, fs->gdt_block + block_index,
                       fs->gdt + block_index * fs->block_size);
}

static int bitmap_set_csum(struct ext4_fs *fs, u32 group, int inode_bitmap,
                        const u8 *bitmap)
{
    u8 *desc = gdt_desc(fs, group);
    u16 flags = rd16(desc + 0x12);
    u32 bytes;
    u32 crc;

    if (!ext4_has_csum(fs))
        return 0;
    if (inode_bitmap) {
        if (flags & EXT4_BG_INODE_UNINIT) {
            wr16(desc + 0x1A, 0);
            if (fs->desc_size >= 64)
                wr16(desc + 0x3A, 0);
            return 0;
        }
        bytes = fs->inodes_per_group / 8u;
        crc = crc32c(fs->csum_seed, bitmap, bytes);
        wr16(desc + 0x1A, (u16)(crc & 0xFFFFu));
        if (fs->desc_size >= 64)
            wr16(desc + 0x3A, (u16)(crc >> 16));
    } else {
        if (flags & EXT4_BG_BLOCK_UNINIT) {
            wr16(desc + 0x18, 0);
            if (fs->desc_size >= 64)
                wr16(desc + 0x38, 0);
            return 0;
        }
        bytes = fs->blocks_per_group / 8u;
        crc = crc32c(fs->csum_seed, bitmap, bytes);
        wr16(desc + 0x18, (u16)(crc & 0xFFFFu));
        if (fs->desc_size >= 64)
            wr16(desc + 0x38, (u16)(crc >> 16));
    }
    return 0;
}

static int inode_slot(struct ext4_fs *fs, u32 ino, u64 *block, u32 *off)
{
    u8 desc[64];
    u32 group;
    u32 index;
    u64 table;
    u64 offset;

    if (ino == 0 || ino > fs->inodes_count)
        return -1;
    group = (ino - 1) / fs->inodes_per_group;
    index = (ino - 1) % fs->inodes_per_group;
    if (read_group_desc(fs, group, desc) != 0)
        return -1;
    table = rd32(desc + 8);
    if (fs->desc_size >= 64)
        table |= (u64)rd16(desc + 0x28) << 32;
    offset = (u64)index * fs->inode_size;
    *block = table + offset / fs->block_size;
    *off = (u32)(offset % fs->block_size);
    if (*off + fs->inode_size > fs->block_size)
        return -1;
    return 0;
}

static int read_inode_raw(struct ext4_fs *fs, u32 ino, u8 *raw)
{
    u64 block;
    u32 off;
    u8 *buf;
    int ret = -1;

    if (inode_slot(fs, ino, &block, &off) != 0)
        return -1;
    buf = (u8 *)kmalloc(fs->block_size);
    if (!buf)
        return -1;
    if (read_block(fs, block, buf) == 0 &&
        ext4_inode_check_csum(fs, ino, buf + off) == 0) {
        memcpy(raw, buf + off, fs->inode_size);
        ret = 0;
    }
    kfree(buf);
    return ret;
}

static int write_inode_raw(struct ext4_fs *fs, u32 ino, const u8 *raw)
{
    u64 block;
    u32 off;
    u8 *buf;
    int ret = -1;

    if (inode_slot(fs, ino, &block, &off) != 0)
        return -1;
    buf = (u8 *)kmalloc(fs->block_size);
    if (!buf)
        return -1;
    if (read_block(fs, block, buf) == 0) {
        memcpy(buf + off, raw, fs->inode_size);
        if (write_block(fs, block, buf) == 0)
            ret = 0;
    }
    kfree(buf);
    return ret;
}

static int write_inode(struct ext4_fs *fs, u32 ino, const struct ext4_inode *inode)
{
    u8 *raw;
    int ret;

    raw = (u8 *)kmalloc(fs->inode_size);
    if (!raw)
        return -1;
    if (read_inode_raw(fs, ino, raw) != 0) {
        kfree(raw);
        return -1;
    }
    memcpy(raw, inode, sizeof(*inode));
    if (fs->inode_size >= 128 + 2)
        wr16(raw + 128, EXT4_INODE_EXTRA_ISIZE);
    ext4_inode_set_csum(fs, ino, raw);
    ret = write_inode_raw(fs, ino, raw);
    kfree(raw);
    return ret;
}

static int zero_inode_slot(struct ext4_fs *fs, u32 ino)
{
    u8 *raw;
    int ret;

    raw = (u8 *)kzalloc(fs->inode_size);
    if (!raw)
        return -1;
    if (fs->inode_size >= 128 + 2)
        wr16(raw + 128, EXT4_INODE_EXTRA_ISIZE);
    ext4_inode_set_csum(fs, ino, raw);
    ret = write_inode_raw(fs, ino, raw);
    kfree(raw);
    return ret;
}

static int bitmap_alloc(struct ext4_fs *fs, u32 group, u64 bitmap_block,
                        u32 nbits, u32 start, int inode_bitmap, u32 *out)
{
    u8 *bitmap;
    u32 i;
    int ret = -1;

    if (nbits == 0 || nbits > fs->block_size * 8u)
        return -1;
    bitmap = (u8 *)kmalloc(fs->block_size);
    if (!bitmap)
        return -1;
    if (read_block(fs, bitmap_block, bitmap) != 0)
        goto out;
    if (start >= nbits)
        start = 0;
    for (i = 0; i < nbits; i++) {
        u32 bit = (start + i) % nbits;

        if (!bitmap_test(bitmap, bit)) {
            bitmap_set(bitmap, bit);
            bitmap_set_csum(fs, group, inode_bitmap, bitmap);
            if (write_block(fs, bitmap_block, bitmap) == 0) {
                *out = bit;
                ret = 0;
            }
            break;
        }
    }
out:
    kfree(bitmap);
    return ret;
}

static int alloc_block(struct ext4_fs *fs, u64 *out)
{
    u32 g;
    u32 hint_group = 0;
    u32 hint_off = 0;

    if (fs->next_block_hint >= fs->first_data_block) {
        hint_group = (fs->next_block_hint - fs->first_data_block) /
                     fs->blocks_per_group;
        hint_off = (fs->next_block_hint - fs->first_data_block) %
                   fs->blocks_per_group;
    }
    if (fs->free_blocks == 0)
        return -1;
    for (g = 0; g < fs->groups; g++) {
        u8 *desc = gdt_desc(fs, g);
        u32 group_first = fs->first_data_block + g * fs->blocks_per_group;
        u32 nbits = fs->blocks_per_group;
        u32 start = (g == hint_group) ? hint_off : 0;
        u32 bit;
        u32 block;
        u16 free_count;

        if (group_first >= fs->blocks_count)
            continue;
        if (rd16(desc + 0x12) & EXT4_BG_BLOCK_UNINIT)
            continue;
        if ((u64)group_first + fs->blocks_per_group > fs->blocks_count)
            nbits = fs->blocks_count - group_first;
        if (bitmap_alloc(fs, g, rd32(desc + 0), nbits, start, 0, &bit) != 0)
            continue;
        block = group_first + bit;
        free_count = rd16(desc + 0x0C);
        if (free_count > 0)
            wr16(desc + 0x0C, (u16)(free_count - 1));
        if (gdt_flush_group(fs, g) != 0)
            return -1;
        fs->free_blocks--;
        fs->next_block_hint = block + 1;
        {
            u8 *zero = (u8 *)kzalloc(fs->block_size);
            if (!zero)
                return -1;
            if (write_data_block(fs, block, zero) != 0) {
                kfree(zero);
                return -1;
            }
            kfree(zero);
        }
        *out = block;
        return 0;
    }
    return -1;
}

static int free_block(struct ext4_fs *fs, u64 block)
{
    u32 g;
    u32 group_first;
    u32 bit;
    u8 *desc;
    u8 *bitmap;
    int ret = -1;

    if (block < fs->first_data_block || block >= fs->blocks_count)
        return -1;
    g = (u32)((block - fs->first_data_block) / fs->blocks_per_group);
    group_first = fs->first_data_block + g * fs->blocks_per_group;
    bit = (u32)(block - group_first);
    /* blocks_per_group comes from the superblock: keep the bit inside the
       group so a corrupt geometry cannot clear an unrelated bitmap bit. */
    if (g >= fs->groups || bit >= fs->blocks_per_group ||
        bit >= fs->block_size * 8u)
        return -1;
    desc = gdt_desc(fs, g);
    bitmap = (u8 *)kmalloc(fs->block_size);
    if (!bitmap)
        return -1;
    if (read_block(fs, rd32(desc + 0), bitmap) != 0)
        goto out;
    if (!bitmap_test(bitmap, bit))
        goto out;
    bitmap_clear(bitmap, bit);
    bitmap_set_csum(fs, g, 0, bitmap);
    if (write_block(fs, rd32(desc + 0), bitmap) != 0)
        goto out;
    wr16(desc + 0x0C, (u16)(rd16(desc + 0x0C) + 1));
    if (gdt_flush_group(fs, g) != 0)
        goto out;
    fs->free_blocks++;
    ret = 0;
out:
    kfree(bitmap);
    return ret;
}

static int adjust_dirs_count(struct ext4_fs *fs, u32 ino, int delta)
{
    u32 g;
    u8 *desc;
    u16 used;

    if (ino == 0 || ino > fs->inodes_count)
        return -1;
    g = (ino - 1) / fs->inodes_per_group;
    desc = gdt_desc(fs, g);
    used = rd16(desc + 0x10);
    if (delta > 0)
        wr16(desc + 0x10, (u16)(used + 1));
    else if (used > 0)
        wr16(desc + 0x10, (u16)(used - 1));
    return gdt_flush_group(fs, g);
}

static int alloc_inode(struct ext4_fs *fs, u32 *out)
{
    u32 g;

    if (fs->free_inodes == 0)
        return -1;
    for (g = 0; g < fs->groups; g++) {
        u8 *desc = gdt_desc(fs, g);
        u32 nbits = fs->inodes_per_group;
        u32 bit;
        u32 ino;
        u16 free_count;

        if (rd16(desc + 0x12) & EXT4_BG_INODE_UNINIT)
            continue;
        if ((u64)(g + 1) * fs->inodes_per_group > fs->inodes_count)
            nbits = fs->inodes_count - g * fs->inodes_per_group;
        if (bitmap_alloc(fs, g, rd32(desc + 4), nbits, 0, 1, &bit) != 0)
            continue;
        ino = g * fs->inodes_per_group + bit + 1;
        free_count = rd16(desc + 0x0E);
        if (free_count > 0)
            wr16(desc + 0x0E, (u16)(free_count - 1));
        /* Keep bg_itable_unused describing the still-untouched inode tail:
           allocating an inode inside the tail moves the tail forward. */
        {
            u32 unused = rd16(desc + 0x1C);

            if (bit + 1 + unused > nbits)
                wr16(desc + 0x1C, (u16)(nbits - bit - 1));
        }
        if (gdt_flush_group(fs, g) != 0)
            return -1;
        fs->free_inodes--;
        fs->next_ino_hint = ino + 1;
        if (zero_inode_slot(fs, ino) != 0)
            return -1;
        *out = ino;
        return 0;
    }
    return -1;
}

static int free_inode(struct ext4_fs *fs, u32 ino)
{
    u32 g;
    u32 bit;
    u8 *desc;
    u8 *bitmap;
    int ret = -1;

    if (ino == 0 || ino > fs->inodes_count)
        return -1;
    g = (ino - 1) / fs->inodes_per_group;
    bit = (ino - 1) % fs->inodes_per_group;
    desc = gdt_desc(fs, g);
    bitmap = (u8 *)kmalloc(fs->block_size);
    if (!bitmap)
        return -1;
    if (read_block(fs, rd32(desc + 4), bitmap) != 0)
        goto out;
    if (!bitmap_test(bitmap, bit))
        goto out;
    bitmap_clear(bitmap, bit);
    bitmap_set_csum(fs, g, 1, bitmap);
    if (write_block(fs, rd32(desc + 4), bitmap) != 0)
        goto out;
    wr16(desc + 0x0E, (u16)(rd16(desc + 0x0E) + 1));
    if (gdt_flush_group(fs, g) != 0)
        goto out;
    fs->free_inodes++;
    if (zero_inode_slot(fs, ino) != 0)
        goto out;
    ret = 0;
out:
    kfree(bitmap);
    return ret;
}

static u32 extent_leaf_max(struct ext4_fs *fs)
{
    return (fs->block_size - sizeof(struct ext4_extent_header)) /
           sizeof(struct ext4_extent);
}

static void extent_init(struct ext4_inode *inode)
{
    struct ext4_extent_header *h =
        (struct ext4_extent_header *)(void *)inode->block;

    h->magic = EXT4_EXT_MAGIC;
    h->entries = 0;
    h->max = 4;
    h->depth = 0;
    h->generation = 0;
}

static int extent_add(struct ext4_extent_header *h, u32 node_bytes, u32 lblk,
                      u64 pblk, int *full)
{
    u32 pos;

    if (h->depth != 0)
        return -1;
    /* The header must describe entries that fit inside the bytes we own;
       without this a crafted i_block header with max = 5 turns the memmove
       below into a heap overwrite. */
    if (!harden_extent_header_ok((const u8 *)(const void *)h, node_bytes))
        return -1;
    pos = 0;
    while (pos < h->entries) {
        struct ext4_extent *e =
            (struct ext4_extent *)(void *)((u8 *)h + 12 + pos * 12);
        if (e->ee_block >= lblk)
            break;
        pos++;
    }
    if (pos > 0) {
        struct ext4_extent *prev =
            (struct ext4_extent *)(void *)((u8 *)h + 12 + (pos - 1) * 12);
        u64 prev_end = ((u64)prev->ee_start_hi << 32) | prev->ee_start_lo;
        u32 plen = prev->ee_len & 0x7FFFu;

        prev_end += plen;
        if (prev->ee_block + plen == lblk && prev_end == pblk) {
            prev->ee_len = (u16)(plen + 1);
            *full = 0;
            return 0;
        }
    }
    if (pos < h->entries) {
        struct ext4_extent *e =
            (struct ext4_extent *)(void *)((u8 *)h + 12 + pos * 12);
        if (e->ee_block == lblk) {
            *full = 0;
            return 0;
        }
    }
    if (h->entries >= h->max) {
        *full = 1;
        return 1;
    }
    memmove((u8 *)h + 12 + (pos + 1) * 12, (u8 *)h + 12 + pos * 12,
            (h->entries - pos) * sizeof(struct ext4_extent));
    {
        struct ext4_extent *e =
            (struct ext4_extent *)(void *)((u8 *)h + 12 + pos * 12);
        e->ee_block = lblk;
        e->ee_len = 1;
        e->ee_start_hi = (u16)(pblk >> 32);
        e->ee_start_lo = (u32)pblk;
    }
    h->entries++;
    *full = 0;
    return 0;
}

static int extent_insert(struct ext4_fs *fs, u32 ino,
                         struct ext4_inode *inode, u32 lblk, u64 pblk);

static int extent_grow(struct ext4_fs *fs, u32 ino,
                       struct ext4_inode *inode, u32 lblk, u64 pblk)
{
    struct ext4_extent_header *root =
        (struct ext4_extent_header *)(void *)inode->block;
    struct ext4_extent_header *leaf;
    struct ext4_extent_idx *idx;
    u64 block;
    u32 i;

    if (root->depth != 0 || root->entries >= root->max)
        return -1;
    if (alloc_block(fs, &block) != 0)
        return -1;
    leaf = (struct ext4_extent_header *)kzalloc(fs->block_size);
    if (!leaf) {
        free_block(fs, block);
        return -1;
    }
    leaf->magic = EXT4_EXT_MAGIC;
    leaf->entries = root->entries;
    leaf->max = (u16)extent_leaf_max(fs);
    leaf->depth = 0;
    for (i = 0; i < root->entries; i++) {
        struct ext4_extent *src =
            (struct ext4_extent *)(void *)((u8 *)root + 12 + i * 12);
        struct ext4_extent *dst =
            (struct ext4_extent *)(void *)((u8 *)leaf + 12 + i * 12);
        *dst = *src;
    }
    extent_block_set_csum(fs, ino, inode, (u8 *)(void *)leaf);
    if (write_block(fs, block, leaf) != 0) {
        kfree(leaf);
        free_block(fs, block);
        return -1;
    }
    kfree(leaf);
    idx = (struct ext4_extent_idx *)(void *)((u8 *)root + 12);
    root->entries = 1;
    root->depth = 1;
    root->max = 4;
    idx->ei_block = 0;
    idx->ei_leaf_lo = (u32)block;
    idx->ei_leaf_hi = (u16)(block >> 32);
    idx->ei_unused = 0;
    inode->blocks_lo += fs->block_size / 512u;
    return extent_insert(fs, ino, inode, lblk, pblk);
}

static int extent_insert(struct ext4_fs *fs, u32 ino,
                         struct ext4_inode *inode, u32 lblk, u64 pblk)
{
    struct ext4_extent_header *h =
        (struct ext4_extent_header *)(void *)inode->block;
    int full = 0;

    if (h->magic != EXT4_EXT_MAGIC)
        extent_init(inode);
    if (h->depth == 0) {
        int ret = extent_add(h, EXT4_INLINE_EXTENT_BYTES, lblk, pblk, &full);

        if (ret == 1)
            return extent_grow(fs, ino, inode, lblk, pblk);
        return ret;
    }
    if (h->depth == 1) {
        struct ext4_extent_idx *idx =
            (struct ext4_extent_idx *)(void *)((u8 *)h + 12);
        u64 leaf_block = ((u64)idx->ei_leaf_hi << 32) | idx->ei_leaf_lo;
        struct ext4_extent_header *leaf;
        int ret;

        if (leaf_block == 0)
            return -1;
        leaf = (struct ext4_extent_header *)kzalloc(fs->block_size);
        if (!leaf)
            return -1;
        if (read_block(fs, leaf_block, leaf) != 0) {
            kfree(leaf);
            return -1;
        }
        if (extent_block_check_csum(fs, ino, inode, (u8 *)(void *)leaf) != 0) {
            kfree(leaf);
            return -1;
        }
        ret = extent_add(leaf, fs->block_size - 12u, lblk, pblk, &full);
        if (ret == 0) {
            extent_block_set_csum(fs, ino, inode, (u8 *)(void *)leaf);
            ret = write_block(fs, leaf_block, leaf);
        }
        else
            ret = -1;
        kfree(leaf);
        return ret;
    }
    return -1;
}

static int trim_extents(struct ext4_fs *fs, struct ext4_inode *inode,
                        struct ext4_extent_header *h, u32 new_blocks)
{
    u32 i;
    u32 out = 0;

    for (i = 0; i < h->entries; i++) {
        struct ext4_extent *e =
            (struct ext4_extent *)(void *)((u8 *)h + 12 + i * 12);
        u32 start = e->ee_block;
        u32 len = e->ee_len & 0x7FFFu;
        u64 phys = ((u64)e->ee_start_hi << 32) | e->ee_start_lo;
        u32 keep;
        u32 j;

        if (len == 0)
            continue;
        if (start >= new_blocks) {
            for (j = 0; j < len; j++) {
                if (free_block(fs, phys + j) != 0)
                    return -1;
            }
            inode->blocks_lo -= len * (fs->block_size / 512u);
            continue;
        }
        keep = len;
        if (start + len > new_blocks) {
            u32 drop = start + len - new_blocks;

            for (j = 0; j < drop; j++) {
                if (free_block(fs, phys + keep - drop + j) != 0)
                    return -1;
            }
            inode->blocks_lo -= drop * (fs->block_size / 512u);
            keep = new_blocks - start;
        }
        if (out != i) {
            struct ext4_extent *dst =
                (struct ext4_extent *)(void *)((u8 *)h + 12 + out * 12);
            *dst = *e;
        }
        e = (struct ext4_extent *)(void *)((u8 *)h + 12 + out * 12);
        e->ee_len = (u16)keep;
        out++;
        if (start + len > new_blocks)
            continue;
    }
    h->entries = (u16)out;
    return 0;
}

static int extent_trim(struct ext4_fs *fs, u32 ino,
                       struct ext4_inode *inode, u32 new_blocks)
{
    struct ext4_extent_header *root =
        (struct ext4_extent_header *)(void *)inode->block;

    if (root->magic != EXT4_EXT_MAGIC)
        return 0;
    if (root->depth == 0)
        return trim_extents(fs, inode, root, new_blocks);
    if (root->depth == 1) {
        struct ext4_extent_idx *idx =
            (struct ext4_extent_idx *)(void *)((u8 *)root + 12);
        u64 leaf_block = ((u64)idx->ei_leaf_hi << 32) | idx->ei_leaf_lo;
        struct ext4_extent_header *leaf;
        int ret;

        if (leaf_block == 0)
            return -1;
        leaf = (struct ext4_extent_header *)kzalloc(fs->block_size);
        if (!leaf)
            return -1;
        if (read_block(fs, leaf_block, leaf) != 0) {
            kfree(leaf);
            return -1;
        }
        ret = trim_extents(fs, inode, leaf, new_blocks);
        if (ret == 0) {
            if (leaf->entries == 0) {
                if (free_block(fs, leaf_block) != 0)
                    ret = -1;
                else {
                    inode->blocks_lo -= fs->block_size / 512u;
                    root->entries = 0;
                    root->depth = 0;
                    root->max = 4;
                }
            } else {
                extent_block_set_csum(fs, ino, inode, (u8 *)(void *)leaf);
                if (write_block(fs, leaf_block, leaf) != 0)
                    ret = -1;
            }
        }
        kfree(leaf);
        return ret;
    }
    return -1;
}

static int truncate_inode(struct ext4_fs *fs, u32 ino,
                          struct ext4_inode *inode, u64 size)
{
    u64 current = (u64)inode->size_lo | ((u64)inode->size_high << 32);
    u32 new_blocks;

    if (size > current)
        return -1;
    new_blocks = (u32)((size + fs->block_size - 1) / fs->block_size);
    if (extent_trim(fs, ino, inode, new_blocks) != 0)
        return -1;
    if ((size % fs->block_size) != 0 && size != 0) {
        u64 phys = 0;
        u8 *buf;
        u32 off = (u32)(size % fs->block_size);

        if (bmap(fs, inode, (u32)(size / fs->block_size), &phys) != 0)
            return -1;
        buf = (u8 *)kzalloc(fs->block_size);
        if (!buf)
            return -1;
        if (read_block(fs, phys, buf) != 0) {
            kfree(buf);
            return -1;
        }
        memset(buf + off, 0, fs->block_size - off);
        if (write_data_block(fs, phys, buf) != 0) {
            kfree(buf);
            return -1;
        }
        kfree(buf);
    }
    inode->size_lo = (u32)size;
    inode->size_high = (u32)(size >> 32);
    return write_inode(fs, ino, inode);
}

static int ext4_truncate_impl(struct ext4_fs *fs, const char *path, u64 size)
{
    struct ext4_inode inode;
    u32 ino;

    if (!fs || !(fs->flags & EXT4_MOUNT_RW) || !path)
        return -1;
    if (ext4_lookup(fs, path, &ino) != 0)
        return -1;
    if (read_inode(fs, ino, &inode) != 0)
        return -1;
    if ((inode.mode & 0xF000u) != 0x8000u)
        return -1;
    return truncate_inode(fs, ino, &inode, size);
}


static u16 dir_rec_len(u32 name_len)
{
    return (u16)ALIGN_UP(8u + name_len, 4u);
}

static int split_path(const char *path, char *parent, usize pcap,
                      char *name, usize ncap)
{
    const char *last = 0;
    const char *p;
    usize plen;
    usize nlen;

    if (!path || !parent || !name || pcap == 0 || ncap == 0)
        return -1;
    while (*path == '/')
        path++;
    if (*path == 0)
        return -1;
    for (p = path; *p; p++)
        if (*p == '/')
            last = p;
    if (last) {
        plen = (usize)(last - path);
        while (plen > 0 && path[plen - 1] == '/')
            plen--;
        if (plen == 0) {
            if (pcap < 2)
                return -1;
            parent[0] = '/';
            parent[1] = 0;
        } else {
            if (plen + 1 > pcap)
                return -1;
            memcpy(parent, path, plen);
            parent[plen] = 0;
        }
        nlen = strlen(last + 1);
        if (nlen + 1 > ncap)
            return -1;
        memcpy(name, last + 1, nlen + 1);
    } else {
        if (pcap < 2 || ncap < strlen(path) + 1)
            return -1;
        parent[0] = '/';
        parent[1] = 0;
        strncpy(name, path, ncap - 1);
        name[ncap - 1] = 0;
    }
    if (name[0] == 0 || strlen(name) > 255)
        return -1;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return -1;
    return 0;
}

static int dir_add(struct ext4_fs *fs, u32 parent_ino,
                   struct ext4_inode *parent, const char *name, u32 child,
                   u8 file_type)
{
    u32 name_len = (u32)strlen(name);
    u16 need = dir_rec_len(name_len);
    u64 size = (u64)parent->size_lo | ((u64)parent->size_high << 32);
    u64 pos;
    u32 limit = (u32)ext4_dir_usable(fs);
    u8 *buf;

    if (name_len == 0 || name_len > 255)
        return -1;
    if (dir_walk(fs, parent_ino, name, 0, 0, 0) > 0)
        return -1;
    buf = (u8 *)kzalloc(fs->block_size);
    if (!buf)
        return -1;
    for (pos = 0; pos < size; pos += fs->block_size) {
        u64 phys = 0;
        u32 off = 0;
        int ret = bmap(fs, parent, (u32)(pos / fs->block_size), &phys);

        if (ret < 0)
            goto fail;
        if (ret > 0)
            continue;
        if (read_block(fs, phys, buf) != 0)
            goto fail;
        while (off + 8 <= limit) {
            u32 entry = rd32(buf + off);
            u16 rec = rd16(buf + off + 4);
            u32 used;

            if (rec < 8 || (rec & 3u) != 0 || off + rec > limit)
                break;
            used = entry ? dir_rec_len(buf[off + 6]) : 8u;
            if (used < 8 || used > rec)
                break;
            if (entry == 0 && rec >= need) {
                wr32(buf + off, child);
                wr16(buf + off + 4, rec);
                buf[off + 6] = (u8)name_len;
                buf[off + 7] = file_type;
                memcpy(buf + off + 8, name, name_len);
                ext4_dir_set_tail(fs, parent_ino, parent->generation, buf);
                if (write_block(fs, phys, buf) != 0)
                    goto fail;
                kfree(buf);
                return write_inode(fs, parent_ino, parent);
            }
            if (entry != 0 && rec >= used + need) {
                wr16(buf + off + 4, (u16)used);
                wr32(buf + off + used, child);
                wr16(buf + off + used + 4, (u16)(rec - used));
                buf[off + used + 6] = (u8)name_len;
                buf[off + used + 7] = file_type;
                memcpy(buf + off + used + 8, name, name_len);
                ext4_dir_set_tail(fs, parent_ino, parent->generation, buf);
                if (write_block(fs, phys, buf) != 0)
                    goto fail;
                kfree(buf);
                return write_inode(fs, parent_ino, parent);
            }
            off += rec;
        }
    }
    {
        u64 block;
        u32 lblk = (u32)(size / fs->block_size);

        if (alloc_block(fs, &block) != 0)
            goto fail;
        if (extent_insert(fs, parent_ino, parent, lblk, block) != 0) {
            free_block(fs, block);
            goto fail;
        }
        memset(buf, 0, fs->block_size);
        wr32(buf, child);
        wr16(buf + 4, (u16)limit);
        buf[6] = (u8)name_len;
        buf[7] = file_type;
        memcpy(buf + 8, name, name_len);
        ext4_dir_set_tail(fs, parent_ino, parent->generation, buf);
        if (write_block(fs, block, buf) != 0) {
            free_block(fs, block);
            goto fail;
        }
        parent->size_lo = (u32)(size + fs->block_size);
        parent->size_high = (u32)((size + fs->block_size) >> 32);
        parent->blocks_lo += fs->block_size / 512u;
    }
    kfree(buf);
    return write_inode(fs, parent_ino, parent);
fail:
    kfree(buf);
    return -1;
}

static int dir_remove(struct ext4_fs *fs, u32 parent_ino,
                      struct ext4_inode *parent, const char *name)
{
    u32 name_len = (u32)strlen(name);
    u64 size = (u64)parent->size_lo | ((u64)parent->size_high << 32);
    u64 pos;
    u32 limit = (u32)ext4_dir_usable(fs);
    u8 *buf;

    buf = (u8 *)kzalloc(fs->block_size);
    if (!buf)
        return -1;
    for (pos = 0; pos < size; pos += fs->block_size) {
        u64 phys = 0;
        u32 off = 0;
        u32 prev = 0xFFFFFFFFu;
        int ret = bmap(fs, parent, (u32)(pos / fs->block_size), &phys);

        if (ret < 0)
            goto fail;
        if (ret > 0)
            continue;
        if (read_block(fs, phys, buf) != 0)
            goto fail;
        while (off + 8 <= limit) {
            u32 entry = rd32(buf + off);
            u16 rec = rd16(buf + off + 4);
            u32 nlen = buf[off + 6];

            if (rec < 8 || (rec & 3u) != 0 || off + rec > limit)
                break;
            if (entry != 0 && nlen == name_len &&
                memcmp(buf + off + 8, name, name_len) == 0) {
                if (prev != 0xFFFFFFFFu)
                    wr16(buf + prev + 4, (u16)(rd16(buf + prev + 4) + rec));
                else {
                    wr32(buf + off, 0);
                    buf[off + 6] = 0;
                }
                ext4_dir_set_tail(fs, parent_ino, parent->generation, buf);
                if (write_block(fs, phys, buf) != 0)
                    goto fail;
                kfree(buf);
                return 0;
            }
            prev = off;
            off += rec;
        }
    }
    kfree(buf);
    return -1;
fail:
    kfree(buf);
    return -1;
}

static int dir_is_empty(struct ext4_fs *fs, u32 ino)
{
    struct ext4_inode inode;
    u64 size;
    u64 pos;
    u32 limit = (u32)ext4_dir_usable(fs);
    u8 *buf;
    int empty = 1;

    if (read_inode(fs, ino, &inode) != 0)
        return -1;
    size = (u64)inode.size_lo | ((u64)inode.size_high << 32);
    buf = (u8 *)kzalloc(fs->block_size);
    if (!buf)
        return -1;
    for (pos = 0; pos < size && empty; pos += fs->block_size) {
        u64 phys = 0;
        u32 off = 0;
        int ret = bmap(fs, &inode, (u32)(pos / fs->block_size), &phys);

        if (ret < 0) {
            empty = -1;
            break;
        }
        if (ret > 0)
            continue;
        if (read_block(fs, phys, buf) != 0) {
            empty = -1;
            break;
        }
        while (off + 8 <= limit) {
            u32 entry = rd32(buf + off);
            u16 rec = rd16(buf + off + 4);
            u32 nlen = buf[off + 6];

            if (rec < 8 || (rec & 3u) != 0 || off + rec > limit)
                break;
            if (entry != 0 && nlen > 0) {
                const char *n = (const char *)(buf + off + 8);

                if (!(nlen == 1 && n[0] == '.') &&
                    !(nlen == 2 && n[0] == '.' && n[1] == '.')) {
                    empty = 0;
                    break;
                }
            }
            off += rec;
        }
    }
    kfree(buf);
    return empty;
}

static int dir_update_dotdot(struct ext4_fs *fs, u32 ino, u32 parent)
{
    struct ext4_inode inode;
    u64 size;
    u64 pos;
    u32 limit = (u32)ext4_dir_usable(fs);
    u8 *buf;

    if (read_inode(fs, ino, &inode) != 0)
        return -1;
    size = (u64)inode.size_lo | ((u64)inode.size_high << 32);
    buf = (u8 *)kzalloc(fs->block_size);
    if (!buf)
        return -1;
    for (pos = 0; pos < size; pos += fs->block_size) {
        u64 phys = 0;
        u32 off = 0;
        int ret = bmap(fs, &inode, (u32)(pos / fs->block_size), &phys);

        if (ret < 0)
            goto fail;
        if (ret > 0)
            continue;
        if (read_block(fs, phys, buf) != 0)
            goto fail;
        while (off + 8 <= limit) {
            u16 rec = rd16(buf + off + 4);
            u32 nlen = buf[off + 6];
            const char *n = (const char *)(buf + off + 8);

            if (rec < 8 || (rec & 3u) != 0 || off + rec > limit)
                break;
            if (rd32(buf + off) != 0 && nlen == 2 && n[0] == '.' && n[1] == '.') {
                wr32(buf + off, parent);
                ext4_dir_set_tail(fs, ino, inode.generation, buf);
                if (write_block(fs, phys, buf) != 0)
                    goto fail;
                kfree(buf);
                return 0;
            }
            off += rec;
        }
    }
fail:
    kfree(buf);
    return -1;
}


int ext4_mount(struct ext4_fs *fs, struct block_device *bd, u32 flags)
{
    u8 sb[1024];

    if (ext4_probe(fs, bd) != 0)
        return -1;
    fs->flags = flags;
    if (fs->feature_compat & EXT4_FEATURE_COMPAT_HAS_JOURNAL) {
        if (journal_open(fs, (flags & EXT4_MOUNT_RW) != 0) != 0) {
            ext4_release(fs);
            return -1;
        }
    } else if (fs->state != 1) {
        ext4_release(fs);
        return -1;
    }
    if (load_gdt(fs) != 0) {
        ext4_release(fs);
        return -1;
    }
    if (read_super(fs, sb) != 0) {
        ext4_release(fs);
        return -1;
    }
    fs->free_blocks = rd32(sb + 0x0C);
    fs->free_inodes = rd32(sb + 0x10);
    if ((flags & EXT4_MOUNT_RW) && write_super_state(fs, 0) != 0) {
        ext4_release(fs);
        return -1;
    }
    return 0;
}

int ext4_umount(struct ext4_fs *fs)
{
    int ret = 0;

    if (!fs)
        return -1;
    if (fs->flags & EXT4_MOUNT_RW) {
        if (journal_commit(fs) != 0) {
            /* Keep the superblock marked dirty so the next mount replays
               or discards the journal instead of trusting the on-disk
               metadata. */
            ret = -1;
        } else {
            if (write_super_state(fs, 1) != 0)
                ret = -1;
            if (block_flush(fs->bd) != 0)
                ret = -1;
        }
    }
    journal_close(fs);
    ext4_release(fs);
    return ret;
}

static int ext4_create_impl(struct ext4_fs *fs, const char *path, u16 mode)
{
    char parent_path[256];
    char name[256];
    struct ext4_inode inode;
    struct ext4_inode parent;
    u32 parent_ino;
    u32 ino;

    if (!fs || !(fs->flags & EXT4_MOUNT_RW) || !path)
        return -1;
    if (ext4_lookup(fs, path, &ino) == 0)
        return -1;
    if (split_path(path, parent_path, sizeof(parent_path),
                   name, sizeof(name)) != 0)
        return -1;
    if (ext4_lookup(fs, parent_path, &parent_ino) != 0)
        return -1;
    if (read_inode(fs, parent_ino, &parent) != 0)
        return -1;
    if ((parent.mode & 0xF000u) != 0x4000u)
        return -1;
    if (alloc_inode(fs, &ino) != 0)
        return -1;
    memset(&inode, 0, sizeof(inode));
    inode.mode = (u16)(EXT4_S_IFREG | (mode & 07777u));
    inode.links_count = 1;
    inode.flags = EXT4_EXTENTS_FL;
    extent_init(&inode);
    if (write_inode(fs, ino, &inode) != 0) {
        free_inode(fs, ino);
        return -1;
    }
    if (dir_add(fs, parent_ino, &parent, name, ino, EXT4_FT_REG_FILE) != 0) {
        free_inode(fs, ino);
        return -1;
    }
    return 0;
}

static int ext4_mkdir_impl(struct ext4_fs *fs, const char *path, u16 mode)
{
    char parent_path[256];
    char name[256];
    struct ext4_inode inode;
    struct ext4_inode parent;
    u32 parent_ino;
    u32 ino;
    u64 block;
    u8 *buf;

    if (!fs || !(fs->flags & EXT4_MOUNT_RW) || !path)
        return -1;
    if (ext4_lookup(fs, path, &ino) == 0)
        return -1;
    if (split_path(path, parent_path, sizeof(parent_path),
                   name, sizeof(name)) != 0)
        return -1;
    if (ext4_lookup(fs, parent_path, &parent_ino) != 0)
        return -1;
    if (read_inode(fs, parent_ino, &parent) != 0)
        return -1;
    if ((parent.mode & 0xF000u) != 0x4000u)
        return -1;
    if (alloc_inode(fs, &ino) != 0)
        return -1;
    if (alloc_block(fs, &block) != 0) {
        free_inode(fs, ino);
        return -1;
    }
    buf = (u8 *)kzalloc(fs->block_size);
    if (!buf) {
        free_block(fs, block);
        free_inode(fs, ino);
        return -1;
    }
    wr32(buf, ino);
    wr16(buf + 4, 12);
    buf[6] = 1;
    buf[7] = EXT4_FT_DIR;
    buf[8] = '.';
    wr32(buf + 12, parent_ino);
    wr16(buf + 16, (u16)(ext4_dir_usable(fs) - 12));
    buf[18] = 2;
    buf[19] = EXT4_FT_DIR;
    buf[20] = '.';
    buf[21] = '.';
    ext4_dir_set_tail(fs, ino, inode.generation, buf);
    if (write_block(fs, block, buf) != 0) {
        kfree(buf);
        free_block(fs, block);
        free_inode(fs, ino);
        return -1;
    }
    kfree(buf);
    memset(&inode, 0, sizeof(inode));
    inode.mode = (u16)(EXT4_S_IFDIR | (mode & 07777u));
    inode.links_count = 2;
    inode.flags = EXT4_EXTENTS_FL;
    inode.size_lo = fs->block_size;
    inode.blocks_lo = fs->block_size / 512u;
    extent_init(&inode);
    if (extent_insert(fs, ino, &inode, 0, block) != 0) {
        free_block(fs, block);
        free_inode(fs, ino);
        return -1;
    }
    if (write_inode(fs, ino, &inode) != 0) {
        free_block(fs, block);
        free_inode(fs, ino);
        return -1;
    }
    parent.links_count++;
    if (dir_add(fs, parent_ino, &parent, name, ino, EXT4_FT_DIR) != 0) {
        free_block(fs, block);
        free_inode(fs, ino);
        return -1;
    }
    if (adjust_dirs_count(fs, ino, 1) != 0)
        return -1;
    return 0;
}

static int ext4_write_file_impl(struct ext4_fs *fs, const char *path,
                               const void *buf, usize len)
{
    struct ext4_inode inode;
    u32 ino;
    u64 nblocks;
    u64 i;
    u8 *block;

    if (!fs || !(fs->flags & EXT4_MOUNT_RW) || !path || (!buf && len))
        return -1;
    if (ext4_lookup(fs, path, &ino) != 0)
        return -1;
    if (read_inode(fs, ino, &inode) != 0)
        return -1;
    if ((inode.mode & 0xF000u) != 0x8000u)
        return -1;
    if (truncate_inode(fs, ino, &inode, 0) != 0)
        return -1;
    if (len == 0)
        return 0;
    nblocks = (len + fs->block_size - 1) / fs->block_size;
    for (i = 0; i < nblocks; i++) {
        u64 phys = 0;

        if (bmap(fs, &inode, (u32)i, &phys) == 0 && phys != 0)
            continue;
        if (alloc_block(fs, &phys) != 0)
            return -1;
        if (extent_insert(fs, ino, &inode, (u32)i, phys) != 0) {
            free_block(fs, phys);
            return -1;
        }
        inode.blocks_lo += fs->block_size / 512u;
    }
    block = (u8 *)kzalloc(fs->block_size);
    if (!block)
        return -1;
    for (i = 0; i < nblocks; i++) {
        u64 phys = 0;
        usize off = (usize)(i * fs->block_size);
        usize chunk = len - off;

        if (chunk > fs->block_size)
            chunk = fs->block_size;
        if (bmap(fs, &inode, (u32)i, &phys) != 0 || phys == 0) {
            kfree(block);
            return -1;
        }
        memset(block, 0, fs->block_size);
        memcpy(block, (const u8 *)buf + off, chunk);
        if (write_data_block(fs, phys, block) != 0) {
            kfree(block);
            return -1;
        }
    }
    kfree(block);
    inode.size_lo = (u32)len;
    inode.size_high = (u32)((u64)len >> 32);
    return write_inode(fs, ino, &inode);
}

static int ext4_unlink_impl(struct ext4_fs *fs, const char *path)
{
    char parent_path[256];
    char name[256];
    struct ext4_inode inode;
    struct ext4_inode parent;
    u32 ino;
    u32 parent_ino;

    if (!fs || !(fs->flags & EXT4_MOUNT_RW) || !path)
        return -1;
    if (ext4_lookup(fs, path, &ino) != 0)
        return -1;
    if (read_inode(fs, ino, &inode) != 0)
        return -1;
    if ((inode.mode & 0xF000u) != 0x8000u)
        return -1;
    if (split_path(path, parent_path, sizeof(parent_path),
                   name, sizeof(name)) != 0)
        return -1;
    if (ext4_lookup(fs, parent_path, &parent_ino) != 0)
        return -1;
    if (read_inode(fs, parent_ino, &parent) != 0)
        return -1;
    if (dir_remove(fs, parent_ino, &parent, name) != 0)
        return -1;
    if (inode.links_count > 1) {
        inode.links_count--;
        return write_inode(fs, ino, &inode);
    }
    if (extent_trim(fs, ino, &inode, 0) != 0)
        return -1;
    return free_inode(fs, ino);
}

static int ext4_rmdir_impl(struct ext4_fs *fs, const char *path)
{
    char parent_path[256];
    char name[256];
    struct ext4_inode inode;
    struct ext4_inode parent;
    u32 ino;
    u32 parent_ino;
    int empty;

    if (!fs || !(fs->flags & EXT4_MOUNT_RW) || !path)
        return -1;
    if (ext4_lookup(fs, path, &ino) != 0 || ino == EXT4_ROOT_INO)
        return -1;
    if (read_inode(fs, ino, &inode) != 0)
        return -1;
    if ((inode.mode & 0xF000u) != 0x4000u)
        return -1;
    empty = dir_is_empty(fs, ino);
    if (empty != 1)
        return -1;
    if (split_path(path, parent_path, sizeof(parent_path),
                   name, sizeof(name)) != 0)
        return -1;
    if (ext4_lookup(fs, parent_path, &parent_ino) != 0)
        return -1;
    if (read_inode(fs, parent_ino, &parent) != 0)
        return -1;
    if (dir_remove(fs, parent_ino, &parent, name) != 0)
        return -1;
    if (extent_trim(fs, ino, &inode, 0) != 0)
        return -1;
    if (free_inode(fs, ino) != 0)
        return -1;
    parent.links_count--;
    if (write_inode(fs, parent_ino, &parent) != 0)
        return -1;
    return adjust_dirs_count(fs, ino, -1);
}

static int ext4_rename_impl(struct ext4_fs *fs, const char *oldpath,
                            const char *newpath)
{
    char old_parent_path[256];
    char new_parent_path[256];
    char old_name[256];
    char new_name[256];
    struct ext4_inode inode;
    struct ext4_inode old_parent;
    struct ext4_inode new_parent;
    struct ext4_inode target;
    u32 ino;
    u32 target_ino;
    u32 old_parent_ino;
    u32 new_parent_ino;
    u8 file_type;
    int have_target;

    if (!fs || !(fs->flags & EXT4_MOUNT_RW) || !oldpath || !newpath)
        return -1;
    if (strcmp(oldpath, newpath) == 0)
        return 0;
    if (ext4_lookup(fs, oldpath, &ino) != 0)
        return -1;
    if (read_inode(fs, ino, &inode) != 0)
        return -1;
    if ((inode.mode & 0xF000u) == 0x4000u)
        file_type = EXT4_FT_DIR;
    else if ((inode.mode & 0xF000u) == 0x8000u)
        file_type = EXT4_FT_REG_FILE;
    else
        return -1;
    if (split_path(oldpath, old_parent_path, sizeof(old_parent_path),
                   old_name, sizeof(old_name)) != 0)
        return -1;
    if (split_path(newpath, new_parent_path, sizeof(new_parent_path),
                   new_name, sizeof(new_name)) != 0)
        return -1;
    if (ext4_lookup(fs, old_parent_path, &old_parent_ino) != 0)
        return -1;
    if (ext4_lookup(fs, new_parent_path, &new_parent_ino) != 0)
        return -1;
    if (read_inode(fs, old_parent_ino, &old_parent) != 0)
        return -1;
    if (read_inode(fs, new_parent_ino, &new_parent) != 0)
        return -1;
    if ((new_parent.mode & 0xF000u) != 0x4000u)
        return -1;
    have_target = ext4_lookup(fs, newpath, &target_ino) == 0;
    if (have_target) {
        if (target_ino == ino)
            return 0;
        if (read_inode(fs, target_ino, &target) != 0)
            return -1;
        if (file_type == EXT4_FT_DIR) {
            if ((target.mode & 0xF000u) != 0x4000u)
                return -1;
            if (ext4_rmdir(fs, newpath) != 0)
                return -1;
        } else {
            if (ext4_unlink(fs, newpath) != 0)
                return -1;
        }
        if (read_inode(fs, new_parent_ino, &new_parent) != 0)
            return -1;
    }
    if (dir_add(fs, new_parent_ino, &new_parent, new_name, ino, file_type) != 0)
        return -1;
    if (dir_remove(fs, old_parent_ino, &old_parent, old_name) != 0)
        return -1;
    if (file_type == EXT4_FT_DIR && old_parent_ino != new_parent_ino) {
        if (dir_update_dotdot(fs, ino, new_parent_ino) != 0)
            return -1;
        if (old_parent.links_count > 0)
            old_parent.links_count--;
        new_parent.links_count++;
        if (write_inode(fs, old_parent_ino, &old_parent) != 0)
            return -1;
        if (write_inode(fs, new_parent_ino, &new_parent) != 0)
            return -1;
    }
    return 0;
}

int ext4_fsync(struct ext4_fs *fs)
{
    if (!fs || !fs->bd)
        return -1;
    if (journal_commit(fs) != 0)
        return -1;
    return block_flush(fs->bd);
}

static int ext4_txn_end(struct ext4_fs *fs, int ret)
{
    int sret;

    sret = sb_journal(fs);
    if (journal_commit(fs) != 0)
        return -1;
    if (sret != 0)
        return -1;
    return ret;
}

int ext4_create(struct ext4_fs *fs, const char *path, u16 mode)
{
    int ret;

    if (journal_begin(fs) != 0)
        return -1;
    ret = ext4_create_impl(fs, path, mode);
    return ext4_txn_end(fs, ret);
}

int ext4_mkdir(struct ext4_fs *fs, const char *path, u16 mode)
{
    int ret;

    if (journal_begin(fs) != 0)
        return -1;
    ret = ext4_mkdir_impl(fs, path, mode);
    return ext4_txn_end(fs, ret);
}

int ext4_write_file(struct ext4_fs *fs, const char *path, const void *buf,
                    usize len)
{
    int ret;

    if (journal_begin(fs) != 0)
        return -1;
    ret = ext4_write_file_impl(fs, path, buf, len);
    return ext4_txn_end(fs, ret);
}

int ext4_truncate(struct ext4_fs *fs, const char *path, u64 size)
{
    int ret;

    if (journal_begin(fs) != 0)
        return -1;
    ret = ext4_truncate_impl(fs, path, size);
    return ext4_txn_end(fs, ret);
}

int ext4_unlink(struct ext4_fs *fs, const char *path)
{
    int ret;

    if (journal_begin(fs) != 0)
        return -1;
    ret = ext4_unlink_impl(fs, path);
    return ext4_txn_end(fs, ret);
}

int ext4_rmdir(struct ext4_fs *fs, const char *path)
{
    int ret;

    if (journal_begin(fs) != 0)
        return -1;
    ret = ext4_rmdir_impl(fs, path);
    return ext4_txn_end(fs, ret);
}

int ext4_rename(struct ext4_fs *fs, const char *oldpath, const char *newpath)
{
    int ret;

    if (journal_begin(fs) != 0)
        return -1;
    ret = ext4_rename_impl(fs, oldpath, newpath);
    return ext4_txn_end(fs, ret);
}
