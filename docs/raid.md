# RAID1 storage

Phase 3B-3 adds a mirrored block device between the partition layer and the
ext4 filesystem. The boot root is found either directly on a partition named
`crugroot` (single-disk path, unchanged since phase 3A) or on a RAID1 array
assembled from GPT partitions of type `fd00` (Linux RAID) and named `md0`.

```
virtio-blk-pci disks vda..vdh
  -> GPT partition views (type A19D880F-05FC-4D3B-A006-743F0F84911E)
  -> kernel/block/raid.c       metadata validation, md0, mirror I/O
  -> ext4 + JBD2               unchanged storage stack
```

RAID1 is the first redundancy layer because the specification orders storage
as `EXT4 -> crypt.device -> raid.device -> member partitions -> block layer`:
encryption belongs above the array, and the phase 3 acceptance criteria include
booting a degraded array.

## Metadata format

Each member stores a 512-byte little-endian record at its last two sectors.
Both copies are identical, so a torn write to one sector still leaves a usable
copy. The struct is packed and checked with a compile-time assertion:

| Offset | Size | Field | Meaning |
| --- | --- | --- | --- |
| `0x00` | 4 | `magic` | `0x43524131` (`CRA1`) |
| `0x04` | 2 | `version` | `1` |
| `0x06` | 2 | `level` | `1` (RAID1 only) |
| `0x08` | 4 | `member_count` | expected members, 1..8 |
| `0x0C` | 4 | `slot` | member index, less than `member_count` |
| `0x10` | 4 | `chunk_sectors` | `0` for RAID1 |
| `0x14` | 4 | `state` | `0` (clean) |
| `0x18` | 8 | `array_sectors` | usable sectors, `member_sectors - 2` |
| `0x20` | 8 | `member_sectors` | partition size in sectors |
| `0x28` | 8 | `generation` | array generation, must match on every member |
| `0x30` | 16 | `array_uuid` | identifies the array |
| `0x40` | 16 | `member_uuid` | identifies this member |
| `0x50` | 428 | `reserved` | zero |
| `0x1FC` | 4 | `checksum` | CRC32 over all 512 bytes with this field zero |

The data region is sectors `0..array_sectors-1`; the metadata copies occupy
`member_sectors-2` and `member_sectors-1`. `member_sectors` must equal the
partition size exactly, and `array_sectors` may not exceed `member_sectors-2`.

## Assembly and I/O

`raid_assemble()` takes up to `RAID_MAX_MEMBERS` (8) partition devices and:

1. reads the metadata from each member, trying the last sector first and the
   second-to-last sector second;
2. rejects a bad CPU/checksum, a wrong magic/version/level, non-zero
   `chunk_sectors` or `state`, a slot outside `member_count`, a partition size
   mismatch, or an `array_sectors` larger than the data region;
3. uses the first valid member to define the array UUID, geometry and
   generation, and rejects any later member that disagrees or reuses a slot;
4. registers a block device named `md0` with `array_sectors` sectors.

Missing members are allowed: the array reports `present` against the metadata
`member_count` and boots degraded. There is no requirement that slot 0 exists.

Reads try healthy members in discovery order and return the first successful
transfer; a failing member is marked failed for the rest of the boot. Writes
and flushes go to every healthy member and succeed if at least one completes,
marking failures the same way. A write that fails on some members does not
repair or resynchronise them.

All I/O is synchronous, 512 bytes per request, and inherits the virtio-blk
polling implementation. There are no write barriers beyond the virtio flush
already used by JBD2, no bitmap, no rebuild and no hot spare.

## Boot integration

`storage_init()` runs after `heap_init()` with interrupts disabled:

1. `block_init()` and `virtio_blk_init()` enumerate every legacy
   virtio-blk-pci function (up to eight) as `vda`..`vdh`.
2. Every `vd?` disk's GPT is parsed.
3. A partition named exactly `crugroot` is preferred and mounted directly.
4. Otherwise all partitions whose type GUID is the Linux RAID GUID
   (`A19D880F-05FC-4D3B-A006-743F0F84911E`) become array members.
5. `raid_assemble(..., "md0", ...)` runs and prints the array state.
6. ext4 is mounted on `md0`; the read-only, write and journal self-tests are
   unchanged and run against the array.

Relevant serial markers:

```
CRUGOS: block ready (2 device(s))
CRUGOS: gpt ready
CRUGOS: raid ready
CRUGOS: raid healthy (2/2 members)      # or: CRUGOS: raid degraded (1/2 members)
CRUGOS: ext4 ready
CRUGOS: journal ready
CRUGOS: storage ready
```

## Host tools

| Script | Purpose |
| --- | --- |
| `scripts/mkraid.sh A B` | builds two GPT disks with a `crugraid` (`fd00`) partition and one identical ext4 image |
| `scripts/raid-meta.py` | writes and checksums the 512-byte metadata record |
| `scripts/check-raid.py A [B]` | validates metadata, geometry and CRC, extracts the data region, runs `e2fsck -fn` and verifies the test files |

Both disks are 16 MiB; the partition spans LBA 2048..26623 (24576 sectors).
The ext4 image has 1 KiB blocks, a 1 MiB journal, and the same feature set as
the single-disk test image. `mkraid.sh` writes `/hello.txt`, then copies the
whole data region to both members and stamps slot 0 and slot 1 with the same
random array UUID and generation 1.

## Gates

| Command | Array | Expectation |
| --- | --- | --- |
| `make test` | none | direct `crugroot` path; `EXPECT_RAID=no` |
| `make test-raid` | two members | `raid healthy (2/2 members)`, exit 33, metadata and ext4 clean |
| `make test-raid-degraded` | one member | `raid degraded (1/2 members)`, exit 33, metadata and ext4 clean |
| `make test-compat` | none | AVX2-disabled build still passes the direct path |

`check-raid.py` also compares the two data regions byte for byte after a
healthy boot, proving the mirror stayed in sync through the write path and the
journal.

## Known limits

- RAID1 only; RAID10, RAID5/6, chunking and striping are not implemented.
- One array (`md0`). There is no assembly by UUID argument, no auto-assembly
  beyond the fixed GPT type, and no installer integration.
- Metadata is written by host tools. The kernel validates it but never creates
  or updates it, so it cannot change member count, generation or slot.
- Failed members are tracked only in memory. Reboot retries every member; there
  is no write-intent bitmap, rebuild, scrub, hot spare or fail-persistence.
- Reads always prefer the first healthy member, so there is no read balancing.
- No online capacity change, no repair on write mismatch and no bad-sector
  remapping.
- Only mirrored data on member partitions; the ESP is not mirrored yet.

These are tracked in [roadmap.md](roadmap.md) under the remaining phase 3B
items.
