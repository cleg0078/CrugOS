# RAID1 and RAID10 storage

Phase 3B-3 added a mirrored block device between the partition layer and the
ext4 filesystem; phase 3B-5 adds RAID10 striping over mirror pairs, and phase 3B-6 rebuilds a
blank replacement member from its mirror at boot. The boot
root is found either directly on a partition named `crugroot` (single-disk
path, unchanged since phase 3A) or on one array assembled from GPT partitions
of type `fd00` (Linux RAID) and named `md0`.

```
virtio-blk-pci disks vda..vdh / NVMe namespaces
  -> GPT partition views (type A19D880F-05FC-4D3B-A006-743F0F84911E)
  -> kernel/block/raid.c       metadata validation, md0, mirror/stripe I/O
  -> ext4 + JBD2               unchanged storage stack
```

RAID is the first redundancy layer because the specification orders storage
as `EXT4 -> crypt.device -> raid.device -> member partitions -> block layer`:
encryption belongs above the array, and the phase 3 acceptance criteria
include booting a degraded array.

## Metadata format

Each member stores a 512-byte little-endian record at its last two sectors.
Both copies are identical, so a torn write to one sector still leaves a usable
copy. The struct is packed and checked with a compile-time assertion:

| Offset | Size | Field | Meaning |
| --- | --- | --- | --- |
| `0x00` | 4 | `magic` | `0x43524131` (`CRA1`) |
| `0x04` | 2 | `version` | `1` |
| `0x06` | 2 | `level` | `1` (RAID1) or `10` (RAID10) |
| `0x08` | 4 | `member_count` | expected members: 1..8 for RAID1, even 4..8 for RAID10 |
| `0x0C` | 4 | `slot` | member index, less than `member_count` |
| `0x10` | 4 | `chunk_sectors` | `0` for RAID1, non-zero chunk for RAID10 (128 in tests) |
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

1. probes the metadata on each member, trying the last sector first and the
   second-to-last sector second; a member whose magic is zero in both copies is
   a blank cold spare rather than an error;
2. rejects a bad CRC, a wrong magic/version/level, a non-zero `state`, a slot
   outside `member_count`, a partition size mismatch, or an `array_sectors`
   larger than the data region;
3. requires `chunk_sectors == 0` and 1..8 members for level 1; requires a
   non-zero chunk and an even member count of 4..8 for level 10;
4. uses the first valid member to define the array UUID, level, chunk, member
   count, geometry and generation, and rejects any later member that disagrees
   or reuses a slot;
5. for RAID10, rejects the array if any mirror pair has no present member;
6. assigns each blank member to the lowest missing slot and rebuilds it from a
   present member (see [Cold-spare rebuild](#cold-spare-rebuild));
7. registers a block device named `md0` with `array_sectors` sectors.

Missing members are allowed: the array reports `present` against the metadata
`member_count` and boots degraded. A RAID10 array tolerates one missing member
per mirror pair; losing both members of a pair fails assembly.

### RAID1

Slots are not paired. Reads try healthy members in discovery order and return
the first successful transfer; a failing member is marked failed for the rest
of the boot. Writes and flushes go to every healthy member and succeed if at
least one completes.

### RAID10

Members form mirror pairs by slot: pair `p` is slots `2p` and `2p+1`.
Sector `lba` maps to pair:

```
pair = (lba / chunk_sectors) % (member_count / 2)
```

so the array stripes 128-sector (64 KiB) chunks across the mirror pairs and
each pair stores a full copy of its chunks. Reads try the present members of
that pair (slot `2p`, then `2p+1`) one 512-byte sector at a time; writes go to
every present member of the pair and succeed if at least one completes. A
failed member is marked and skipped for the rest of the boot, so a healthy
member keeps serving the pair. Flushes go to all present members.

All I/O is synchronous, 512 bytes per request, and inherits the virtio-blk or
NVMe polling implementation. There are no write barriers beyond the device
flush already used by JBD2 and no write-intent bitmap.

## Cold-spare rebuild

A member whose metadata sectors are both zero is treated as a blank
replacement disk, not as corruption. This is the realistic case after a disk
swap: the GPT and the `crugraid` partition are recreated, but the array data and
metadata are missing. At assembly time the kernel:

1. records every blank member as a spare and assembles the array from the
   valid members;
2. computes the missing slot mask (`member_count` bits minus the valid slots);
3. pairs spares with missing slots in ascending slot order (slot 0 first);
4. copies the array data into the spare synchronously in 64-sector (32 KiB)
   chunks, then flushes the member;
5. writes a fresh metadata record to the spare's last two sectors, with the
   array UUID, level, chunk, geometry and generation of the array and a newly
   derived `member_uuid`, then flushes again.

RAID1 copies the whole data region. RAID10 copies only the chunks owned by the
spare's mirror pair:

```
for (lba = pair * chunk; lba < array_sectors; lba += pairs * chunk)
    copy min(chunk, array_sectors - lba) sectors from a present pair member
```

so a rebuilt member ends up byte-identical to its mirror for exactly the chunks
that pair owns. After the copy the member participates in normal read, write and
flush I/O and the array reports healthy. The kernel prints one line per rebuilt
member and a summary marker:

```
raid   : rebuilt raid-b.img as slot 1
CRUGOS: raid rebuilt (1 member(s))
CRUGOS: raid healthy (2/2 members)
```

A rebuild is offline and synchronous: it happens before ext4 is mounted, so no
filesystem I/O observes a half-copied member. It is also part of boot time, so a
replaced member adds roughly the time needed to read and write its share of the
array (about 0.7 s for the 12 MiB test array). If the rebuild fails, the kernel
prints `raid   : rebuild of <name> failed`, leaves that slot missing and boots
degraded; an extra spare with no missing slot is reported and ignored. The
array generation is not bumped, because the array was never online and there is
no write-intent bitmap to describe divergent writes.

## Boot integration

`storage_init()` runs after `heap_init()` with interrupts disabled:

1. `block_init()` then `virtio_blk_init()` and `nvme_init()` enumerate block
   devices; every 512-byte device is a RAID candidate.
2. Every disk's GPT is parsed.
3. A partition named exactly `crugroot` is preferred and mounted directly.
4. Otherwise all partitions whose type GUID is the Linux RAID GUID
   (`A19D880F-05FC-4D3B-A006-743F0F84911E`) become array members.
5. `raid_assemble(..., "md0", ...)` runs, rebuilds blank cold spares, and
   prints the array geometry and state.
6. ext4 is mounted on `md0`; the read-only, write and journal self-tests are
   unchanged and run against the array.

Relevant serial markers (a four-member RAID10 healthy boot):

```
CRUGOS: block ready (4 device(s))
CRUGOS: gpt ready
CRUGOS: raid ready
raid   : level 10, 4 member(s), 2 pair(s), chunk 128 sectors
CRUGOS: raid healthy (4/4 members)      # or: CRUGOS: raid degraded (2/4 members)
# with a cold spare, additionally:
# CRUGOS: raid rebuilt (1 member(s))
CRUGOS: ext4 ready
CRUGOS: journal ready
CRUGOS: storage ready
```

RAID1 prints `level 1, N member(s), 1 pair(s), chunk 0 sectors` and
`raid healthy (2/2 members)` or `raid degraded (1/2 members)`.

## Host tools

| Script | Purpose |
| --- | --- |
| `scripts/mkraid.sh A B` | two GPT disks with a `crugraid` (`fd00`) partition and one identical ext4 image; slots 0/1, level 1 |
| `scripts/mkraid10.sh A B C D` | four GPT disks with one ext4 image, slots 0..3, level 10, chunk 128 |
| `scripts/raid-meta.py` | writes and checksums the 512-byte metadata record; optional level and chunk arguments |
| `scripts/raid-blank.sh A` | zeroes a member's data and metadata while keeping its GPT, to simulate a blank replacement disk |
| `scripts/check-raid.py A [B]` | validates RAID1 metadata, geometry and CRC, extracts the data region, runs `e2fsck -fn` and verifies the test files |
| `scripts/check-raid10.py A [B [C [D]]]` | validates RAID10 metadata, unique slots, pair coverage and CRC, rebuilds the logical array from one member per pair, runs `e2fsck -fn` and verifies the test files on every member |

All test disks are 16 MiB; the partition spans LBA 2048..26623 (24576
sectors). The ext4 image has 1 KiB blocks, a 1 MiB journal, and the same
feature set as the single-disk test image. The builders write `/hello.txt`,
then copy the whole data region to every member and stamp the metadata with a
shared random array UUID and generation 1.

Because RAID10 members contain only their own pair's chunks, `check-raid10.py`
rebuilds the 24574-sector logical image by copying each chunk from a present
member of its pair before running `e2fsck`. `check-raid.py` additionally
compares the two RAID1 data regions byte for byte after a healthy boot.

## Gates

| Command | Array | Expectation |
| --- | --- | --- |
| `make test` | none | direct `crugroot` path; `EXPECT_RAID=no` |
| `make test-raid` | two members | `raid healthy (2/2 members)`, exit 33, metadata and ext4 clean |
| `make test-raid-degraded` | one member | `raid degraded (1/2 members)`, exit 33, metadata and ext4 clean |
| `make test-raid-repair` | two members, second blank | slot 1 rebuilt, `raid rebuilt (1 member(s))`, healthy, both members clean afterwards |
| `make test-raid10` | four members, two pairs | `raid healthy (4/4 members)`, exit 33, metadata and ext4 clean |
| `make test-raid10-degraded` | members 0 and 2 | `raid degraded (2/4 members)`, exit 33, metadata and ext4 clean |
| `make test-raid10-repair` | members 0 and 2 valid, 1 and 3 blank | slots 1 and 3 rebuilt, `raid rebuilt (2 member(s))`, healthy, all four members clean afterwards |
| `make test-compat` | none | AVX2-disabled build still passes the direct path |

## Known limits

- RAID1 and RAID10 only; RAID5/6 are not implemented.
- One array (`md0`). There is no assembly by UUID argument, no auto-assembly
  beyond the fixed GPT type, and no installer integration.
- RAID10 has no spare pair, so it tolerates one failure per pair but not a
  second failure in the same pair; there is no rebalance after a member loss.
- Metadata is written by host tools and, for a rebuilt member, by the kernel.
  The kernel only replaces blank spares: it cannot change member count, level,
  chunk, generation or slot, and it never rewrites a valid member.
- A member with corrupt non-zero metadata is fatal to assembly; only fully blank
  metadata is treated as a cold spare.
- Failed members are tracked only in memory. Reboot retries every member; there
  is no online rebuild, hot spare, scrub, write-intent bitmap or
  fail-persistence, and the array generation is not bumped by a rebuild.
- Reads always prefer the first healthy member of a mirror pair, so there is
  no read balancing.
- No online capacity change, no repair on write mismatch and no bad-sector
  remapping.
- Only mirrored data on member partitions; the ESP is not mirrored yet.
- Rebuild is boot-time and synchronous, so booting with a replacement disk is
  slower (about 0.7 s for the 12 MiB test array) and blocks until the copy
  finishes.

These are tracked in [roadmap.md](roadmap.md) under the remaining phase 3B
items.
