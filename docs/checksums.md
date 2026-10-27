# Checksums (ext4 metadata and JBD2 journal)

Phase 3B-8 makes the on-disk format self-checking: every ext4 metadata
structure the kernel writes carries a CRC32C checksum, and the journal carries
JBD2 checksum v3 tags, a checksummed journal superblock, a checksummed
descriptor tail and a checksummed commit block. The point is not speed but
detection: a torn or corrupted block now fails the mount or the replay instead
of silently becoming a wrong directory entry.

## CRC32C

`include/crug/crc32c.h` and `kernel/lib/crc32c.c` implement the Castagnoli
polynomial (reflected `0x82F63B78`), which is what ext4 and JBD2 use. The
function takes an explicit seed and does not xor the result:

```c
u32 crc32c(u32 seed, const void *data, usize len);
```

- seed `0xFFFFFFFF` (`CRC32C_SEED_ONES`) starts a new chain;
- seed `0` (`CRC32C_SEED_ZERO`) is the "value of an empty buffer" and is what
  on-disk zero checksums use;
- there is no table initialization step and no allocation, so it is usable in
  the earliest boot path.

`make test-host` runs `tests/host/test_crc32c.c` (629 checks) which pins the
seed-0 empty value, the standard `crc32c(0xFFFFFFFF, "123456789") ==
0x1CF96D7C`, incremental seeding (`crc32c(crc32c(~0, a), b) ==
crc32c(~0, a + b)`), every byte-offset variant of a buffer and the
bit-at-a-time reference implementation for random-length inputs.

`tools/gen-crc32c.py` regenerates the whole `kernel/lib/crc32c.c` (table plus
the loop) so the source of truth is the polynomial, not a 256-entry literal.
The table lives in `.rodata` and needs no initialization before the allocator
and filesystem come up.

## ext4 checksum seed

All ext4 metadata checksums start from `csum_seed`:

- when `s_feature_incompat & 0x2000` (`metadata_csum_seed`) is set, the seed is
  `s_checksum_seed` at superblock offset `0x270`;
- otherwise it is `crc32c(0xFFFFFFFF, uuid, 16)` over `s_uuid` at offset `0x68`.

The seed is read once at `ext4_probe()` into `struct ext4_fs->csum_seed`.

## ext4 structures

| Structure | Offset of the field | Covered bytes |
| --- | --- | --- |
| Superblock | `0x3FC` | `0x000..0x3FC` with the checksum field zeroed |
| Group descriptor | `desc + 0x1E` | the whole descriptor (`desc_size` bytes) |
| Block bitmap | last 4 bytes | the block, with the checksum field zeroed |
| Inode bitmap | last 4 bytes | the block, with the checksum field zeroed |
| Inode | `inode + 0x7C` | `inode_size` bytes (low 16 bits at `0x7C`, high at `0x82`) |
| Directory block | `block + block_size - 4` | `block_size - 12` bytes |
| Directory entry (ext-4 tail) | tail `+ 8` | `block_size - 12` bytes from the block start |
| Extent block | tail after the last entry | header + entries, then the tail |

Two rules matter in practice:

1. The directory tail checksum chains the *inode generation*:
   `seed_block = crc32c(crc32c(csum_seed, le32(ino)), le32(generation))`. A
   tail computed without the generation is accepted by the kernel but rejected
   by `e2fsck` ("directory passes checks but fails checksum").
2. The inode checksum is only 32-bit when `i_extra_isize >= 32`; for a smaller
   extra area `e2fsck` checks only the low 16 bits at `0x7C`, so the kernel
   writes `i_extra_isize = 32` on freshly allocated inodes and fills both
   halves.

The superblock, group-descriptor and bitmap checksums are recomputed whenever a
field they cover changes; the code recomputes instead of patching, so the same
helpers serve the write path and the journal checkpoint.

## JBD2 journal checksums

The journal superblock (`block 0` of inode 8) is 1024 bytes:

| Field | Offset | Value |
| --- | --- | --- |
| `s_feature_compat` | `0x24` | bit `0x1` = checksum v1 |
| `s_feature_incompat` | `0x28` | `0x2` 64-bit, `0x8` csum v2, `0x10` csum v3 |
| `s_uuid` | `0x30` | 16 bytes, the checksum seed material |
| `s_checksum_type` | `0x50` | a single byte; must be `4` (crc32c) |
| `s_checksum` | `0xFC` | big-endian crc32c of the whole block with `0xFC` zeroed |

The journal checksum seed is `crc32c(0xFFFFFFFF, s_uuid, 16)`. Details that
cost debugging time:

- `s_checksum_type` is a **byte**, not a 32-bit field; writing it big-endian
  makes `e2fsck` report "Journal superblock is corrupt".
- the superblock checksum is computed over all 1024 bytes with only the four
  checksum bytes zeroed, seeded with `0xFFFFFFFF` (not the UUID seed).

Tag format follows `journal_tag_bytes()`: v3 uses 16 bytes (block number,
flags, high block number, checksum), otherwise 8 bytes plus 2 when checksums are
enabled plus 4 for 64-bit block numbers. The data checksum is
`crc32c(crc32c(csum_seed, be32(sequence)), data, block_size)`; v3 stores it
big-endian at tag offset 12, v2 stores the low 16 bits at tag offset 4.

A descriptor block ends with a 4-byte `jbd2_journal_block_tail` at
`block_size - 4`: the tail is zeroed, then the whole descriptor block is
checksummed and the result stored at the tail. 

A commit block is checksummed over the whole block with `h_chksum_type`,
`h_chksum_size` and `h_chksum[0]` zeroed, and the result stored big-endian at
offset 16.

The kernel accepts legacy, v2 and v3 journals and writes the format the
superblock declares. `scripts/journal-csum.py` is the host helper that flips a
freshly formatted journal to v3 (mke2fs 1.47 does not set the journal checksum
flags at all), so both the image and `e2fsck` exercise the checksummed path.

## Replay and failure policy

Replay is two-pass: it first validates the whole transaction (descriptor tail,
every data tag checksum, tag bounds, `LAST_TAG`, then the commit block), and
only then writes the blocks home. A transaction without a valid commit is
discarded as an incomplete tail of the log. A checksum mismatch is fatal,
leaves `s_start` in place and refuses the mount, so a damaged log is never
partially applied.

## Gates

- `make test` - normal boot with `metadata_csum`; `scripts/check-disk.py` runs
  `e2fsck -fn` and requires exit 0, so every checksum we wrote is verified by
  e2fsprogs, not just by our own code.
- `make test-journal-csum` - boots the crash image, commits 100 blocks in two
  descriptors, exits without checkpointing, then `e2fsck -fy` must replay the
  log and `e2fsck -fn` must be clean.
- `make test-journal-replay` - the same crash image, but the *kernel* replays:
  the next boot must print `CRUGOS: journal replayed 100 block(s) at mount`,
  verify the file contents and leave an `e2fsck -fn`-clean filesystem.
- `make test-host` - CRC32C unit tests.

## Known limits

- No `metadata_csum_seed` writing: the seed is honored on read, but images we
  create use the UUID-derived seed.
- No revoke records or async commits; a journal that declares them is rejected
  at open time rather than mis-replayed.
- No 64-bit journal block numbers on write (the format is supported on read).
- Checksum failures are reported and the filesystem stays read-only; there is
  no online repair.
