#pragma once

#include <crug/types.h>
#include <crug/block.h>

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
    u64 gdt_block;
    u32 root_ino;
};

int ext4_mount(struct ext4_fs *fs, struct block_device *bd);
void ext4_umount(struct ext4_fs *fs);
int ext4_lookup(struct ext4_fs *fs, const char *path, u32 *ino);
int ext4_read_file(struct ext4_fs *fs, const char *path, void *buf, usize cap,
                   u64 *out_size);
int ext4_list(struct ext4_fs *fs, const char *path, char *out, usize cap);
int ext4_selftest(void);
