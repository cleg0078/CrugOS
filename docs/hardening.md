# Hardening: untrusted input on the boot path (phase H1)

Phase H1 (hardening pass 1) is the first pass that treats device- and
image-supplied bytes as hostile. At the time of H1 CrugOS was ring-0 only: there
were no DPL3 selectors and no userland, so the syscall dispatcher was driven by
the in-kernel self-test rather than by an untrusted process. Phase U1 has since
crossed into ring 3, so a `DPL3` program now reaches that dispatcher directly
through the copy boundary described below, and the validators are still the part
that survives the move. What an attacker could control at the start was the
boot path: an inserted disk, a crafted partition or filesystem image, a tampered
file on the ESP (`cmdline.txt`, `kernel.elf`) and a PCIe
device that lies about its registers. Phase H1 puts those values behind a small
set of pure validators, so a check runs once in the kernel, ships with a host
test, and is proven end to end by a crafted-image gate. The review that drove
the pass (`specs/fix-p1/REPORT.md`, tree at `v0.30.0`) counted 6 High, 16
Medium and 18 Low findings; H1 closes the Highs that are reachable today and
adds the `secure_zero()` primitive the rest of the pass needed. Phase H2 then
hardened the kernel heap and phase H3 built the trap policy; phase U1 crossed
into ring 3. See [heap.md](heap.md), [traps.md](traps.md) and
[userspace.md](userspace.md) for those passes, and [roadmap.md](roadmap.md) for
what remains.

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

## The user copy boundary

Phase U1 is where the syscall pointer gate stops being an in-kernel convenience
and becomes a real trust boundary. A ring-3 pointer is not an address in the
4 GiB identity map, so `harden_range_ok()` cannot bound it: the user window
(`USER_BASE` 0x0000008000000000, `USER_TOP` 0x0000010000000000) is the whole
universe a `DPL3` program may name. `harden_user_range_ok(va, size)` is pure
window arithmetic -- at most one page, page aligned, inside the window -- and it
is independent of SMAP, because a CPU without SMAP still must not let a user
pointer reach kernel memory by arithmetic alone. `harden_user_segment_ok()` and
`harden_user_entry_ok()` extend the same rule to ELF segments and the entry
point, so the loader refuses a module that names a kernel address.

The window check answers "is this allowed at all"; it does not answer "is this
mapped". `kernel/mm/uaccess.c` therefore follows the check with a walk of the
active address space: `vm_translate()` reads the root `vm_space_activate()` last
installed in `kernel/mm/vm.c`'s `g_root`, so the walk always sees the PML4 of the
thread being served. Every page in the range must resolve, and an in-window but
unmapped page returns `UACCESS_EFAULT` (`-14`) instead of taking a kernel `#PF`
while the kernel is dereferencing a caller's pointer. Only the actual copy is bracketed by `stac`/`clac`, and only
when CPUID exposes SMAP, so the window around the access is as small as the
instruction allows. `uaccess_user_read()`, `uaccess_user_write()` and
`uaccess_user_str()` are the only copies the syscall layer uses for CPL3
callers; the in-kernel path keeps the identity-map check, which is why the
self-test above is still meaningful. The syscall layer copies through kernel
bounce buffers (`SYS_WRITE` moves at most 4096 bytes in 256-byte chunks), so no
user pointer is dereferenced twice and a fault part-way through reports one
clean error. Phase U3 adds the file syscalls on the same boundary: `SYS_OPEN`
passes its path through `uaccess_user_str`, `SYS_READ`/`SYS_WRITE` move the
caller's buffer through `uaccess_user_read`/`uaccess_user_write` and
`SYS_LISTDIR` writes the listing back with `uaccess_user_write`, each through a
kernel bounce buffer, while a CPL0 caller keeps the `harden_range_ok()`
identity path. See [userspace.md](userspace.md) for the window, the page walk
and the syscall split.

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
| `control` | none | `CRUGOS: phase u3 boot ok` (any phase id matches), `CRUGOS: ext4 ready`, `CRUGOS: storage ready`, `CRUGOS: writable storage ok`, `CRUGOS: selftest ok` | 33 |
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

Phase H4 adds the complementary host-side gate. `make test-fuzz` is not a
crafted case and not a boot: per case it mutates a copy of the pristine disk
image -- and the 256-byte crypt volume header -- forks a child and runs
`gpt_parse()`, `ext4_mount()`, the read path and the write path in a host
harness before the kernel ever sees the bytes, so the fail-closed property above
is also probed with thousands of mutations no hand-written case covers. It
cannot replace this gate: it runs in ring 3, never boots QEMU and starts every
mutation from a clean image, so the crafted-image boot stays the only place
where a rejection proves the kernel prints the right marker and exits 35. See
[fuzzing.md](fuzzing.md) and gate 47 in [build.md](build.md).

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

- Report item 3 is now phase H3, and phase U1 closed its ring-3 half: every
  non-`#BP` exception builds one bounded report classified by `trap_classify()`
  -- the user-killable set is `TRAP_USER` for a CPL3 frame and `TRAP_FATAL` from
  ring 0, `#BP` stays handled, and everything else is fatal -- and `#DF`, `#NMI`,
  `#MC` and `#DB` run on their own IST stacks, with `__df_stack` grown to 16 KiB.
  U1 makes `rsp0` per-thread and turns a `TRAP_USER` fault into a per-thread
  kill instead of `panic("user-mode <name>")`, so a user program cannot take the
  machine down by faulting. What stays open is the address-space half: a
  user-frame `#DF`, `#NMI` or `#MC` still panics (there is no policy for
  abandoning a whole address space), and every user program shares the kernel
  CR3, so two programs are not isolated from each other. See [traps.md](traps.md)
  and [userspace.md](userspace.md) for the report contract, the classification
  matrix, the `trap.test=` recipes and the `make test-trap` / `make test-user`
  gates.
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
- A host-side fuzzer now exists but is not this gate: phase H4's
  `make test-fuzz` mutates whole disk images (and the crypt volume header) in a
  forked host process and drives the real `kernel/fs/ext4.c`, `kernel/fs/gpt.c`
  and `kernel/block/crypt_meta.c`, 20000 ext4 cases plus 2000 header cases per
  sweep, with about 10580 cases mounting, 10535 walking a path, 2594 writing
  and 0 crashes. It stops where a host process cannot follow: no ring-0-only
  faults, no QEMU devices, no dirty-journal replay and no concurrency. See
  [fuzzing.md](fuzzing.md).
- Several Mediums became live when ring 3 landed: a bad pointer in the syscall
  ABI now comes from a `DPL3` process, not only from the in-kernel self-test.
  Phase U1 answers the pointer half with the user copy boundary above and the
  fault half with the per-thread kill path. Phase U2 gives every ring-3 thread
  its own PML4, so two programs run at once with private windows and the same
  virtual address resolving to different frames, and phase U3 puts the file
  syscalls on the same copy boundary, so a ring-3 descriptor names a kernel
  object the caller cannot corrupt (the table is per thread). What remains is
  that slot 0
  of every space is still the shared kernel identity map: this is isolation
  between user programs, not kernel-address isolation, and there is no
  per-process resource accounting.
