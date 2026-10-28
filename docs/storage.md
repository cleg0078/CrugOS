# Storage stack

Phase 3A is a vertical slice: a real GPT disk with an ext4 filesystem is read
through a PCI virtio-blk device during boot. Phase 3B-1 adds a write path and
verifies it by creating, rewriting, renaming, truncating and deleting files in a
scratch directory, then checking the resulting filesystem with e2fsck on the host.
Phase 3B-2 adds JBD2 journal replay and ordered metadata journaling, and proves
recovery at mount time. Phase 3B-3 mirrors the root filesystem across two
virtual disks in a RAID1 array and can boot from a single remaining member.
Phase 3B-4 adds a polled NVMe driver, so the same GPT/ext4/JBD2 stack can boot
from an NVMe namespace instead of a legacy virtio-blk disk. Phase 3B-5 adds
RAID10 striping over mirror pairs, with healthy four-member and degraded
(two-member, one per pair) boot gates. Phase 3B-6 rebuilds blank replacement
members from their mirror at boot, so a swapped disk is repaired before ext4 is
mounted. Phase 3B-7 adds a polled AHCI/SATA driver, so the same stack can boot
from a SATA disk as well. Phase 3B-8 adds ext4 metadata checksums and JBD2
journal checksums with multi-descriptor transactions. Phase 3B-9 adds
`crypt.device`, an AES-256-XTS volume unlocked with an Argon2id-derived key, so
the same ext4/JBD2 stack can run on an encrypted partition. Phase 3B-10 unlocks
that volume with a masked passphrase typed on the console (three attempts with a
per-attempt timeout), while `crypt.pass=` on the loader command line remains for
automation. Phase 4A adds `input.device`: a polled PS/2 keyboard whose bytes are
merged with the serial line behind `console_try_getc()`, so the same prompt also
accepts a real keyboard. See [input.md](input.md).

```
virtio-blk-pci (legacy I/O BAR)  NVMe PCIe (class 01/08/02)  AHCI/SATA (class 01/06/01)
  -> drivers/block/virtio_blk.c    -> drivers/block/nvme.c    -> drivers/block/ahci.c
  -> kernel/block/block.c          registry, partitions, bounds checks, flush
  -> kernel/fs/gpt.c               GPT header + entries, CRC32
  -> kernel/block/raid.c           optional RAID1/RAID10 array "md0"
  -> kernel/block/crypt.c          optional AES-256-XTS "crypt0"
  -> kernel/fs/ext4.c              mount, lookup, read/write, create, rename
  -> kernel/fs/storage.c           boot policy and read/write self-test
```

## Boot integration

`storage_init()` runs after `heap_init()` and before `sched_init()`, while
interrupts are still disabled, and returns 1 on success. The boot order and
markers are:

```
CRUGOS: virtio ready (<n> device(s))
CRUGOS: nvme ready (<n> device(s))
CRUGOS: ahci ready (<n> device(s))
CRUGOS: block ready (<n> device(s))
CRUGOS: gpt ready
CRUGOS: crypt passphrase required
CRUGOS: type the passphrase on the console (serial or keyboard) (3 attempt(s))
passphrase: ***********             # masked, never logged
CRUGOS: crypt ready (<n> sectors, keyslot <i>, Argon2id m=<m> KiB t=<t> p=<p>)
CRUGOS: crypt aes: software           # or: aes-ni
CRUGOS: root device crypt0            # encrypted root only
CRUGOS: raid ready                    # RAID root only
CRUGOS: raid rebuilt (<n> member(s))  # only after rebuilding a blank spare
CRUGOS: raid healthy (<n>/<n> members) # or: CRUGOS: raid degraded (<n>/<n> members)
CRUGOS: root device (<partition>)
CRUGOS: ext4 ready
CRUGOS: ext4 /hello.txt: hello from ext4
CRUGOS: journal ready
CRUGOS: storage ready
...
CRUGOS: writable storage ok
CRUGOS: journal selftest ok (<n> blocks recovered)
```

The `crypt` lines appear only when a `crugcrypt` partition is the root; a
rejected passphrase prints `CRUGOS: crypt unlock failed` (with a reason) and
then `CRUGOS: no root filesystem` before the deterministic failure exit. When
there is no `crypt.pass=` token the kernel asks on the console instead (serial
line first, then the PS/2 keyboard), printing one `passphrase:` prompt per
attempt, three attempts bounded by
`crypt.prompt_timeout=` (1..3600 s, 60 s per attempt by default) and only the
`*` mask. A rejected line prints `CRUGOS: passphrase rejected (N attempt(s)
left)`; three rejections print `CRUGOS: crypt locked (too many attempts)`, and
an empty answer, Ctrl-C, overflow or timeout prints `CRUGOS: crypt locked (no
passphrase)`. Both lock paths end in `CRUGOS: no root filesystem`. The secret
is never echoed and every line buffer is zeroed afterwards.

### Boot media without a block device

A boot medium can declare that it ships no disk at all with the
`storage=none` token, which is what the hybrid image built by `make iso`
passes. The transports are still probed and print their `ready` lines, and then

```
CRUGOS: storage skipped (storage=none)
```

replaces the GPT/root/ext4/journal block: `storage_init()` returns success with
`g_present` and `g_root_part` cleared, `storage_selftest()` returns a soft
success (`if (!g_present || !g_root_part) return g_bootstrap ? 1 : 0;`), and
the writable, journal and crash self-tests are skipped because there is no
mount to write to. The token only relaxes the storage contract; it never
fabricates a device. A machine with no block device and no token still prints
`CRUGOS: no root filesystem` and exits 35. See [iso.md](iso.md).

A missing virtio/NVMe/AHCI device, an unusable GPT, a bad RAID metadata record
or an ext4 mount failure panics (`storage: ...`) because the phase 3 boot gate
expects a root filesystem, unless `storage=none` declared there is none. The
read/write self-test also panics if any write-path operation fails, so a broken
write path cannot silently boot. The init step time is reported as `storage=`
in the `boot   :` timing line.

## Block layer

`include/crug/block.h` defines a `block_device`: a 16-byte name, a sector
count, a 512-byte sector size, read/write/flush callbacks, a private pointer and
a registry link. Devices register with `block_register()`; legacy virtio disks
are named `vda`..`vdh`, NVMe namespaces are `nvme0n1`.., AHCI drives are
`sda`..`sdh`, and an assembled RAID array is `md0`.
`block_read()` and `block_write()` reject
out-of-range requests, `block_partition()` creates an offset view that reuses
the parent device, and `block_flush()` forwards a cache flush to the backing
device when it has one.

## PCI and legacy virtio-blk

`drivers/pci/pci.c` uses the legacy 0xCF8/0xCFC configuration mechanism. It
enumerates buses 0..255, every device and every function, looking for the nth
function with vendor `0x1AF4`, device `0x1001` (legacy virtio block).
`virtio_blk_init()` probes up to eight of them and registers each as
`vda`..`vdh`. Each device maps I/O BAR0, enables I/O and bus-master access
and runs the legacy virtio 0.9.5 register layout:

| Offset | Register |
| --- | --- |
| `0x00` | device features |
| `0x04` | guest features |
| `0x08` | queue PFN |
| `0x0C` | queue size |
| `0x0E` | queue select |
| `0x10` | queue notify |
| `0x12` | status |
| `0x14` | capacity low |
| `0x18` | capacity high |

Each device gets one split virtqueue of 2..256 entries placed in three
contiguous PMM pages (descriptors, available ring, used ring). A request uses three
descriptors: the 16-byte block header, one 512-byte data sector and the
status byte. I/O is synchronous and polled with a 500 ms
`timer_monotonic_ns()` deadline; there are no interrupts, no MSI-X and no
multiqueue. The guest accepts `VIRTIO_BLK_F_FLUSH` when the device offers it
and implements `VIRTIO_BLK_T_FLUSH`; all other guest features stay disabled.
`block_read(..., count)` and `block_write(..., count)` issue one request per
sector, which is slow but simple and keeps the ring state trivial.

QEMU devices are attached with `disable-modern=on` so they expose the legacy
interface the driver implements. Additional disks become `vdb`, `vdc`, and so
on:

```
-drive file=<disk.img>,if=none,id=disk0,format=raw
-device virtio-blk-pci,drive=disk0,disable-modern=on
-drive file=<raid-b.img>,if=none,id=disk1,format=raw
-device virtio-blk-pci,drive=disk1,disable-modern=on
```

## NVMe

`drivers/block/nvme.c` discovers controllers by PCI class `01/08/02`, maps
the (possibly 64-bit, possibly above 4 GiB) BAR through generic page tables,
disables any controller left enabled by firmware, creates one admin queue and
one I/O queue pair, identifies namespace 1 and registers it as `nvme0n1`.
Reads, writes and flushes are polled with a 1000 ms deadline; PRP1/PRP2 and a
PRP list cover multi-page requests. Queue-creation dwords use queue id in the
low 16 bits and size in the high 16 bits; CSTS is at `0x1C` and AQA at `0x24`.
See [nvme.md](nvme.md) for the register map, command encoding, failure modes
and limits.

QEMU attaches an NVMe namespace with:

```
-drive file=<nvme.img>,if=none,id=nvme0,format=raw
-device nvme,drive=nvme0,serial=CRUGNVME0,logical_block_size=512
```

`make test-nvme` builds the same GPT/ext4 image as `make disk`, boots with only
the NVMe disk, requires `CRUGOS: nvme ready (1 device(s))` and `nvme0n1`, and
runs `scripts/check-disk.py` on the image.

## AHCI / SATA

`drivers/block/ahci.c` discovers controllers by PCI class `01/06/01`, which on
q35 is the ICH9 AHCI at `00:1f.2`. Ports are capped by `CAP.NP` and by the
mapped BAR window, empty ports get a 20 ms link window, and each populated port
is brought up with a command list, FIS area and command table. `IDENTIFY
DEVICE` supplies the 48-bit LBA capacity, and drives register as `sda`..`sdh`
with `READ/WRITE DMA EXT`, `FLUSH CACHE EXT` and an 8 KiB bounce buffer.

QEMU attaches a SATA disk with:

```
-drive file=<ahci.img>,if=none,id=ahci0,format=raw
-device ide-hd,drive=ahci0,bus=ide.1
```

`make test-ahci` builds the same GPT/ext4 image, boots with only that disk, and
requires `CRUGOS: ahci ready`, an `sd?` drive and an `sd?` root device before it
runs `scripts/check-disk.py`. Note that the firmware ESP disk is also on this
controller (port 0), so a passing boot normally prints two `sd` devices. See
[ahci.md](ahci.md) for the register map, BAR mapping rules and limits.

## GPT

`gpt_parse()` reads LBA 1, checks the protective MBR entry type `0xEE`, then
validates the `EFI PART` header, its CRC32 (poly `0xEDB88320`) and the entry
array CRC. Entries are 128..4096 bytes, at most 128 are scanned and at most
`GPT_MAX_PARTITIONS` (16) are kept. UTF-16 partition names are converted to
ASCII by dropping the high byte. `storage_init()` has three root policies: it
prefers a partition named exactly `crugroot`; otherwise it unlocks an encrypted
root, which is a partition named `crugcrypt` or carrying the CrugOS type GUID
`8f1c7d2e-3b45-4c6a-9e01-c2a1b7f0d9e3`; failing both, it assembles a RAID
array from partitions whose type GUID is the Linux RAID type
`A19D880F-05FC-4D3B-A006-743F0F84911E`. Partition bounds are checked again
when an offset view is created.

## RAID1 and RAID10

When no `crugroot` or encrypted partition exists, every `fd00` partition
becomes a member of one array. Each member carries a checksummed 512-byte metadata record in
its last two sectors; `raid_assemble()` validates level, chunk, geometry,
UUID, generation and slots, then registers `md0` with `member_sectors - 2`
usable sectors. Missing members boot degraded instead of failing. The full
metadata layout, assembly rules, host tools and limits are documented in
[raid.md](raid.md).

Level 1 mirrors every sector across all members: reads take the first healthy
member, writes and flushes go to every healthy member, and a failed member is
dropped for the rest of the boot. Level 10 requires an even 4..8 members and a
non-zero chunk; slots pair as `2p`/`2p+1`, and sector `lba` maps to pair
`(lba / chunk_sectors) % (member_count / 2)`. A RAID10 read tries the present
members of that pair, writes go to both, and assembly fails if any mirror pair
has no present member. The serial line after `raid ready` reports the actual
geometry, for example
`raid   : level 10, 4 member(s), 2 pair(s), chunk 128 sectors`.

A member whose metadata sectors are both zero is a cold spare, not a failure.
After the valid members define the array, each blank member is assigned the
lowest missing slot and copied synchronously from a present member of its mirror
(RAID1: the whole data region; RAID10: only the chunks owned by its pair) in
64-sector chunks. The kernel then writes a fresh checksummed metadata record to
the spare, flushes it, prints `raid   : rebuilt <name> as slot <n>` and counts
it as present, so the array reports healthy. The rebuild is offline: it runs
before ext4 mounts, adds roughly 0.7 s for the 12 MiB test array, keeps the
array generation and never rewrites a valid member. If the copy fails the slot
stays missing and the array boots degraded. See [raid.md](raid.md) for the
alignment loop and limits.

## crypt.device

`kernel/block/crypt.c` wraps one partition as `crypt0`. The 16 KiB header
holds the algorithm identifiers, Argon2id parameters, a 32-byte salt, the
volume id and eight 256-byte keyslots; the data area begins at sector 32 of the
partition.

`storage_parse_cmdline()` takes `crypt.pass=` when the loader command line has
one and wipes the command-line page afterwards. With no token the boot falls
back to the console: `storage_prompt_unlock()` prints
`CRUGOS: crypt passphrase required`, then `passphrase: `, and reads a masked
line through `console_try_getc()` (serial line first, then the polled i8042
keyboard via `input_try_getc()`) and the shared line editor
(`kernel/lib/lineedit.c`), echoing `*` per accepted byte. It allows three
attempts, each bounded by `crypt.prompt_timeout=` (1..3600 s, 60 s by default),
prints `CRUGOS: passphrase rejected (N attempt(s) left)` for a wrong line and
`CRUGOS: crypt locked (too many attempts)` - or `CRUGOS: crypt locked (no
passphrase)` for an empty answer, Ctrl-C, overflow or timeout - when it gives
up, zeroing every line buffer.

Either path hands the passphrase to `crypt_unlock()`, which derives 96 bytes
with Argon2id (a 64-byte XTS key-encryption key and a 32-byte MAC key),
verifies the keyslot MAC with a constant-time compare, unwraps the 64-byte
master key and registers the device. Reads decrypt each 512-byte sector in place with the data-relative LBA
as the XTS tweak; writes encrypt through a 16 KiB bounce buffer. AES-NI is used
when the CPU advertises it, otherwise the software tables. `storage_lock()`
wipes the passphrase, the volume keys and the bounce buffer and marks the
device unusable before the boot report finishes, and the report prints only the
redacted command-line length. The full layout, key hierarchy, security limits
and host tools are in [crypto.md](crypto.md).

## ext4 mount and read path

`ext4_mount()` reads the 1024-byte superblock at byte 1024 and supports:

- block sizes 1, 2 and 4 KiB; 32-bit block counts only
  (`s_blocks_count_hi` must be zero);
- 32- and 64-byte group descriptors; inode sizes 128 bytes up to one block;
- `FILETYPE`, `EXTENTS`, `64BIT`, `FLEX_BG`, `CSUM_SEED` and `RECOVER`
  incompatible features;
- `SPARSE_SUPER`, `LARGE_FILE`, `RECOVER`, `HUGE_FILE`, `DIR_NLINK`,
  `EXTRA_ISIZE` and `METADATA_CSUM` read-only-compatible features; the kernel
  verifies and recomputes superblock, group-descriptor, bitmap, inode,
  directory-tail and extent-tail checksums when `METADATA_CSUM` is set;
- a clean filesystem, or a dirty one whose journal can be replayed; a read-only
  mount is refused while the journal is dirty, while `EXT4_MOUNT_RW` replays
  it first;
- extent trees of depth up to 5, plus the legacy 12 direct blocks and single
  indirect block; every extent header is checked against the bytes the node
  really owns (60 for the inline root in `i_block`, `block_size - 12 - csum`
  for a leaf) before its entries are followed or appended;
- linear directories; `DIR_INDEX` (htree) is rejected.

Phase H1 validates the geometry before it trusts it. `ext4_probe()` requires a
power-of-two block size of at least 1024, non-zero block and inode counts, and
`s_blocks_per_group` / `s_inodes_per_group` at most `block_size * 8` (one
bitmap block's worth of bits). That check runs before the `metadata_csum`
superblock checksum, so a crafted value is rejected with
`CRUGOS: ext4 mount rejected (geometry)` without the attacker also having to
fix the checksum. After `read_inode()` the root inode's inline extent root is
validated against the real 60-byte `i_block` area (magic `0xF30A`, entries at
most `eh_max`, depth at most 5 and `12 + eh_max * 12 <= 60`), so a bad header
fails with `CRUGOS: ext4 mount rejected (root extent)` only after the inode
checksum has authenticated. Either failure makes `storage_init()` print
`CRUGOS: no root filesystem` and exit 35.

Unsupported features are rejected at mount time rather than misread:
`META_BG`, `INLINE_DATA`, `ENCRYPT`, an unknown checksum type and any unknown
incompatible bit. There is no triple-indirect
or double-indirect extent mapping beyond the single indirect block.

When `COMPAT_HAS_JOURNAL` is set, `ext4_mount()` opens the journal before it
loads the group descriptor table. A read-only mount fails if `s_start` is
non-zero; a read-write mount replays the journal first. A non-journaled
filesystem still requires the clean `s_state == 1` marker.

The directory walk copies entries into a caller buffer and stops on a
malformed record. `ext4_read_file()` allocates one block-sized scratch buffer
from the heap, zero-fills sparse holes and returns the file size in
`out_size` even when the caller's buffer truncates the data.

## ext4 write path

`ext4_mount(fs, bd, EXT4_MOUNT_RW)` additionally loads the group descriptor
table into the heap and allows these operations:

| Call | Behaviour |
| --- | --- |
| `ext4_create(path, mode)` | allocates an inode, initializes an empty extent tree and adds a directory entry |
| `ext4_mkdir(path, mode)` | allocates an inode and one data block containing `.` and `..`, links it into the parent and increments the parent link count |
| `ext4_write_file(path, buf, len)` | truncates the file to zero, allocates extent-mapped blocks, copies the data and updates size/blocks |
| `ext4_truncate(path, size)` | frees blocks past `size`, shrinks the tail extent and updates the inode |
| `ext4_unlink(path)` | removes the directory entry, decrements the parent link count, frees blocks and the inode |
| `ext4_rmdir(path)` | requires an empty directory, removes it and adjusts the parent link and used-directory counts |
| `ext4_rename(old, new)` | moves an entry between directories, updating both parents; replacing a target is not supported |
| `ext4_fsync(fs)` | commits the current journal transaction and issues a virtio flush |

Writes use the extent tree only. A newly created file has an inline extent root;
the first overflow allocates one leaf block and turns the root into a depth-1
index (no depth-2 growth yet). The legacy direct/single-indirect mapping is
read-only. There is no delayed allocation.

Because the filesystem carries `metadata_csum`, every write path that touches a
checksummed structure recomputes its checksum instead of patching it: inode
checksums come from `ext4_inode_set_csum()`, bitmaps and group descriptors are
re-summed after allocation, and both the adding and the removing directory
paths end in `ext4_dir_set_tail()` so a directory block is never written with a
stale tail (that tail chains the inode generation as well as the block). See
[checksums.md](checksums.md) for the field offsets.

Every public write call runs inside one journal transaction: the wrapper calls
`journal_begin()`, runs the existing `*_impl` operation, then commits. Nested
operations (for example `ext4_rename()` calling `unlink`/`rmdir`) share the
same transaction through the journal depth counter. File data blocks bypass the
journal and are written directly; all metadata blocks (inode, bitmap, group
descriptor, directory and extent leaf) are queued in the journal.

### Allocation correctness details

- A block bitmap bit is relative to the group's first block. With 1 KiB blocks
  the superblock occupies block 1, so group 0 starts at
  `s_first_data_block` and the first bit maps to that block. Allocation and
  free use `group_first = s_first_data_block + group * blocks_per_group`;
  treating the bit as an absolute block silently duplicates data blocks.
- Inode bitmap bits are relative to the group's inode range:
  `ino = group * inodes_per_group + bit + 1`.
- `mkdir` and `rmdir` maintain the group descriptor's `used_dirs_count`, and
  every directory add or remove writes the parent inode so link counts and
  sizes survive a remount.
- `bg_itable_unused` shrinks when an inode inside the untouched tail is
  allocated, so the unused-inode area never claims a live inode.
- Freeing re-checks the bit index before it clears it: a group at or past
  `groups` or a bit at or past `blocks_per_group` is refused, so a corrupted
  extent cannot make `free_block()` clear a bit in another group's bitmap.
- Group descriptor free counters and the superblock free counts/state are
  written back; `ext4_umount()` marks the filesystem clean and flushes it
  after the final journal commit succeeds. If that commit fails the
  superblock stays dirty so the next mount replays or discards the journal.
  Mounting read-only after a clean unmount passes the same clean-state check
  the boot path uses.

## JBD2 journal

The journal is the standard JBD2 layout stored in inode 8 and mapped through the
normal extent path. All journal fields are big-endian. The writer and the replay
path support the three on-disk tag formats (legacy 8 bytes, checksum v2 and
checksum v3 16-byte tags) and pick one from the journal superblock, so an image
formatted by mke2fs without checksums still mounts.

| Item | Value |
| --- | --- |
| Superblock | block 0, magic `0xC03B3998`, type 4, 1024 bytes |
| Descriptor block | type 1, header + tags + 16-byte UUID + optional 4-byte tail |
| Commit block | type 2, same sequence, checksum at byte 16 |
| Sequence | superblock bytes 24..27; start block bytes 28..31 |
| Checksum type | byte 0x50 must be 4 (`JBD2_CRC32C_CHKSUM`) when enabled |
| Checksum | crc32c over the 1024-byte superblock at byte 0xFC |

When `s_feature_incompat` bit 0x10 (`CSUM_V3`) is set, the writer emits v3
tags: big-endian block number at 0, flags at 4, high block number at 8 and a
32-bit checksum at 12. Bit 0x8 (`CSUM_V2`) selects 2-byte checksums with the
legacy 8-byte tag plus the checksum. Bit 0x2 enables 64-bit block numbers.
The journal superblock itself is checksummed, so a torn log header is detected
before any block is applied.

`journal_open()` validates the magic, block size, `maxlen`, `first`, the
start range and the feature flags (revoke and async-commit journals are
rejected), verifies the superblock checksum when checksums are enabled, and then
replays a dirty log. Recovery reads the descriptor chain at `s_start`, collects
the tag list, validates the checksum of every queued data block, validates the
following commit block *before* writing any data, applies the transaction with
raw device writes, advances to the next sequence and finally clears `s_start`.
A descriptor chain that ends without a commit is discarded and treated as the
end of the log; a checksum mismatch is fatal and leaves `s_start` untouched so a
later boot can retry instead of silently dropping committed metadata.

The writer (`journal_commit`) is ordered:

1. flush the device so file data blocks are durable;
2. set `s_start = first` and `s_sequence` in the journal superblock (with its
   checksum recomputed);
3. write descriptor blocks and their queued metadata blocks, chunked so that
   every descriptor fits one journal block, with the 16-byte UUID after the
   first tag and `LAST_TAG` on the last tag of each chunk;
4. write one commit block carrying the checksum of the whole block;
5. flush, then checkpoint the metadata to its final location;
6. flush, clear `s_start` and increment `s_sequence`.

A transaction may span several descriptors: each holds at most
`(block_size - 12 - 16 - 4) / tag_bytes` blocks (62 for 1 KiB v3 tags), and the
whole chain plus the commit block must fit in the journal
(`chunks + blocks + 1 <= maxlen - 1`). The superblock write is journaled like
any other metadata block, so a transaction that updates allocation counters and
inodes commits atomically.

`CRUGOS: journal selftest ok` is printed after the write self-test queues a
dirty data block, unmounts *without* clearing the journal, remounts read-write
and verifies that `journal_open()` recovered the block. The host `e2fsck`
gate then confirms the resulting filesystem is clean.

The crash gates exercise the checksummed path directly:

- `make test-journal-replay` boots the `crash` image, journals 100 data blocks
  in one transaction (two descriptors at 1 KiB) and exits without checkpointing
  or clearing the journal. The next boot finds a refused read-only mount, mounts
  read-write, replays `CRUGOS: journal replayed 100 block(s) at mount`, verifies
  the file contents, and the host `e2fsck -fn` must exit 0.
- `make test-journal-csum` stops after that same commit and lets the host
  `e2fsck -fy` replay the log instead, proving our checksummed descriptors,
  commit block and journal superblock are accepted by e2fsprogs.

## Test disks

`make disk` builds `build/x86_64/disk.img` with `scripts/mkdisk.sh` (16 MiB
by default, `DISK_SIZE_MB` overrides):

- GPT, one partition at LBA 2048..26623 (12 MiB), type `8300`, name
  `crugroot`;
- ext4 with 1 KiB blocks, `has_journal` with a 1 MiB journal (`-J size=1`),
  `metadata_csum`, `^64bit`, `^flex_bg`, `^dir_index`, `^resize_inode`,
  `^sparse_super`, `^large_file`, `^orphan_file`;
- JBD2 checksum v3 enabled by `scripts/journal-csum.py` after `mke2fs`, because
  mke2fs 1.47 does not set the journal checksum flags itself;
- `/hello.txt` containing `hello from ext4`; `scripts/mkdisk.sh ... crash` adds
  `/simulate-crash`, which selects the crash self-test path.

`make run` and `make test` depend on `disk` and pass the image as `vda`.
`make test` always deletes and regenerates the image first, boots the kernel,
then runs `scripts/check-disk.py`. The checker extracts the `crugroot`
partition, runs `e2fsck -fn`, verifies `/hello.txt`, and verifies that the
5000-byte pattern written by the kernel survived as a 1000-byte
`/selftest/b.txt` after truncation and a read-only remount. It also proves the
journal was checkpointed and cleared: a leftover dirty journal makes
`e2fsck` fail the gate.

`make nvme-disk` builds `build/x86_64/nvme.img` with the same `mkdisk.sh`
recipe (GPT `crugroot` plus ext4) for `make test-nvme`. `make ahci-disk` builds
`build/x86_64/ahci.img` the same way for `make test-ahci`.

`make crypt-disk` builds `build/x86_64/crypt.img` with `scripts/mkdisk.sh ...
crypt`: a GPT `crugcrypt` partition (type `8f1c7d2e-...`) whose 12 MiB data
area is the usual ext4/JBD2 image, encrypted in place by `mkcrypt format`
after a 16 KiB header and two sealed keyslots are written. `make test-crypt`
puts `crypt.pass=$(CRYPT_PASS)` in the ESP command-line file, boots from the
image, and runs `check-disk.py --crypt-tool build/x86_64/mkcrypt
--crypt-password ...`, which extracts and decrypts the partition before
`e2fsck -fn` and the file checks. The wrong-key and tamper gates expect
`CRUGOS: crypt unlock failed` instead, and `make test-crypt-ni` repeats the
healthy boot with `QEMU_CPU=qemu64,+aes,+avx,+avx2` and requires
`CRUGOS: crypt aes: aes-ni`.

The three prompt gates deliberately leave `CRYPT_PASS` empty, so the ESP
carries no `crypt.pass=` token and the kernel must ask. `make test-crypt-prompt`
uses [scripts/run-crypt-prompt.py](../scripts/run-crypt-prompt.py) to reserve a
loopback port, run QEMU with `SERIAL_TCP` (`-serial tcp:...,server=on,wait=on`),
type the passphrase once at the prompt and then check the decrypted image with
`mkcrypt extract`. `make test-crypt-prompt-wrong` types
`CRYPT_PROMPT_WRONG_PASS` three times and expects
`CRUGOS: crypt locked (too many attempts)`; `make test-crypt-prompt-timeout`
appends `crypt.prompt_timeout=$(CRYPT_PROMPT_TIMEOUT_SEC)` (2 s) and sends
nothing, expecting `CRUGOS: crypt locked (no passphrase)`. Both failing gates
exit 35, and all three fail if the passphrase text shows up in the serial log.

`make raid-disks` builds two 16 MiB GPT disks with `scripts/mkraid.sh`:
each has a `crugraid` partition of type `fd00` at LBA 2048..26623, the same
ext4 image is written to both data regions, and `scripts/raid-meta.py` stamps
slots 0 and 1 with a shared array UUID and generation. `make test-raid` boots
with both disks and expects a healthy array; `make test-raid-degraded` boots
with only the first disk and expects a degraded array. `make test-raid-repair`
runs `scripts/raid-blank.sh` on the second member (data and metadata zeroed,
GPT kept), boots, expects one rebuild and a healthy array, and then verifies
both members with `scripts/check-raid.py`, which validates metadata copies on
each member, runs `e2fsck -fn` on the data region and checks the persisted
files.

`make raid10-disks` builds four such disks with `scripts/mkraid10.sh`:
slots 0..3, level 10, chunk 128 sectors, and one shared ext4 image. `make
test-raid10` boots all four members and expects `raid healthy (4/4 members)`;
`make test-raid10-degraded` boots slots 0 and 2 and expects
`raid degraded (2/4 members)`. `make test-raid10-repair` blanks slots 1 and 3,
boots, expects two rebuilds and a healthy array, then checks all four members.
Because each member holds only its pair's chunks, `scripts/check-raid10.py`
validates metadata and pair coverage, then rebuilds the logical array from one
member per pair before running `e2fsck -fn` and checking the persisted files.

To inspect the disks on the host:

```
sgdisk -p build/x86_64/disk.img
dd if=build/x86_64/disk.img of=/tmp/part.img bs=512 skip=2048 count=24576
debugfs -R 'cat /hello.txt' /tmp/part.img
e2fsck -fn /tmp/part.img
sgdisk -p build/x86_64/raid-a.img
scripts/check-raid.py build/x86_64/raid-a.img build/x86_64/raid-b.img
```

## Failure modes

- `virtio : unsupported queue size` or `queue setup failed`: the device is not
  the legacy model, or BAR0 is not an I/O BAR.
- `storage: no usable GPT partition`: no protective MBR or bad CRCs; check
  with `sgdisk -p`.
- `CRUGOS: ext4 mount rejected (geometry)`: `s_blocks_per_group` or
  `s_inodes_per_group` is zero or larger than `block_size * 8`, so the bitmap
  arithmetic would run past its block. Check with `dumpe2fs -h`.
- `CRUGOS: ext4 mount rejected (root extent)`: the root inode's inline extent
  root has the wrong magic, `eh_max` does not fit the 60-byte `i_block` area,
  or its depth exceeds 5. The inode checksum was valid, so the image was
  crafted, not merely truncated.
- `storage: ext4 mount failed`: unsupported feature, unclean state, or a
  partition offset that does not point at the filesystem. `dumpe2fs -h` on the
  extracted partition shows the feature flags.
- `storage: /hello.txt not found`: the image was built without the test file;
  run `make disk` to regenerate it.
- `raid   : metadata invalid`: a member's last two sectors hold a non-zero but
  invalid `CRA1` record, or the CRC, level, chunk, geometry, UUID or generation
  disagree. Truly blank metadata is treated as a cold spare and rebuilt, not
  rejected. Run `scripts/check-raid.py` or `scripts/check-raid10.py` on the
  images.
- `raid   : mirror pair N has no member`: a RAID10 array lost both members of
  a pair, so there is no source to rebuild from. Recreate the disks with
  `make raid10-disks`.
- `raid   : rebuild of <name> failed` or `raid   : spare <name> has no missing
  slot`: the copy failed (slot left missing, array boots degraded) or more
  spares were supplied than there are missing slots (extra spare ignored).
- `raid   : ... does not match the array geometry`: members carry different
  level/chunk/member counts, sizes, UUIDs or generations. Check that all
  images came from the same `mkraid.sh`/`mkraid10.sh` run.
- `CRUGOS: raid degraded` on a two-disk test: one virtio device did not appear
  or its metadata was rejected; the serial log lists the devices found. A
  four-member RAID10 test showing `2/4` is expected for the degraded gate.
- `CRUGOS: crypt unlock failed` with a `crypt` parenthetical: the 16 KiB
  header is not valid, the passphrase does not match any active keyslot, or a
  keyslot MAC failed. Check that the `crypt.pass=` token in the ESP command
  line - or the line typed at the console prompt - matches the passphrase
  `mkdisk.sh`/`mkcrypt format` sealed; `make test-crypt-wrongkey` and
  `make test-crypt-tamper` expect this failure.
- `CRUGOS: crypt locked (no passphrase)`: no `crypt.pass=` token was found and
  the console prompt got no usable line - EOF, Ctrl-C, a line at the 256-byte
  limit, or the `crypt.prompt_timeout=` deadline. In a non-interactive run
  (serial log only) nothing can answer the prompt, so supply the token or use
  a prompt gate; the `test-crypt` targets put it on the ESP.
- `CRUGOS: passphrase rejected (N attempt(s) left)`: the typed line did not
  match any active keyslot. Two more attempts follow; after the third the boot
  locks.
- `CRUGOS: crypt locked (too many attempts)`: all three console attempts were
  rejected, so the volume stays locked and the boot exits 35. Check the
  passphrase sealed by `make crypt-disk`.
- `CRUGOS: crypt ready` but the ext4 mount fails: the decrypted data is not the
  filesystem image that was formatted, or the image was regenerated without
  re-encrypting it. Run `make crypt-disk` and `make test-crypt` together.
- `crypt aes: software` in `make test-crypt-ni`: the guest CPU model lacks
  `+aes`; the target sets `QEMU_CPU=qemu64,+aes,+avx,+avx2`.
- `ahci   : BAR setup failed`: 32-bit BAR mask overflow or a BAR inside the
  identity map that needs `vm_protect_page()` instead of `vm_map_page()`; see
  [ahci.md](ahci.md).
- `CRUGOS: ahci ready (0 device(s))` in `make test-ahci`: the image was not
  attached on `bus=ide.1`, or the port never reported `SSTS.DET == 3`.
- `nvme   : controller not ready` or an NVMe timeout: CSTS/AQA offsets,
  firmware leaving the controller enabled, or a BAR above 4 GiB not mapped by
  the generic page-table path; see [nvme.md](nvme.md).
- `storage: no usable GPT partition` with `fd00` disks: the partition type or
  name is wrong, or `mkraid.sh` was not run.
- `storage: writable test failed`: the read/write self-test failed; the serial
  log contains the individual step that returned the error. Run
  `scripts/check-disk.py` on the image to see whether e2fsck or the file
  content check failed.
- `storage: journal selftest failed`: the dirty-journal replay did not restore
  the block, or the remount could not replay it. A read-only mount of a dirty
  journal is refused by design; use `EXT4_MOUNT_RW` to recover.
- A write returning `-1` after a large metadata operation: the transaction does
  not fit in the journal at all (more descriptors than `maxlen - 1` allows), or
  the journal tag format leaves no room for a descriptor.
- `journal: descriptor checksum mismatch` / `journal: data checksum mismatch`:
  the journal superblock or a queued block was corrupted. The replay path fails
  closed and leaves `s_start` set, so the filesystem stays read-only until the
  image is repaired on the host.
- A timeout in `virtio_submit()` means the device never completed a request;
  QEMU was probably given a modern-only device or a wrong disk image.

## Known limits

- Up to eight legacy virtio-blk disks, up to four NVMe controllers with one
  namespace each, and up to four AHCI controllers/four drives; no USB storage
  and no hotplug.
- Polled I/O with no request merging, no interrupts and no DMA address
  translation (identity map for virtio; NVMe PRPs point at identity-mapped
  bounce pages; AHCI uses a single PRDT entry over an 8 KiB bounce buffer).
  Virtio issues one sector per request; NVMe and AHCI issue one command per
  block call and AHCI uses one non-queued command slot.
- GPT only: no MBR boot disks, no backup-table repair and no partition
  creation.
- ext4 writes checksummed metadata (superblock, group descriptors, bitmaps,
  inode and directory-extension tails, extent tails) and JBD2 checksum v2/v3
  journals; revoke records, async commits and 64-bit journal block numbers are
  not written. There is no directory index and no delayed allocation. A
  transaction larger than the journal capacity fails instead of wrapping.
- The write path handles at most one extent-tree level and does not replace an
  existing rename target; hard links, symlinks, ownership and extended
  attributes are not implemented.
- RAID1 and RAID10 only, one array (`md0`). Rebuild is boot-time and
  synchronous, only for fully blank metadata; there is no online rebuild, hot
  spare, scrub, write-intent bitmap or generation bump.
- Encryption is one `crypt.device` over one partition: AES-256-XTS with no
  per-sector authentication (header/keyslot MACs only), passphrase typed on the
  serial line or the PS/2 keyboard (or supplied through the loader command
  line), no framebuffer/GOP input, no USB HID keyboard and no virtual
  terminals, no TPM/keyfile slot, no rekey or slot-management tool,
  no discard propagation, and RAID below encryption is not implemented. The
  passphrase, keys and bounce buffer are wiped by `storage_lock()` and the boot
  report redacts the command line. No redundant ESP yet. These remain phase 3B
  work.
