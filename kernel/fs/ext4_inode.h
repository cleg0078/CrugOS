#pragma once

#include <crug/types.h>

#define EXT4_EXTENTS_FL 0x00080000u
#define EXT4_ROOT_INO 2u
#define EXT4_INODE_EXTRA_ISIZE 32u

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

