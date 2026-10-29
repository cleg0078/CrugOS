/* CrugOS copy-on-write frame reference counts (phase U6).
 *
 * A forked child shares every user frame with its parent, so the allocator
 * must not hand a frame back while any address space still maps it. This
 * table keeps one reference count per shared physical frame: an
 * open-addressed hash over the frame number, sized for a whole forked image
 * (COW_TABLE_SLOTS 4096 frames = 16 MiB of shared data).
 *
 * The module only needs <crug/types.h>, so kernel/mm/cow.c links into the
 * host unit tests unchanged and is exercised without CR3 or interrupts.
 */
#ifndef CRUG_COW_H
#define CRUG_COW_H

#include <crug/types.h>

/* Entries in the frame table. A fork shares one entry per distinct frame, so
   this also bounds how many pages one fork chain can hold together. */
#define COW_TABLE_SLOTS 4096u

/* Forget every frame (host tests). */
void cow_reset(void);

/* Register one more owner of the frame PA: 0 on success, -1 when the table
   is full. The first share of a frame records two owners (the space that
   already maps it plus the caller). */
int cow_share(u64 pa);

/* Owners currently recorded for PA, or 0 when the frame is not shared. */
u32 cow_refs(u64 pa);

/* Drop one owner: 1 means the caller owns the frame again and must free it,
   0 means another address space still maps it. An untracked frame always
   reports 1, so a space that never forked frees exactly as before. */
int cow_release(u64 pa);

/* Frames currently recorded as shared (diagnostics and tests). */
u32 cow_tracked(void);

#endif /* CRUG_COW_H */
