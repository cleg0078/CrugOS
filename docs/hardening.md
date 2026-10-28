# Hardening: untrusted input on the boot path (phase H1)

Phase H1 (hardening pass 1) is the first pass that treats device- and
image-supplied bytes as hostile. CrugOS is ring-0 only today: there are no DPL3
selectors and no userland, so the syscall dispatcher is driven by the in-kernel
self-test rather than by an untrusted process. What an attacker can control
right now is the boot path: an inserted disk, a crafted partition or filesystem
image, a tampered file on the ESP (`cmdline.txt`, `kernel.elf`) and a PCIe
device that lies about its registers. Phase H1 puts those values behind a small
set of pure validators, so a check runs once in the kernel, ships with a host
test, and is proven end to end by a crafted-image gate. The review that drove
the pass (`specs/fix-p1/REPORT.md`, tree at `v0.30.0`) counted 6 High, 16
Medium and 18 Low findings; H1 closes the Highs that are reachable today and
adds the `secure_zero()` primitive the rest of the pass needed. Phase H2 then
hardened the kernel heap; see [heap.md](heap.md) for that pass and
[roadmap.md](roadmap.md) for what remains.

## The trust boundary

Everything the kernel learns from a block device, an on-disk header, a
partition table or a PCI configuration register is input, not fact. Before H1
those values were consumed where they were convenient: a superblock field
became a bitmap stride, a capability register became an MMIO offset, a register
argument became a write pointer. Each of those was correct on the images this
tree builds and wrong on an image an attacker builds.

The boundary is now one header and one translation unit:
[`include/crug/harden.h`](../include/crug/harden.h) declares seven pure
validators, [`kernel/lib/harden.c`](../kernel/lib/harden.c) implements them,
and [`tests/host/test_harden.c`](../tests/host/test_harden.c) tests every
boundary on the host, without QEMU or a disk. A validator takes plain integers
(and at most one pointer), reads no global state, allocates nothing, returns a
Boolean and never snaps a value into range: a call that returns false is a
refusal, not a repair. Callers therefore fail closed with a named marker and a
deterministic exit 35 instead of continuing with a clamped value.

The rule for new code is: a value that came from outside the kernel is checked
once, at the point it enters, and the checked value is what the rest of the
function uses. Re-checking a derived index at the point of use is still allowed
and sometimes required (the ext4 block allocator does exactly that), but it is
not a substitute for the entry check.

## The validators

| Validator | What it bounds | Where it is called |
| --- | --- | --- |
| `harden_range_ok(addr, len, limit)` | a register-supplied buffer: `len == 0` is always fine, `addr == 0` and `len > limit` are rejected, otherwise `addr <= limit - len` so the addition cannot wrap | `kernel/core/syscall.c` for `SYS_PORT_SEND` / `SYS_PORT_RECV` with `HARDEN_IDENTITY_LIMIT` (4 GiB, the identity map) |
| `harden_payload_ok(data, len)` | the message payload pointer against its length: a NULL pointer with a non-zero length is rejected | `kernel/core/msgport.c` `msgport_send()` |
| `harden_extent_header_ok(node, node_bytes)` | an ext4 extent header: magic `0xF30A`, entries not above `eh_max`, depth at most 5, and `12 + eh_max * 12` inside the bytes the node really owns | `kernel/fs/ext4.c` `extent_map()`, `extent_add()` and the root-inode check at mount; `node_bytes` is 60 for the inline root in the `i_block` area and `block_size - 12 - csum` for a leaf |
| `harden_ext4_geometry_ok(block_size, bpg, ipg, blocks_count, inodes_count)` | the superblock geometry: `block_size` a power of two of at least 1024, every count non-zero, and `bpg` / `ipg` at most `block_size * 8` (one bitmap block of bits, no more) | `kernel/fs/ext4.c` `ext4_probe()`, before the superblock checksum is verified |
| `harden_nvme_doorbell_ok(bar_size, stride, qid, cq)` | an NVMe doorbell offset: `stride` a power of two of at least 4, and `0x1000 + (2 * qid + (cq ? 1 : 0)) * stride + 4` at most `bar_size` | `drivers/block/nvme.c` `nvme_doorbell()` and `nvme_setup()` |
| `harden_kdf_cost_ok(m, t, lanes, min_m, max_m, min_t, max_t)` | an Argon2id cost tuple: lanes 1..16, `m` in range with `m >= 8 * lanes` (Argon2 needs 8 KiB per lane), `t` in range | `kernel/block/crypt.c` `crypt_unlock()` with the unlock policy constants; the format ceiling is enforced separately by `crypt_meta_validate()` (`kernel/block/crypt_meta.c`), which already requires `lanes <= 16` and `8 * lanes <= m <= 1 GiB` |
| `harden_volume_ok(volume_sectors, parent_sectors, offset_sectors)` | the crypt volume against its parent partition: `offset <= parent` and `volume <= parent - offset` | `kernel/block/crypt.c` `crypt_unlock()` against the partition the header was read from |

`harden_range_ok()` is the only one that takes a pointer, and it takes it as an
integer; nothing is dereferenced inside a validator.

## Syscall pointer gate

Two of the High findings were in the message-port syscalls. `SYS_PORT_RECV`
ended with `*(u64 *)frame->rsi = type`, writing a u64 wherever the caller
pointed with no check at all. `SYS_PORT_SEND` and `SYS_PORT_RECV` also copied
up to `MSGPORT_MAX_PAYLOAD` (1 KiB) through a caller-chosen register with no
range check, so a length and a pointer could describe anything in the 4 GiB
identity map - or outside it, where the access faults in ring 0. A third bug
lived one layer down: `msgport_send()` accepted `data == 0` with `len > 0`
and enqueued the message anyway, so a receive copied uninitialised kernel heap
into a caller buffer (an information leak that needs no faults at all).

The fix is `kernel/core/syscall.c`. `SYS_PORT_SEND` validates the `rdx` /
`r10` buffer against `HARDEN_IDENTITY_LIMIT`; `SYS_PORT_RECV` validates the
same buffer and additionally accepts `rsi == 0` (meaning "do not report a
sender") or requires an 8-byte writable range, and only then stores the type.
Every rejection returns the new `SYS_EFAULT` (`-14`, in
[`include/crug/syscall.h`](../include/crug/syscall.h)) and copies nothing.
`msgport_send()` calls `harden_payload_ok()` and refuses a NULL payload with a
non-zero length, which also makes the `data == 0, len > 0` leak impossible for
any caller, not just the syscall.

The in-kernel self-test exercises the gate in `syscall_pointer_gate()`: a
buffer at address 0, a buffer at `0x100000000` (one byte past the identity
map), a valid buffer that must return 0, and a receive with and without a
destination. A failure prints
`CRUGOS: syscall selftest failed (pointer gate)`; success stays inside the
normal `CRUGOS: syscall selftest ok` line.

## Image gate

`make test-hardening` is the 44th gate. It is not a fuzzer: it is a
reproducible adversarial boot. Its control case does not hardcode a phase
string: [`scripts/run-hardening.py`](../scripts/run-hardening.py) matches any
`CRUGOS: phase <id> boot ok` line with
`CONTROL_PHASE_RE = re.compile(r"CRUGOS: phase [0-9a-z]+ boot ok")`, alongside
the unchanged `CRUGOS: ext4 ready`, `CRUGOS: storage ready`,
`CRUGOS: writable storage ok` and `CRUGOS: selftest ok` markers, so the H1 image
gate keeps working across phase bumps. The script takes a pristine test image
and the pristine encrypted image, copies the pair per case, patches exactly
one field with `struct.pack_into` (recomputing an inode checksum where the case needs a self-consistent inode), boots the patched
pair under QEMU, and asserts both the exit status and the marker the kernel
must print.

| Case | Patch | Required marker | Exit |
| --- | --- | --- | --- |
| `control` | none | `CRUGOS: phase h3 boot ok` (any phase id matches), `CRUGOS: ext4 ready`, `CRUGOS: storage ready`, `CRUGOS: writable storage ok`, `CRUGOS: selftest ok` | 33 |
| `geometry-bpg` | superblock `+0x20` (`s_blocks_per_group`) = 100000 | `CRUGOS: ext4 mount rejected (geometry)` | 35 |
| `geometry-ipg` | superblock `+0x28` (`s_inodes_per_group`) = 100000 | `CRUGOS: ext4 mount rejected (geometry)` | 35 |
| `root-extent` | root inode `eh_max` (`inode + 44`) = 5, inode checksum recomputed | `CRUGOS: ext4 mount rejected (root extent)` | 35 |
| `kdf-high` | crypt header `+0x18` (`kdf_m_cost_kib`) = 131072 | `CRUGOS: crypt unlock failed (kdf policy)` | 35 |
| `kdf-low` | crypt header `+0x18` = 512 | `CRUGOS: crypt unlock failed (kdf policy)` | 35 |
| `volume-huge` | crypt header `+0x38` (`volume_sectors`) = `0x0000FFFFFFFFFFFF` | `CRUGOS: crypt unlock failed (volume exceeds partition)` | 35 |

Every case also forbids `CRUGOS: panic` and `trap   : page fault`, and every
hostile case must not print `CRUGOS: ext4 ready` or `CRUGOS: storage ready`:
the point is a clean refusal, not a crash that happens to stop the boot. A
QEMU timeout (exit 124) fails the case; a still-running guest is not evidence of
a rejection.

## Ordering

The order of the two ext4 checks is deliberate. `harden_ext4_geometry_ok()`
runs in `ext4_probe()` before the `metadata_csum` superblock checksum is
verified, so a crafted `s_blocks_per_group` or `s_inodes_per_group` is
rejected without the attacker having to also fix the superblock checksum. The
root extent check is the mirror image: it runs after `read_inode()`, so
reaching `CRUGOS: ext4 mount rejected (root extent)` requires a valid inode
checksum, and the crafted image has to be self-consistent right up to the
validator. Geometry is a superblock question and is answered from the
superblock; the extent header is an inode question and is answered only after
the inode authenticated.

Either rejection fails `storage_init()`, so the kernel prints
`CRUGOS: no root filesystem` and exits QEMU with status 35 - the same
deterministic boot failure used by every other fail-closed storage path, not a
panic.

## KDF and volume policy

The on-disk crypt format and the kernel's unlock policy are intentionally
different numbers. The format ceiling (`CRUG_CRYPT_MAX_M_COST_KIB` = 1 GiB,
`CRUG_CRYPT_MAX_T_COST` = 64, `CRUG_CRYPT_MAX_LANES` = 16) says what a header
is allowed to describe; the unlock policy says what this kernel is willing to
pay before it has authenticated anything. A hostile header could otherwise
demand 1 GiB of Argon2id memory at `t = 64`: a 15-20 minute pre-authentication
stall on this host, and an out-of-memory boot on a small guest. The kernel now
enforces `m` 1024..65536 KiB, `t` 2..8, lanes 1..16 and `m >= 8 * lanes` at
unlock time. `t = 1` and `m = 512` are refused as well: the policy fails
closed on trivially cheap headers instead of deriving a key from them.

`tools/mkcrypt` is deliberately unchanged, and so is the format: the on-disk
format still accepts up to 1 GiB and `t = 64`, so a volume formatted on a
larger machine stays readable by the format and the host tool, and the default
gate images keep `CRYPT_M=4096` (4096 KiB, `t = 3`, one lane) inside the
policy. Tightening the format would have turned a boot-policy decision into a
storage-format break.

The volume bound is the same idea for the data area:
`harden_volume_ok(h->volume_sectors, parent->sectors, offset_sectors)` requires
the header's volume to fit inside the partition the header was read from, so a
crafted `volume_sectors` cannot make the block layer address sectors past the
end of the parent device.

Both refusals wipe the 16 KiB header copy with `secure_zero()` before they
return, print their marker (`CRUGOS: crypt unlock failed (kdf policy)` or
`CRUGOS: crypt unlock failed (volume exceeds partition)`), and leave the
passphrase and key material on the existing zeroization paths.

## Wiping secrets

`secure_zero(void *dst, usize n)` in [`kernel/lib/string.c`](../kernel/lib/string.c)
overwrites through a `volatile u8 *`. A plain `memset()` on a buffer that is
freed or goes out of scope is a dead store: the compiler is entitled to remove
it, and with LTO or a smarter optimiser it eventually will. The volatile store
is the barrier that keeps the wipe in the object code. It replaces the plain
wipes in the crypt header, bounce, scratch and master-key paths and in
`storage_wipe()` (`kernel/fs/storage.c`), so every secret the kernel held is
erased through the same primitive.

The loader has one more copy. `boot/efi/main.c` reads the optional
`\crugos\cmdline.txt` into a pool allocation and passes it to the kernel in
`boot_info`; after parsing, the kernel wipes the `boot_info` copy, but the
loader's page-pool copy is still in `EfiConventionalMemory` and would keep
`crypt.pass=` in USABLE memory for whatever runs next. The loader therefore
zeroes its own copy immediately after it has copied the line into `boot_info`,
before it calls `ExitBootServices`. (The ESP file itself still contains the
token: `crypt.pass=` remains an automation convenience, not a production
interface.)

## Running the gate

```
make test-hardening
```

The target depends on `esp` and the two pristine images, then runs
`scripts/run-hardening.py` with the disk, the encrypted disk, the passphrase,
the serial-log directory, the expected exit code and `HARDEN_TIMEOUT_SEC`
(default 120 seconds per crafted image). Each case copies the pristine pair, so
one case cannot contaminate the next. `--keep` keeps the patched images under
`<serial-log dir>/hardening/` for inspection with `debugfs` or `mkcrypt
extract`.

A green run ends like this:

```
ok: harden control boot (exit 33)
ok: harden geometry-bpg -> CRUGOS: ext4 mount rejected (geometry) (exit 35)
ok: harden geometry-ipg -> CRUGOS: ext4 mount rejected (geometry) (exit 35)
ok: harden root-extent -> CRUGOS: ext4 mount rejected (root extent) (exit 35)
ok: harden kdf-high -> CRUGOS: crypt unlock failed (kdf policy) (exit 35)
ok: harden kdf-low -> CRUGOS: crypt unlock failed (kdf policy) (exit 35)
ok: harden volume-huge -> CRUGOS: crypt unlock failed (volume exceeds partition) (exit 35)
ok: hardening pass 1 rejected 6 crafted image(s) (control boot exit 33)
```

## What is not covered

- Report item 3 is now phase H3, not this pass: every non-`#BP` exception
  builds one bounded report and is classified by `trap_classify()` -- the
  user-killable set is `TRAP_USER` for a CPL3 frame and `TRAP_FATAL` from ring 0,
  `#BP` stays handled, and everything else is fatal -- and `#DF`, `#NMI`, `#MC`
  and `#DB` run on their own IST stacks, with `__df_stack` grown to 16 KiB. See
  [traps.md](traps.md) for the report contract, the classification matrix, the
  `trap.test=` recipes and the `make test-trap` gate. What stays open is the
  ring-3 half: there is no DPL3 process yet, so `rsp0` still points at the boot
  stack, a `TRAP_USER` fault panics as `user-mode <name>` instead of killing a
  thread, and the crafted-image gate still avoids the panic by construction
  rather than by policy.
- Heap hardening is now phase H2, not this pass: page cookies, validated slab
  headers, the doubly linked large-block list and the read-only integrity
  checker are documented in [heap.md](heap.md), which also lists what the
  checker does not cover. The allocator's free-list `next` and block magic
  (report items F7/F8) are addressed there.
- Weak EFI entropy is only surfaced, not fixed: the loader still trusts the
  firmware's RNG for the entropy it mixes into the boot report, and there is no
  way for the operator to see that a firmware returned a constant.
- The AHCI timeout path still reuses its bounce buffer after a failed command;
  the crypt pass bounds the buffers it owns but AHCI has no equivalent
  ownership check yet.
- There is no host-side fuzzer for ext4, GPT or the crypt format. H1 proves
  six specific crafted images (plus an unpatched control); a fuzzer that mutates whole images and runs
  the mount path in a harness is the next step.
- Several Mediums become Critical once ring 3 exists: today a bad pointer in
  the syscall ABI is caught by the in-kernel self-test, but a DPL3 process
  would reach the same dispatcher directly. The validators are the part that
  survives the move; the policy around them is not written yet.
