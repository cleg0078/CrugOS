# Storage stack

Phase 3A is a vertical slice: a real GPT disk with an ext4 filesystem is read
through a PCI virtio-blk device during boot. Phase 3B-1 adds a write path and
verifies it by creating, rewriting, renaming, truncating and deleting files in a
scratch directory, then checking the resulting filesystem with e2fsck on the host.
RAID, encryption and journal replay are still later phases.

```
virtio-blk-pci (legacy I/O BAR)
  -> drivers/block/virtio_blk.c    block_device "vda" + flush
  -> kernel/block/block.c          registry, partitions, bounds checks, flush
  -> kernel/fs/gpt.c               GPT header + entries, CRC32
  -> kernel/fs/ext4.c              mount, lookup, read/write, create, rename
  -> kernel/fs/storage.c           boot policy and read/write self-test
```

## Boot integration

`storage_init()` runs after `heap_init()` and before `sched_init()`, while
interrupts are still disabled, and returns 1 on success. The boot order and
markers are:

```
CRUGOS: block ready (<device>, <sectors> sectors)
CRUGOS: gpt ready
CRUGOS: ext4 ready
CRUGOS: ext4 /hello.txt: hello from ext4
CRUGOS: storage ready
...
CRUGOS: writable storage ok
```

A missing virtio device, an unusable GPT or an ext4 mount failure panics
(`storage: ...`) because the phase 3 boot gate expects a root filesystem. The
read/write self-test also panics if any write-path operation fails, so a broken
write path cannot silently boot. The init step time is reported as `storage=`
in the `boot   :` timing line.

## Block layer

`include/crug/block.h` defines a `block_device`: a 16-byte name, a sector
count, a 512-byte sector size, read/write/flush callbacks, a private pointer and
a registry link. Devices register with `block_register()`; the first device is
the boot disk and is named `vda`. `block_read()` and `block_write()` reject
out-of-range requests, `block_partition()` creates an offset view that reuses
the parent device, and `block_flush()` forwards a cache flush to the backing
device when it has one.

## PCI and legacy virtio-blk

`drivers/pci/pci.c` uses the legacy 0xCF8/0xCFC configuration mechanism and
scans bus 0 for vendor `0x1AF4`, device `0x1001` (legacy virtio block). The
driver maps I/O BAR0, enables I/O and bus-master access and runs the legacy
virtio 0.9.5 register layout:

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

One split virtqueue of 2..256 entries is placed in three contiguous PMM
pages (descriptors, available ring, used ring). A request uses three
descriptors: the 16-byte block header, one 512-byte data sector and the
status byte. I/O is synchronous and polled with a 500 ms
`timer_monotonic_ns()` deadline; there are no interrupts, no MSI-X and no
multiqueue. The guest accepts `VIRTIO_BLK_F_FLUSH` when the device offers it
and implements `VIRTIO_BLK_T_FLUSH`; all other guest features stay disabled.
`block_read(..., count)` and `block_write(..., count)` issue one request per
sector, which is slow but simple and keeps the ring state trivial.

The QEMU device is attached with `disable-modern=on` so it exposes the legacy
interface the driver implements:

```
-drive file=<disk.img>,if=none,id=disk0,format=raw
-device virtio-blk-pci,drive=disk0,disable-modern=on
```

## GPT

`gpt_parse()` reads LBA 1, checks the protective MBR entry type `0xEE`, then
validates the `EFI PART` header, its CRC32 (poly `0xEDB88320`) and the entry
array CRC. Entries are 128..4096 bytes, at most 128 are scanned and at most
`GPT_MAX_PARTITIONS` (16) are kept. UTF-16 partition names are converted to
ASCII by dropping the high byte. The kernel does not require a specific type
GUID; `storage_init()` prefers a partition named `crugroot` and otherwise
uses the first entry. Partition bounds are checked again when the offset view
is created.

## ext4 mount and read path

`ext4_mount()` reads the 1024-byte superblock at byte 1024 and supports:

- block sizes 1, 2 and 4 KiB; 32-bit block counts only
  (`s_blocks_count_hi` must be zero);
- 32- and 64-byte group descriptors; inode sizes 128 bytes up to one block;
- `FILETYPE`, `EXTENTS`, `64BIT` and `FLEX_BG` incompatible features;
- `SPARSE_SUPER`, `LARGE_FILE`, `HUGE_FILE`, `DIR_NLINK` and `EXTRA_ISIZE`
  read-only-compatible features;
- a clean filesystem: `s_state` must be `EXT2_VALID_FS` (1), because a
  read-only mount cannot replay the journal;
- extent trees of depth up to 5, plus the legacy 12 direct blocks and single
  indirect block;
- linear directories; `DIR_INDEX` (htree) is rejected.

Unsupported features are rejected at mount time rather than misread:
`META_BG`, `INLINE_DATA`, `ENCRYPT`, `METADATA_CSUM`, the journal recovery
flag and any unknown incompatible or read-only-compatible bit. There is no
journal replay and no triple-indirect or double-indirect extent mapping beyond
the single indirect block.

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
| `ext4_fsync(fs)` | issues a virtio flush and returns any device error |

Writes use the extent tree only. A newly created file has an inline extent root;
the first overflow allocates one leaf block and turns the root into a depth-1
index (no depth-2 growth yet). The legacy direct/single-indirect mapping is
read-only. There is no journal (JBD2), no checksum metadata and no delayed
allocation.

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
- Group descriptor free counters and the superblock free counts/state are
  written back; `ext4_umount()` marks the filesystem clean and flushes it.
  Mounting read-only after a clean unmount passes the same clean-state check
  the boot path uses.

## Test disk

`make disk` builds `build/x86_64/disk.img` with `scripts/mkdisk.sh` (16 MiB
by default, `DISK_SIZE_MB` overrides):

- GPT, one partition at LBA 2048..26623 (12 MiB), type `8300`, name
  `crugroot`;
- ext4 with 1 KiB blocks, `^has_journal`, `^metadata_csum`, `^64bit`,
  `^flex_bg`, `^dir_index`, `^resize_inode`, `^sparse_super`, `^large_file`;
- `/hello.txt` containing `hello from ext4`.

`make run` and `make test` depend on `disk` and pass the image as `vda`.
`make test` always deletes and regenerates the image first, boots the kernel,
then runs `scripts/check-disk.py`. The checker extracts the `crugroot`
partition, runs `e2fsck -fn`, verifies `/hello.txt`, and verifies that the
5000-byte pattern written by the kernel survived as a 1000-byte
`/selftest/b.txt` after truncation and a read-only remount.

To inspect the partition on the host:

```
sgdisk -p build/x86_64/disk.img
dd if=build/x86_64/disk.img of=/tmp/part.img bs=512 skip=2048 count=24576
debugfs -R 'cat /hello.txt' /tmp/part.img
e2fsck -fn /tmp/part.img
```

## Failure modes

- `virtio : unsupported queue size` or `queue setup failed`: the device is not
  the legacy model, or BAR0 is not an I/O BAR.
- `storage: no usable GPT partition`: no protective MBR or bad CRCs; check
  with `sgdisk -p`.
- `storage: ext4 mount failed`: unsupported feature, unclean state, or a
  partition offset that does not point at the filesystem. `dumpe2fs -h` on the
  extracted partition shows the feature flags.
- `storage: /hello.txt not found`: the image was built without the test file;
  run `make disk` to regenerate it.
- `storage: writable test failed`: the read/write self-test failed; the serial
  log contains the individual step that returned the error. Run
  `scripts/check-disk.py` on the image to see whether e2fsck or the file
  content check failed.
- A timeout in `virtio_submit()` means the device never completed a request;
  QEMU was probably given a modern-only device or a wrong disk image.

## Known limits

- One legacy virtio-blk disk; no AHCI, NVMe, USB storage or hotplug.
- Polled, one-sector-at-a-time I/O with no request merging, no interrupts and
  no DMA address translation (identity map only).
- GPT only: no MBR boot disks, no backup-table repair and no partition
  creation.
- ext4 has no journal replay, no metadata checksums, no directory index and no
  delayed allocation. The mount is refused if the filesystem is not clean.
- The write path handles at most one extent-tree level and does not replace an
  existing rename target; hard links, symlinks, ownership and extended
  attributes are not implemented.
- No RAID1/RAID10, no `crypt.device`, no AES-256-XTS or Argon2id, and no
  redundant ESP yet. These remain phase 3B work.
