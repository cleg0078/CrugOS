#pragma once

#include <crug/types.h>

struct ext4_fs;

/* Raw block access that bypasses the journal cache. */
int ext4_raw_read_block(struct ext4_fs *fs, u64 block, void *buf);
int ext4_raw_write_block(struct ext4_fs *fs, u64 block, const void *buf);

/* Journal inode (inode 8) mapping and size. */
int ext4_journal_map_block(struct ext4_fs *fs, u64 logical, u64 *phys);
u64 ext4_journal_block_count(struct ext4_fs *fs);

/* Physical block backing a file's logical block, used by the journal
   replay self-test. */
int ext4_file_block(struct ext4_fs *fs, const char *path, u32 lblk, u64 *phys);

