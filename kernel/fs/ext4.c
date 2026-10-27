#include <crug/ext4.h>
#include <crug/string.h>
#include <crug/heap.h>
#include <crug/kernel.h>

#define EXT4_MAGIC 0xEF53u
#define EXT4_EXTENTS_FL 0x00080000u
#define EXT4_EXT_MAGIC 0xF30Au
#define EXT4_ROOT_INO 2u

#define EXT4_FEATURE_COMPAT_DIR_INDEX 0x0040u
#define EXT4_FEATURE_INCOMPAT_FILETYPE 0x0002u
#define EXT4_FEATURE_INCOMPAT_EXTENTS 0x0040u
#define EXT4_FEATURE_INCOMPAT_64BIT 0x0080u
#define EXT4_FEATURE_INCOMPAT_FLEX_BG 0x0200u
#define EXT4_FEATURE_RO_SPARSE_SUPER 0x0001u
#define EXT4_FEATURE_RO_LARGE_FILE 0x0002u
#define EXT4_FEATURE_RO_RECOVER 0x0004u
#define EXT4_FEATURE_RO_HUGE_FILE 0x0008u
#define EXT4_FEATURE_RO_DIR_NLINK 0x0020u
#define EXT4_FEATURE_RO_EXTRA_ISIZE 0x0040u

struct ext4_inode {
    u16 mode;
    u16 uid;
    u32 size_lo;
    u32 atime;
    u32 ctime;
    u32 mtime;
    u32 dtime;
    u16 gid;
    u16 links_count;
    u32 blocks_lo;
    u32 flags;
    u32 osd1;
    u32 block[15];
    u32 generation;
    u32 file_acl_lo;
    u32 size_high;
    u32 obso_faddr;
    u8 osd2[12];
} __attribute__((packed));

struct ext4_extent_header {
    u16 magic;
    u16 entries;
    u16 max;
    u16 depth;
    u32 generation;
} __attribute__((packed));

struct ext4_extent {
    u32 ee_block;
    u16 ee_len;
    u16 ee_start_hi;
    u32 ee_start_lo;
} __attribute__((packed));

struct ext4_extent_idx {
    u32 ei_block;
    u32 ei_leaf_lo;
    u16 ei_leaf_hi;
    u16 ei_unused;
} __attribute__((packed));

static u16 rd16(const u8 *p)
{
    return (u16)p[0] | (u16)((u16)p[1] << 8);
}

static u32 rd32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) |
           ((u32)p[3] << 24);
}

static int read_block(struct ext4_fs *fs, u64 block, void *buf)
{
    if (block == 0)
        return -1;
    return block_read(fs->bd, block * fs->sectors_per_block,
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
    if (read_block(fs, block, buf) == 0) {
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

static int extent_map(struct ext4_fs *fs, const u8 *node, u32 lblk,
                      u64 *phys, int depth)
{
    const struct ext4_extent_header *h;
    u32 i;

    if (depth > 5)
        return -1;
    h = (const struct ext4_extent_header *)node;
    if (h->magic != EXT4_EXT_MAGIC)
        return -1;
    if (h->entries > h->max || h->max > (fs->block_size - 12u) / 12u)
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
        ret = extent_map(fs, buf, lblk, phys, depth + 1);
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
        return extent_map(fs, (const u8 *)inode->block, lblk, phys, 0);
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

static int dir_walk(struct ext4_fs *fs, u32 ino, const char *target,
                    char *list, usize cap, usize *used)
{
    struct ext4_inode inode;
    u8 *buf;
    u64 size;
    u64 pos;
    int found = 0;
    int failed = 0;

    if (read_inode(fs, ino, &inode) != 0)
        return -1;
    if ((inode.mode & 0xF000u) != 0x4000u)
        return -1;
    size = (u64)inode.size_lo | ((u64)inode.size_high << 32);
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
        off = 0;
        while (off + 8 <= fs->block_size && pos + off + 8 <= size) {
            u32 entry = rd32(buf + off);
            u16 rec = rd16(buf + off + 4);
            u32 name_len;
            const char *name;

            if (rec < 8 || (rec & 3u) != 0 || off + rec > fs->block_size)
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

int ext4_mount(struct ext4_fs *fs, struct block_device *bd)
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
    /* EXT2_VALID_FS: refuse an unclean or orphaned filesystem. A
       read-only mount cannot replay the journal. */
    state = rd16(sb + 0x3A);
    if (state != 1)
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
    if (fs->blocks_per_group == 0 || fs->inodes_per_group == 0 ||
        fs->inodes_count == 0 || fs->blocks_count == 0)
        return -1;
    if (fs->first_data_block != (fs->block_size == 1024u ? 1u : 0u))
        return -1;
    incompat = fs->feature_incompat;
    if (incompat & ~(EXT4_FEATURE_INCOMPAT_FILETYPE |
                     EXT4_FEATURE_INCOMPAT_EXTENTS |
                     EXT4_FEATURE_INCOMPAT_64BIT |
                     EXT4_FEATURE_INCOMPAT_FLEX_BG))
        return -1;
    compat = fs->feature_compat;
    if (compat & EXT4_FEATURE_COMPAT_DIR_INDEX)
        return -1;
    ro = fs->feature_ro_compat;
    if (ro & ~(EXT4_FEATURE_RO_SPARSE_SUPER | EXT4_FEATURE_RO_LARGE_FILE |
               EXT4_FEATURE_RO_HUGE_FILE | EXT4_FEATURE_RO_DIR_NLINK |
               EXT4_FEATURE_RO_EXTRA_ISIZE))
        return -1;
    if (ro & EXT4_FEATURE_RO_RECOVER)
        return -1;
    fs->groups = (fs->blocks_count + fs->blocks_per_group - 1) /
                 fs->blocks_per_group;
    fs->gdt_block = (u64)fs->first_data_block + 1;
    if (read_inode(fs, fs->root_ino, &root) != 0)
        return -1;
    if ((root.mode & 0xF000u) != 0x4000u)
        return -1;
    return 0;
}

void ext4_umount(struct ext4_fs *fs)
{
    if (fs)
        memset(fs, 0, sizeof(*fs));
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
