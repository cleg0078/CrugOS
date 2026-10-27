#pragma once

#include <crug/types.h>
#include <crug/block.h>

#define EXT4_MOUNT_RW 0x0001u

struct journal;

struct ext4_fs {
    struct block_device *bd;
    u32 block_size;
    u32 sectors_per_block;
    u32 blocks_count;
    u32 inodes_count;
    u32 blocks_per_group;
    u32 inodes_per_group;
    u32 inode_size;
    u32 desc_size;
    u32 first_data_block;
    u32 groups;
    u32 feature_compat;
    u32 feature_incompat;
    u32 feature_ro_compat;
    u32 csum_seed;
    u64 gdt_block;
    u32 root_ino;
    u32 flags;
    u32 free_blocks;
    u32 free_inodes;
    u32 next_ino_hint;
    u32 next_block_hint;
    u8 *gdt;
    void *journal;
    u16 state;
};

int ext4_mount(struct ext4_fs *fs, struct block_device *bd, u32 flags);
int ext4_umount(struct ext4_fs *fs);
int ext4_lookup(struct ext4_fs *fs, const char *path, u32 *ino);
int ext4_read_file(struct ext4_fs *fs, const char *path, void *buf, usize cap,
                   u64 *out_size);
int ext4_list(struct ext4_fs *fs, const char *path, char *out, usize cap);
int ext4_create(struct ext4_fs *fs, const char *path, u16 mode);
int ext4_mkdir(struct ext4_fs *fs, const char *path, u16 mode);
int ext4_write_file(struct ext4_fs *fs, const char *path, const void *buf,
                    usize len);
int ext4_truncate(struct ext4_fs *fs, const char *path, u64 size);
int ext4_unlink(struct ext4_fs *fs, const char *path);
int ext4_rmdir(struct ext4_fs *fs, const char *path);
int ext4_rename(struct ext4_fs *fs, const char *oldpath, const char *newpath);
int ext4_fsync(struct ext4_fs *fs);
int ext4_selftest(void);

/* Test hook: publish an unclean superblock (state 0 plus NEEDS_RECOVERY)
   directly, without going through the journal. Used by the crash gate to
   mimic a kernel that died after committing but before checkpointing. */
int ext4_test_mark_dirty(struct ext4_fs *fs);
