# Storage stack

Phase 3A is a vertical slice: a real GPT disk with an ext4 filesystem is read
through a PCI virtio-blk device during boot. The stack is deliberately small
and read-only; RAID, encryption and the write path are later phases.

```
virtio-blk-pci (legacy I/O BAR)
  -> drivers/block/virtio_blk.c    block_device "vda"
  -> kernel/block/block.c          registry, partitions, bounds checks
  -> kernel/fs/gpt.c               GPT header + entries, CRC32
  -> kernel/fs/ext4.c              mount, lookup, list, read
  -> kernel/fs/storage.c           boot policy and self-test
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
```

A missing virtio device, an unusable GPT or an ext4 mount failure panics
(`storage: ...`) because the phase 3 boot gate expects a root filesystem. The
init step time is reported as `storage=` in the `boot   :` timing line.

## Block layer

`include/crug/block.h` defines a `block_device`: a 16-byte name, a sector
count, a 512-byte sector size, read/write callbacks, a private pointer and a
registry link. Devices register with `block_register()`; the first device is
the boot disk and is named `vda`. `block_read()` and `block_write()` reject
out-of-range requests, and `block_partition()` creates an offset view that
reuses the parent device. The phase 3 guest has exactly one block device.

## PCI and legacy virtio-blk

`drivers/pci/pci.c` uses the legacy 0xCF8/0xCFC configuration mechanism and
scans bus 0 for vendor `0x1AF4`, device `0x1001` (legacy virtio block). The
driver maps I/O BAR0, enables I/O and bus-master access and runs the legacy
virtio 0.9.5 register layout:

| Offset | Register |
| --- | --- |
| `0x00` | device features |
| `0x04` | guest features (none accepted) |
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
`timer_monotonic_ns()` deadline; there are no interrupts, no MSI-X, no
multiqueue and no optional features (no `VIRTIO_BLK_F_RO`, no flush, no
`VIRTIO_F_VERSION_1`). `block_read(..., count)` issues one request per sector,
which is slow but simple and keeps the ring state trivial.

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

## ext4 (read-only subset)

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
write, create, truncate, rename or journal replay path, and no triple-indirect
or double-indirect extent mapping beyond the single indirect block.

The directory walk copies entries into a caller buffer and stops on a
malformed record. `ext4_read_file()` allocates one block-sized scratch buffer
from the heap, zero-fills sparse holes and returns the file size in
`out_size` even when the caller's buffer truncates the data.

## Test disk

`make disk` builds `build/x86_64/disk.img` with `scripts/mkdisk.sh` (16 MiB by
default, `DISK_SIZE_MB` overrides):

- GPT, one partition at LBA 2048..14335 (6 MiB), type `8300`, name `crugroot`;
- ext4 with 1 KiB blocks, `^has_journal`, `^metadata_csum`, `^64bit`,
  `^flex_bg`, `^dir_index`, `^resize_inode`, `^sparse_super`, `^large_file`;
- `/hello.txt` containing `hello from ext4`.

`make run` and `make test` depend on `disk` and pass the image as `vda`. To
inspect the partition on the host:

```
sgdisk -p build/x86_64/disk.img
dd if=build/x86_64/disk.img of=/tmp/part.img bs=512 skip=2048 count=12288
debugfs -R 'cat /hello.txt' /tmp/part.img
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
- A timeout in `virtio_submit()` means the device never completed a request;
  QEMU was probably given a modern-only device or a wrong disk image.

## Known limits

- One legacy virtio-blk disk; no AHCI, NVMe, USB storage or hotplug.
- Polled, one-sector-at-a-time I/O with no request merging, no interrupts and
  no DMA address translation (identity map only).
- GPT only: no MBR boot disks, no backup-table repair and no partition
  creation.
- ext4 is read-only, single-filesystem, no journal replay and no directory
  index; the mount is refused if the filesystem is not clean.
- No RAID1/RAID10, no `crypt.device`, no AES-256-XTS or Argon2id, and no
  redundant ESP yet. These remain phase 3B work.
