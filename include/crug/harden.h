/* CrugOS device-supplied value validators (hardening pass 1).

   Everything here is pure: it takes values that may come from a device
   (superblock, extent header, NVMe registers, crypt header) and answers
   whether using them stays inside the buffer or address space the kernel
   actually owns. Keeping the checks in one host-tested place makes the
   trust boundary explicit instead of scattering magic numbers. */
#ifndef CRUG_HARDEN_H
#define CRUG_HARDEN_H

#include <crug/types.h>

/* The kernel runs on a 4 GiB identity map (kernel/mm/vm.c). A syscall
   pointer/length pair handed in by a caller must stay below it. */
#define HARDEN_IDENTITY_LIMIT (4ull * 1024ull * 1024ull * 1024ull)

/* ext4 extent tree, as laid out in kernel/fs/ext4_inode.h. */
#define HARDEN_EXTENT_MAGIC 0xF30Au
#define HARDEN_EXTENT_HEADER_BYTES 12u
#define HARDEN_EXTENT_BYTES 12u
#define HARDEN_EXTENT_MAX_DEPTH 5u

/* NVMe queue doorbells start at 0x1000 inside BAR0. */
#define HARDEN_NVME_DOORBELL_BASE 0x1000ull

/* Highest Argon2 lanes the crypt header may ask for. */
#define HARDEN_KDF_MAX_LANES 16u

/* addr..addr+len-1 must lie inside [0, limit). A zero length is always
   fine; a NULL address with a non-zero length never is. */
int harden_range_ok(u64 addr, u64 len, u64 limit);

/* A message payload of len > 0 needs a real pointer; NULL must not be
   able to copy uninitialised kernel heap into a port. */
int harden_payload_ok(const void *data, u64 len);

/* node_bytes is the number of bytes that really exist at `node`. */
int harden_extent_header_ok(const u8 *node, u32 node_bytes);

int harden_ext4_geometry_ok(u32 block_size, u32 blocks_per_group,
                            u32 inodes_per_group, u32 blocks_count,
                            u32 inodes_count);

int harden_nvme_doorbell_ok(u64 bar_size, u32 stride, u32 qid, int cq);

int harden_kdf_cost_ok(u32 m_cost_kib, u32 t_cost, u32 lanes, u32 min_m_kib,
                       u32 max_m_kib, u32 min_t_cost, u32 max_t_cost);

/* volume_sectors must fit in the parent partition after the header. */
int harden_volume_ok(u64 volume_sectors, u64 parent_sectors,
                     u64 offset_sectors);

/* A user-supplied pointer/length pair handed in from CPL3 must land inside
   the user window (include/crug/vm.h). Kernel addresses never pass. */
int harden_user_range_ok(u64 addr, u64 len);

/* One ELF PT_LOAD reservation from the user-program loader: page aligned and
   inside the window. An empty segment reserves nothing and is always fine. */
int harden_user_segment_ok(u64 va, u64 size);

/* An entry point (or user stack pointer) must land inside the window. */
int harden_user_entry_ok(u64 entry);

#endif