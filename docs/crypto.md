# Encryption: crypt.device (AES-256-XTS + Argon2id)

Phase 3B-9 adds `crypt.device`: a sector-level encrypted block device that sits
between a GPT partition and the ext4 mount. The kernel unlocks the volume at
boot from a passphrase supplied by the loader and then mounts the filesystem
inside the encrypted data area. This is the first encrypted-root milestone; the
spec's RAID-below-encryption install layout and a TPM mode are still phase 3B
work (see [roadmap.md](roadmap.md)).

```
GPT partition type 8f1c7d2e-...-c2a1b7f0d9e3, name "crugcrypt"
  -> kernel/block/crypt.c        crypt0 (AES-256-XTS over 512-byte sectors)
  -> kernel/fs/ext4.c            ext4 on the decrypted device
```

## Disk layout

The partition starts with a 16 KiB header area (32 sectors). The rest of the
partition is the data area; XTS data unit 0 is the first sector after the
header, so the tweak does not depend on the absolute disk address.

```
offset        size      contents
0x0000        256 B     struct crug_crypt_header
0x0100        3840 B    zero padding
0x1000        8 * 256 B keyslots (slot i at 0x1000 + i*256)
0x2000        ...       AES-256-XTS data area (ext4)
```

Header fields (little-endian; `include/crug/crypt.h`):

| Offset | Field | Value |
| --- | --- | --- |
| `0x00` | `magic[8]` | `CRUGCRYP` |
| `0x08` | `version` | 1 |
| `0x0C` | `header_bytes` | 16384 |
| `0x10` | `cipher` | 1 = AES-256-XTS |
| `0x14` | `kdf` | 1 = Argon2id |
| `0x18` | `kdf_m_cost_kib` | Argon2id memory cost in KiB |
| `0x1C` | `kdf_t_cost` | Argon2id iterations |
| `0x20` | `kdf_lanes` | Argon2id parallelism |
| `0x24` | `slot_count` | 8 |
| `0x28` | `slot_bytes` | 256 |
| `0x2C` | `slots_offset` | 4096 |
| `0x30` | `data_offset_sectors` | 32 |
| `0x38` | `volume_sectors` | whole partition size |
| `0x40` | `salt[32]` | random per format |
| `0x60` | `volume_id[16]` | random per format |
| `0x70` | `flags` | 0 |

Keyslot fields:

| Offset | Field | Meaning |
| --- | --- | --- |
| `0x00` | `state` | 0 empty, 1 active |
| `0x04` | `flags` | 0 password, 1 recovery |
| `0x08` | `nonce[32]` | random per seal (low 8 bytes tweak the master-key wrapping) |
| `0x28` | `enc_master[64]` | XTS-wrapped volume master key |
| `0x68` | `mac[32]` | keyed BLAKE2b-256 over header and slot |

Both structures are 256 bytes and the shared header is compiled into the kernel
and `tools/mkcrypt`, with compile-time size assertions.

## Key hierarchy

```
passphrase
  -> Argon2id(salt, m, t, p) = 96 bytes
       derived[0..64)   XTS key-encryption key (KEK)
       derived[64..96)  slot MAC key
  -> KEK decrypts a 64-byte random master key stored in a keyslot
       master[0..32)    XTS data key
       master[32..64)   XTS tweak key
```

It is the spec's hierarchy: the passphrase never becomes the disk key, and a
passphrase change only rewraps the master key in a keyslot. `mkcrypt format`
creates the master key from `/dev/urandom`, seals slot 0 as the normal
passphrase and, optionally, slot 1 with `--recovery`; the kernel tries active
slots in order and reports which one matched.

Sealing a slot:

```
slot_tweak = LE64(slot_index) || nonce[0..8)
enc_master = AES-256-XTS(KEK, block 0, master)
mac = BLAKE2b-256(key = MAC key,
                  "CRUG-SLOT-MAC\0\0\0" || LE32(slot)
                  || header[0..4096) || slot[0..256) with mac zeroed)
```

The MAC covers the immutable header and the slot itself but not the other
slots, so every slot can be sealed independently. Deleting another slot is a
denial of service, not a forgery: the header still authenticates. `unseal`
recomputes the MAC with a constant-time compare before it decrypts the master
key, and all intermediate key material is wiped on every path.

## Ciphers

**AES-256-XTS.** `kernel/crypto/aes.c`, `kernel/crypto/xts.c` and an AES-NI
`aesni.S` path implement the P1619 XEX construction:

```
T = AES(tweak_key, data_unit)
C = AES(data_key, P xor T) xor T     (and T = alpha * T between blocks)
```

`alpha` multiplies by x in GF(2^128) little-endian with the `0x87` reduction
constant, and the tweak is the data-relative 512-byte sector number in
little-endian bytes 0..8. The implementation is checked against IEEE 1619
vector 10 (XTS-AES-256) and round trips; an earlier version that skipped the
final xor was caught by that vector. `aes_init()` dispatches to AES-NI when
CPUID advertises it, otherwise the table-driven software cipher runs; the boot
line `CRUGOS: crypt aes: software`/`aes-ni` names the path in use.

**Argon2id.** `kernel/crypto/argon2id.c` implements RFC 9106 Argon2id v1.3
(type 2) with the hybrid data-independent/data-dependent addressing of pass 0
slices 0-1, BLAKE2b-512 prehash and `H'` expansion. It is verified against
libargon2 vectors for 1 and 4 lanes. Parameters come from the header and are
bounded by `m <= 1 GiB`, `t <= 64` and `lanes <= 16`; the default test volume
uses `m = 4096 KiB`, `t = 3`, `p = 1`. The workspace is allocated from the PMM
as identity-mapped pages, is wiped before it is freed, and the fill loop is
single-threaded even when `p > 1` (the lanes are still computed correctly;
there is no threading layer in the kernel yet).

**BLAKE2b-256** (`kernel/crypto/blake2b.c`) provides the keyed slot MAC.

Crypto code lives in `kernel/crypto/`; it is compiled into both the kernel and
the host tool, so the same implementation formats and opens volumes.

## Boot integration

The UEFI loader reads the optional `\crugos\cmdline.txt` from the ESP into
the `boot_info` allocation (up to 511 bytes) and sets `command_line`/
`command_line_len`. `storage_parse_cmdline()` accepts `crypt.pass=<pass>`
tokens (any whitespace-separated position) and immediately zeroes the loader
page after copying the passphrase, because it is still mapped kernel memory.

`storage_parse_cmdline()` runs first in `storage_init()`, then the root policy
is:

1. a partition named `crugroot` (unencrypted direct root);
2. a partition whose type GUID is the CrugOS encrypted-root GUID or whose name
   is `crugcrypt`: read the header, `crypt_unlock()`, mount ext4 over the
   returned `crypt0` device;
3. otherwise assemble RAID1/RAID10 `md0` from `fd00` partitions and mount it.

With no password the boot prints `CRUGOS: crypt locked (no password)`; a
header, keyslot or MAC failure prints `CRUGOS: crypt unlock failed` (variants
name the step) and storage init fails, so `main.c` prints
`CRUGOS: no root filesystem` and exits with status 35 (a deterministic boot
failure, not a panic). A successful unlock prints:

```
gpt    : crypt root 'crugcrypt' LBA 2048..26623
CRUGOS: gpt ready
CRUGOS: crypt ready (24544 sectors, keyslot 0, Argon2id m=4096 KiB t=3 p=1)
CRUGOS: crypt aes: software
CRUGOS: root device crypt0
CRUGOS: ext4 ready
```

After the boot report and before the final phase marker, `storage_lock()`
wipes the passphrase buffer, wipes the volume keys with `aes256_xts_wipe()`,
zeroes the 16 KiB bounce buffer and marks the crypt device `wiped`, so later
I/O through it fails instead of leaking plaintext. The boot report prints only
`cmdline: 23 bytes (redacted)`, never the token, and no other path logs the
passphrase.

## Host tools

`tools/mkcrypt.c` is built by `make mkcrypt` with the host C compiler and the
shared crypto sources:

```
mkcrypt format IMAGE --plain FSIMAGE --password PASS
               [--recovery PASS] [--m KIB] [--t N] [--p N]
mkcrypt extract IMAGE OUT --password PASS
```

`format` formats the encrypted partition: it writes the header, keyslot 0 and
the optional recovery slot, then encrypts the plaintext filesystem image into
the data area sector by sector. `extract` opens a slot and decrypts the data
area back to a plain image, which is how the disk gate runs `e2fsck` and
`debugfs` on an encrypted root. `scripts/mkdisk.sh OUTPUT SIZE crypt MCRYPT
PASS [RECOVERY [M_KIB]]` builds such an image: it creates a GPT partition of
type `8f1c7d2e-3b45-4c6a-9e01-c2a1b7f0d9e3` named `crugcrypt`, formats the
12 MiB data area as ext4 (1 KiB blocks, JBD2 checksum v3, `/hello.txt`), then
lets `mkcrypt` seal it. The default test parameters are `CRYPT_M=4096`,
`CRYPT_PASS=crugos-test` and `CRYPT_RECOVERY_PASS=crugos-recovery`.

`scripts/check-disk.py` gains `--crypt-tool mkcrypt --crypt-password PASS`: it
extracts the partition, runs `mkcrypt extract`, and then performs the same
`e2fsck -fn` and file checks on the plaintext image. `scripts/tamper.py`
flips one byte at a chosen image offset for the tamper gate.

## Gates

| Target | What it proves |
| --- | --- |
| `make test-crypt` | the encrypted root unlocks with keyslot 0 and the ext4/JBD2 self-tests plus `e2fsck` pass on the decrypted image |
| `make test-crypt-recovery` | the recovery passphrase opens keyslot 1 |
| `make test-crypt-wrongkey` | a wrong passphrase prints `crypt unlock failed` and `no root filesystem` and exits 35 |
| `make test-crypt-tamper` | flipping the MAC byte of keyslot 0 is rejected (exit 35) |
| `make test-crypt-ni` | the same boot with `QEMU_CPU=qemu64,+aes,+avx,+avx2` reports `crypt aes: aes-ni` |

All five are part of the release gate suite. The tamper offset
`1052776 = 2048*512 + 0x1000 + 0x68` is absolute in the image: it targets the
MAC field of keyslot 0 inside the partition.

## Performance and security tradeoffs

- Argon2id dominates encrypted boot time. On this host (TCG, no KVM) the
  default `m=4096 KiB, t=3, p=1` fills most of the ~290 ms `storage=` stage, so
  an encrypted boot takes about 380-430 ms instead of about 190 ms. An attacker who
  steals the disk pays the same cost for every password guess, so the
  parameters are a deliberate boot-time/security tradeoff; a production
  installer should offer a larger `m` (tens of MiB) and accept a slower
  unlock. AES-XTS itself is negligible beside the KDF.
- The passphrase arrives through the loader cmdline file, which is a testing
  convenience, not a production interface: firmware and anyone with ESP write
  access can replace it, and the string exists in the ESP file. The kernel
  wipes its in-memory copy and never logs it, but a Workbench-era installer
  should prompt on the console and unlink the cmdline.
- XTS is encryption, not authentication: an attacker can flip ciphertext
  sectors and the filesystem will see modified plaintext. The header and each
  keyslot are authenticated with a MAC, but there is no per-sector
  authentication tag and no Merkle tree.
- The slot MAC does not cover other slots; slot deletion or a corrupted slot
  records a denial of service. There are eight slots and no in-kernel tool to
  add, remove or rekey one yet; format-time sealing is the only writer.
- No TPM sealing, no FIDO/keyfile slot type (only passphrase and recovery
  passphrase), no discard/TRIM propagation, no suspend/hibernate, and no
  encrypted swap (the kernel has no swap).
- RAID below encryption is not implemented: a `crugcrypt` partition is
  unlocked directly on one disk, and a RAID array is mounted unencrypted.
  The spec's normal install (`raid.device -> crypt.device -> ext4`) is the
  next storage phase.

## Test vectors

- **XTS-AES-256, IEEE 1619 vector 10** (P1619-2007 Appendix B, data unit ff..):
  `1c3b3a102f770386e4836c99e370cf9bea00803f5e482357a4ae12d414a3e63b...`;
  the host suite pins the first two ciphertext blocks and the round trip.
- **Argon2id** against libargon2 for `m=32, t=1, p=1`, `m=32, t=3, p=4` (two
  inputs) and `m=64, t=2, p=1`, plus negative parameter checks.
- **crypt header/slot** host tests cover format/validate, seal/unseal, the
  recovery slot, wrong passwords, out-of-range slots, short scratch and
  tampering with the salt, `enc_master`, MAC, state and flags.
- `make test-host` runs 754 checks, including the AES, XTS, Argon2id, BLAKE2b
  and crypt-metadata vectors.
