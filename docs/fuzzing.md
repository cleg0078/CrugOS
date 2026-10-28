# Host mount-path fuzzing

Phase H4 adds a fuzzer for the one path that a corrupt image reaches first: the
kernel's own storage mount path. It is a host program, `tests/host/fuzz_mount.c`,
that links the real `kernel/fs/ext4.c`, `kernel/fs/gpt.c`, `kernel/fs/journal.c`,
the block layer, the crypt header parser and the lib helpers they call, mutates
a real `scripts/mkdisk.sh` image, and mounts every mutant in a forked child. A
crash is a finding; `make test-fuzz` -- the 47th gate -- makes the sweep part of
the release.

No kernel C code changed for H4: the kernel image, the `BootInfo` v1 ABI and
every boot path are the same as `v0.33.0`. The release adds the harness, its own
link line in `tests/host/Makefile`, the gate and the version bump. That is the
point: the gate can only say something about the storage stack because it
compiles the storage stack.

The harness is not a kernel. It runs in ring 3 on the host, with host stubs for
`kprintf()` (silent unless `--verbose-kernel`), `panic()` (print and exit 97) and
the `pmm_*` page allocator (a 32 MiB static arena), and it serves the image
through a bounds-checked in-memory block device. Those stubs are also its main
limitation; see [Limits and next steps](#limits-and-next-steps).

## Why a host-side fuzzer

The image gates before this one are curated. `make test-hardening` boots six
hand-written hostile images plus a control, and `make test-heap` corrupts
specific heap fields on purpose. Every one of those cases exists because someone
decided that this byte matters. A fuzzer explores the mutations nobody wrote
down: any region of a real, mountable filesystem, at any offset, with one to
four simultaneous edits and values chosen to sit on the edges of a field.

- **A boot is too expensive to repeat.** One QEMU boot takes seconds; 22000 of
  them per gate would take hours. The harness runs the same mount path
  in-process at roughly 1 ms per case, so a 22000-case sweep finishes in about
  23 seconds and can run on every release.
- **It is the shipping source, not a model of it.** The gate links the same
  translation units the kernel links for ext4, GPT, journal replay, the block
  layer, the crypt header and the lib routines behind them, compiled from the
  same tree with the same Makefile flags. A crash is a bug in that code.
- **It is deterministic.** All randomness comes from one seeded xorshift64, so
  the same `--seed` and the same case count replay the same plans in the same
  order; a failure prints the plan and dumps the mutant so it can be inspected.
- **Failure is loud.** A child that dies on a signal, hits the kernel panic hook
  or returns an unexpected exit code fails the run; the gate does not print a
  green line over a partial sweep.

The bet is that the interesting failures are reachable without privileges: a
crafted superblock, inode, extent tree or directory block that sends the mount
path off the end of one of its buffers on a normal C function. That class of bug
does not need ring 0 to reproduce.

## What gets mutated

Both images come from `scripts/mkdisk.sh`, which `make test-fuzz` rebuilds from
scratch before it starts, so a stale `build/x86_64/disk.img` can never decide the
gate:

- `build/x86_64/disk.img` -- the default 16 MiB (`DISK_SIZE_MB`) GPT image with a
  single 24576-sector (12 MiB) ext4 partition named `crugroot` starting at LBA
  2048, which is byte 1048576. The filesystem carries `/hello.txt`,
  `/selftest/b.txt` and a JBD2 journal with checksum v3.
- `build/x86_64/crypt.img` -- the same geometry in crypt mode: the partition is
  named `crugcrypt`, an AES-256-XTS volume header (16 KiB) sits at the partition
  start, and the ext4 filesystem lives in the data area after it. The harness
  copies the first 256 bytes -- `struct crug_crypt_header` -- out of the volume
  and fuzzes that structure on its own.

The partition is located with the real `gpt_parse()`, not with an offset baked
into the harness. Callers test `< 0` for failure, because the function returns
the number of partitions on success; that detail is worth knowing because the
harness got it wrong once (see [The pristine-image control
mount](#the-pristine-image-control-mount)). The first partition entry with a
non-zero `first_lba` and `last_lba >= first_lba` becomes the target.

Every mutation targets one of six regions of the ext4 partition. The weights are
not uniform because a superblock byte is far more likely to reach deep code than
a byte of free space in the last block group:

| Region | Start | Length | Weight |
| --- | --- | --- | --- |
| `superblock` | partition offset + 1024 bytes | 1024 B | 22 |
| `group-desc` | `(first_data + 1) * block_size` | `desc_size * 4` (four descriptors) | 14 |
| `inode-table` | block given by group 0's `bg_inode_table` | `inode_size * 32` (the first 32 inodes) | 16 |
| `root-dir` | inode 2's extent-map root block | one block | 16 |
| `metadata` | partition offset | 64 KiB | 17 |
| `partition` | partition offset | `part_sectors * 512` (the whole partition) | 15 |

The weights add up to 100, so the weight is literally a percentage of the picks.
`build_regions()` reads `block_size`, `inode_size`, `desc_size` and
`first_data_block` out of the superblock, clamps them into the ranges ext4
allows (`inode_size` 128..4096, `block_size` 1024..65536, `desc_size`
32..1024), and only adds the `root-dir` region when inode 2 really carries an
extent tree (magic `0xF30A`, depth 0, at least one entry). A region that starts
past the end of the image or is shorter than 16 bytes is dropped, and at most
eight are kept (`REGION_MAX`), in this order.

The crypt leg has no region list: it always mutates the 256-byte header, and the
failure plan calls that region `crypt-header`.

## The case pipeline and the exit-depth encoding

One case is one plan applied to one fresh fork:

1. `build_plan()` draws the region, the access mode and the mutation count. A
   crypt plan has one region, one to three mutations and no read-write mode; an
   ext4 plan picks a region by weight, picks read-write in one case out of four
   (`rnd % 4 == 0`) and picks one to four mutations (`1 + rnd % 4`).
2. The harness forks. The child inherits the loaded image copy-on-write, so it
   can scribble on its own pages while the parent's image stays pristine.
3. The child applies the plan to `image + region.off` with `region.len` as its
   bound, arms `alarm(g_timeout_sec)`, and runs the stack.
4. A crypt child copies the 256-byte header onto its stack, applies the plan
   there, then calls `crypt_meta_validate()` and `crypt_meta_scratch_bytes()`.
5. An ext4 child runs `gpt_parse()` -> `ext4_mount()` (with `EXT4_MOUNT_RW` in
   the read-write quarter) -> `ext4_lookup("/")`, `ext4_list("/")` and
   `ext4_read_file("/hello.txt")` / `("/selftest/b.txt")` -> the optional
   write path -> `ext4_umount()`.
6. The child's exit code is how far it got. The parent waits, counts the depth,
   and only treats a signal, the panic code or an exit code above 3 as a
   finding.

| Exit | Meaning | Counter |
| --- | --- | --- |
| `0` | the mount was rejected, or the crypt header was rejected | -- |
| `1` | `ext4_mount()` succeeded | mounted |
| `2` | `/hello.txt` was looked up and read with a non-zero size | walked a path |
| `3` | a write operation succeeded | wrote |

Depth 2 needs the lookup *and* the read to return data, so a mutant that mounts
an empty or unreadable root still counts as depth 1. The read-only cases still
walk the namespace: `/` is looked up and listed and `/selftest/b.txt` is read
even when `/hello.txt` is gone.

The read-write path is deliberately not one call. Once the mount is ready it
does all of this, in order, and records `wrote` if either `ext4_create()` or
`ext4_mkdir()` succeeded:

- `ext4_create("/fz-a", 0644)`, then write 512 bytes of a fixed pattern
  (`data[k] = (k * 7 + 3) & 0xFF`), `ext4_truncate("/fz-a", 64)` and
  `ext4_fsync()`.
- `ext4_mkdir("/fz-d", 0755)`, then create and write 200 bytes to
  `/fz-d/x`, rename it to `/fz-d/y`, unlink it and `ext4_rmdir("/fz-d")`.
- `ext4_rename("/fz-a", "/fz-b")`, `ext4_unlink("/fz-b")` and a 32-byte
  rewrite of `/hello.txt`.

That covers block allocation, bitmap updates, directory insert and removal,
extent growth, truncation, the journal commit and an fsync, all against the
mutated metadata. A mutant that corrupts an inode's block map but never writes
would otherwise look harmless.

Cases run one at a time, one child at a time, with no threads and no concurrent
writers. The timeout is per case, and a case that hangs is killed by `SIGALRM`,
which the parent reports like any other fatal signal.

## Mutation kinds

`build_plan()` chooses one of eight mutation kinds per edit, uniformly:

| # | Kind | Width | Effect |
| --- | --- | --- | --- |
| 0 | `MUT_BYTE` | 1 B | XOR the low byte of the value into the first byte (`dst[0] ^= value & 0xFF`) |
| 1 | `MUT_FF` | 16 B | fill with `0xFF` |
| 2 | `MUT_ZERO` | 16 B | fill with zero |
| 3 | `MUT_U16` | 2 B | little-endian 16-bit write |
| 4 | `MUT_U32` | 4 B | little-endian 32-bit write |
| 5 | `MUT_U64` | 8 B | little-endian 64-bit write |
| 6 | `MUT_SPLICE` | 16 B | copy 16 bytes from a random offset in the same region |
| 7 | `MUT_FILL` | 16 B | copy 16 freshly drawn random bytes |

A one-byte write is an XOR rather than an assignment so that the original byte
still matters: flipping one bit distinguishes a field that is tested for an exact
value from one that is merely masked.

The value for kinds 0 and 3..5 comes from `edge_value()`, which is uniform over
ten outcomes:

| Draw | Value |
| --- | --- |
| 1 in 10 | `0` |
| 1 in 10 | `1` |
| 1 in 10 | `2` |
| 1 in 10 | `0xFFFF` |
| 1 in 10 | `0x7FFFFFFF` |
| 1 in 10 | `0x80000000` |
| 1 in 10 | `0xFFFFFFFF` |
| 1 in 10 | `0xFFFFFFFFFFFFFFFF` |
| 1 in 10 | `0x1000` (one page) |
| 1 in 10 | a full random 64-bit value |

Those nine constants are the values the storage code compares against: a
plausible count of zero, one or two, a 16-bit field at its maximum, a signed
maximum and its sign bit, a 32-bit all-ones field, a 64-bit all-ones field, and
a page size. The random tenth keeps the sweep from being only about those
edges.

Offsets are drawn from `[0, region_len - 16)` so a 16-byte edit always fits,
except in regions shorter than 16 bytes (which are never added) and the crypt
header, where the span is the 256-byte structure. The applied length is clamped
to the bytes left in the region; `MUT_U16`/`U32`/`U64` are skipped when the
region cannot hold their width.

All of it is one xorshift64 (`x ^= x << 13; x ^= x >> 7; x ^= x << 17`) seeded
with `0x9E3779B97F4A7C15 ^ (seed * 0x9E3779B1)` and re-seeded to the constant if
that yields zero. A plan therefore depends only on the seed, the case index and
the region list of that image; nothing reads the clock, the PID or the address
space.

## Crash detection, the detector self-test and replaying a dumped image

### What counts as a crash

`run_case()` fails the whole run when the child terminated by a signal, when it
exited with `PANIC_EXIT` (97, the harness `panic()` stub), or when it exited with
a code above 3. Each has its own message and a label that says whether the case
was an ext4 or a crypt header case:

```
FAIL: ext4 case 12345 died on signal 11
FAIL: ext4 case 12345 hit the kernel panic hook
FAIL: crypt case 42 exited with status 7
```

After the message the parent prints the plan it built, so the failure is
self-describing even without the dump:

```
  region=inode-table rw=0 mutation(s)=2
  mutation 0: off=0x3840 kind=4 value=0x80000000
  mutation 1: off=0x1f0 kind=0 value=0x5
```

Offsets are relative to the region, and `kind` is the numbering in the mutation
table above. The kernel's `panic()` is a stub that prints `FUZZ panic: ...` and
exits 97 rather than hanging, so a panic reached through crafted metadata is
reported instead of stalling the gate. A hang inside the mount path is not
pretended to be anything else: the alarm fires, the child dies on `SIGALRM` and
the case is reported as a signal death.

### The detector self-test

A crash detector that cannot detect a crash is worse than none, because it
prints the same green line. `--selftest` forks a child that writes through a
volatile null pointer and requires a fatal signal:

```
ok: fuzz selftest detected the injected crash (signal 11)
```

If the child does not die on a signal the harness prints `FAIL: the harness did
not report an injected crash` and exits 1. `make test-fuzz` runs this first
(`--cases 1 --selftest`; the self-test returns before the case loop), so the
gate cannot be green unless the detector worked on that machine.

### The pristine-image control mount

Before any mutation, `control_mount()` mounts the untouched image read-only and
requires `/hello.txt` to be looked up and read. If that fails the harness prints
`fuzz: the pristine image does not mount` or `fuzz: the pristine image has no
readable /hello.txt` and exits 1 without running a single case. A harness that
was wired to the wrong bytes, or a `mkdisk.sh` change that moved the layout, is
therefore a harness error and never a kernel finding.

That check exists because the harness shipped with exactly that bug. The first
run against a pristine image failed with `fuzz: the image has no GPT partition`
on an image whose GPT was fine: `discover_partition()` tested `gpt_parse()` for
zero when the function returns the partition count, so one partition was treated
as no partition. The fix was to test `< 0`, and the control mount makes that
class of wiring bug fail in one second instead of after a green-looking sweep.
`fuzz: the image has no GPT partition` now means what it says.

### Replaying a dumped case

With `--crash-dir` set (the gate passes `build/x86_64/fuzz`), a finding writes
the mutant next to the plan: `fuzz-ext4-<N>.img`, the full image with the plan
applied, or `fuzz-crypt-<N>.img`, the mutated 256-byte header. Three ways to go
from there:

- The plan is reproducible from the seed. `build/fuzz-mount --image ... --cases
  N --seed 1337` rebuilds the same plans, and the failing case is the one whose
  index matches the message.
- The ext4 dump is a complete disk image, so `make run DISK_IMG=build/x86_64/fuzz/fuzz-ext4-12345.img`
  boots the kernel against it and gives a real serial log, including which
  report line the mount path reached.
- The filesystem can be inspected without the harness at all: `e2fsck`, `dumpe2fs`
  and `debugfs -R "stat /hello.txt"` say which field the plan broke, which is
  usually faster than reading the plan.

## Running it

### The gate

`make test-fuzz` is the 47th gate and the only supported entry point for a
release. It:

1. removes `build/x86_64/disk.img` and `build/x86_64/crypt.img`;
2. rebuilds both with `make disk crypt-disk`;
3. builds the harness with `make -C tests/host fuzz`, which produces the
   separate `tests/host/build/fuzz-mount` binary (not the unit-test binary: the
   harness installs its own `pmm_*` and panic stubs);
4. runs the self-test, then
5. sweeps `FUZZ_CASES` ext4 cases and `FUZZ_CRYPT_CASES` header cases with
   `FUZZ_SEED` and `FUZZ_TIMEOUT_SEC`, and fails on the first finding.

It never boots QEMU, so its output has no kernel banner and no
`CRUGOS: phase u1 boot ok` line. The green case prints:

```
ok: fuzz selftest detected the injected crash (signal 11)
fuzz: image /home/cleg/MyDev/CrugOS/build/x86_64/disk.img (16777216 bytes), partition at byte 1048576 (24576 sectors)
fuzz: 20000 ext4 case(s), 2000 header case(s), seed 1337, timeout 10 s
fuzz: 10580 case(s) mounted, 10535 walked a path, 2594 wrote
ok: fuzz 20000 ext4 case(s) and 2000 crypt header case(s), 0 crash(es)
```

The self-test line comes from the first invocation and the rest from the second;
the counts are the ones this tree produced with the default knobs. The sweep
takes about 23 seconds on the development host, which is what makes the gate
cheap enough to run every time.

### Knobs

| Variable | Default | Meaning |
| --- | --- | --- |
| `FUZZ_CASES` | `20000` | ext4 mutation cases per sweep |
| `FUZZ_CRYPT_CASES` | `2000` | crypt-header mutation cases per sweep |
| `FUZZ_SEED` | `1337` | seed for the xorshift64; any two runs with the same knobs replay identically |
| `FUZZ_TIMEOUT_SEC` | `10` | per-case `alarm()` budget; a case that exceeds it dies on `SIGALRM` |

### Direct invocation

For a focused run the binary takes the same arguments, plus `--verbose-kernel`
and `--selftest`:

```
usage: fuzz-mount --image PATH [--crypt-image PATH] [--cases N]
                 [--crypt-cases N] [--seed N] [--timeout SEC]
                 [--crash-dir DIR] [--verbose-kernel] [--selftest]
```

`--image` is required; `--cases` defaults to 1000, `--crypt-cases` to 0, `--seed`
to 1337 and `--timeout` to 10 seconds. `--selftest` runs the injected-crash check
and exits without sweeping. A missing required argument or a zero case count /
timeout prints the usage and exits 2. `--verbose-kernel` forwards the real
kernel's `kprintf()` output to stderr with a `  kernel: ` prefix, which is
useful when a case panics and the report line does not say enough on its own;
without it the kernel log is swallowed so the sweep output stays readable.

## Results so far

With the gate's defaults -- 20000 ext4 cases, 2000 crypt-header cases, seed
1337 -- the sweep mounts about 10580 cases (53%), walks a path in 10535 and
reaches the write path in 2594 (13%), with zero crashes. The self-test passes
on the same binary. Four seeds and a longer sweep agree:

| Seed | ext4 cases | crypt cases | crash(es) |
| --- | --- | --- | --- |
| `1337` | 20000 | 2000 | 0 |
| `7` | 20000 | 2000 | 0 |
| `424242` | 20000 | 2000 | 0 |
| `999983` | 20000 | 2000 | 0 |

A 60000-case sweep run before the write path was added also found nothing. The
harness runs at roughly 1 ms per case on this host, so 22000 cases plus the two
image rebuilds fit in about 23 seconds.

The interesting part is not the zero. It is that half of the mutants still
mount: the ext4 and journal code rejects malformed metadata by returning an
error rather than trusting it, and the cases that reach depth 2 prove the read
path still works on a damaged image without reading out of bounds. The 2000
header cases all either validate or reject; none sends `crypt_meta_validate()`
or `crypt_meta_scratch_bytes()` off the end of the 256-byte structure.

Zero crashes in 22000 cases is evidence, not proof, and the next section says
exactly which faults the harness cannot reach.

## Limits and next steps

- **Ring 3 only.** The harness runs the kernel's C code as a host process, so it
  cannot reproduce a fault that needs ring 0: a page fault on a kernel virtual
  address, a `SMAP`/`SMEP` violation, an interrupt-context race or any
  privilege-dependent branch. It catches the algorithm, not the addresses it is
  given.
- **No QEMU, so no device or firmware path.** The image is served by an
  in-memory block device with explicit bounds checks. TCG, OVMF, virtio, NVMe,
  AHCI and the real interrupt path are outside the sweep; a driver bug that only
  appears when a real controller returns a short read is invisible here.
- **No dirty-journal replay.** Every case starts from a clean image, so the
  journal is replayed only when a mutation happens to flip the superblock's
  `needs_recovery` bit and the descriptors still parse. A dedicated leg that
  boots `mkdisk.sh` crash mode into the harness would test recovery from a real
  uncommitted transaction.
- **The crypt leg stops at the header parser.** It mutates the 256-byte
  `struct crug_crypt_header` and calls `crypt_meta_validate()` and
  `crypt_meta_scratch_bytes()`. It never unwraps a passphrase, so the Argon2id
  keyslot path and the AES-256-XTS data path are not covered; that is a different
  gate (`make test-crypt`) and a different class of code.
- **The GPT header itself is not mutated.** The harness needs a parseable table
  to find the partition, so only the partition payload is fuzzed. GPT parsing is
  covered by the partitioned-image gates, not by this one.
- **One thread, one writer.** No concurrent mount, no second writer, no SMP.
  The kernel is not SMP yet, but a race would not show up here even if it existed.
- **A fixed mutation model.** Six regions, eight kinds, 16-byte windows, uniform
  offsets, no structure-aware or length-field-aware edits, no coverage feedback
  and no corpus minimisation. It is a random sampler, not libFuzzer: it finds
  shallow bugs cheaply and will not build a long input.
- **No crash forensics beyond the image.** A finding gives the plan, the signal
  and a full image; it does not give registers or a stack trace, because the
  child is a host process and the interesting frames live in the kernel's own
  call graph.
- **A green sweep is a floor.** The gate proves the mount path survives 22000
  random mutants on this host with this seed set. It does not prove the absence
  of a bug that needs a specific 200-byte structure.

The obvious next steps are the ones this list implies: a dirty-journal leg, a
crypt keyslot leg that unwraps a test passphrase, structure-aware mutations over
extent trees and directory blocks, coverage feedback to keep interesting mutants
as a corpus, and -- once the kernel can host its own test harness -- the same
pipeline against a ring-0 build.

