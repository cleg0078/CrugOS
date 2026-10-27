#pragma once

#include <crug/types.h>

struct ext4_fs;

#define JOURNAL_MAGIC 0xC03B3998u

/* Open the journal described by ext4 inode 8. Recovers committed
   transactions when rw is non-zero; a read-only mount is refused when
   the journal is dirty. */
int journal_open(struct ext4_fs *fs, int rw);
void journal_close(struct ext4_fs *fs);

/* Metadata transaction bracketing. Transactions nest; the outermost
   commit writes descriptor/data/commit blocks, checkpoints and clears
   the journal. */
int journal_begin(struct ext4_fs *fs);
int journal_commit(struct ext4_fs *fs);

int journal_queue(struct ext4_fs *fs, u64 block, const void *buf);
int journal_lookup(struct ext4_fs *fs, u64 block, void *buf);

int journal_is_active(struct ext4_fs *fs);
int journal_is_clean(struct ext4_fs *fs);
u32 journal_recovered_blocks(struct ext4_fs *fs);

/* Replay committed transactions, then clear s_start. */
int journal_recover(struct ext4_fs *fs);

/* Test helper: commit one metadata block but leave the journal dirty
   (no checkpoint, no clear) so journal_recover() can be exercised. */
int journal_test_dirty(struct ext4_fs *fs, u64 block, const void *buf,
                       usize len);

/* Test helper: make the next ordinary commit stop after the commit block
   (no checkpoint). The caller must unmount without a further commit; the
   journal is then replayed by the host e2fsck or by the next mount. */
int journal_test_skip_checkpoint(struct ext4_fs *fs, int on);

