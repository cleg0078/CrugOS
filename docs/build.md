# Building CrugOS

This document describes the phase 3B-5 build system: how the kernel, the UEFI
loader and the test disk images are built, which invariants the link must
preserve, and how to diagnose the failure modes that are specific to this
toolchain.

## Requirements

| Tool | Purpose | Notes |
| --- | --- | --- |
| `crugc` | Crug compiler | 0.2.3 / GCC 16.2.0, host `x86_64-pc-linux-gnu`; `-fcrug-kernel` |
| `ld` (GNU binutils) | loader link | must support `-m i386pep` |
| `qemu-system-x86_64` | boot test | 10.x, TCG works without `/dev/kvm` |
| OVMF | UEFI firmware | `/usr/share/OVMF/OVMF_CODE_4M.fd` + `OVMF_VARS_4M.fd` |
| `python3` | boot/PE checks | standard library only |
| `sgdisk`, `mke2fs`, `debugfs`, `e2fsck`, `dd`, `truncate` | test disks | only for `make disk`, `make raid-disks`, `make raid10-disks` and their checks; e2fsprogs |

Run `make preflight` to check all of the above.

## Targets

```
make             # show this target list (same as `make help`)
make all         # kernel.elf, BOOTX64.EFI and the ESP tree
make run         # boot the ESP tree under QEMU + OVMF
make test        # boot, verify markers/version/AVX2, then e2fsck the disk
make test-raid   # boot a two-member RAID1 root and check both members
make test-raid10 # boot a four-member RAID10 root and check every member
make test-nvme   # boot from an NVMe namespace and check the image
make test-raid-degraded  # boot RAID1 with one member missing
make test-raid10-degraded  # boot RAID10 with one member per mirror pair
make test-compat # boot a REQUIRE_AVX2=0 build on a non-AVX2 CPU model
make test-host   # host unit tests for string/printf/ELF/bitmap/VM math/handles
make disk        # build the GPT/ext4 test disk at build/<arch>/disk.img
make raid-disks  # build the two GPT/ext4 RAID1 member disks
make raid10-disks  # build the four GPT/ext4 RAID10 member disks
make nvme-disk   # build the GPT/ext4 image used by test-nvme
make check-disk  # e2fsck and persisted-file checks on the current disk
make size        # kernel size budget (<1 MiB stretch, <2 MiB hard)
make check-efi   # PE32+ structure checks on the loader
make repro       # two independent builds must be byte-identical
make preflight   # toolchain/firmware availability
make version     # print the release version from VERSION
make options     # print every option with its current value
make clean       # remove build/<arch>
make distclean   # remove build/ and host test artifacts
```

Useful variables. Every row can be overridden on the command line; `make
options` prints the complete resolved set (including paths derived from
`BUILD_ROOT`).

| Variable | Default | Meaning |
| --- | --- | --- |
| `ARCH` | `x86_64` | only architecture implemented |
| `MODE` | `release` | `release`, `debug` or `size` |
| `V` | `0` | `V=1` prints full command lines |
| `MEMORY_MB` | `512` | guest RAM; see the firmware floor below |
| `SMP` | `1` | vCPUs (the phase 2 kernel uses the boot CPU only) |
| `TIMEOUT_SEC` | `30` | QEMU wall-clock limit for `make run`/`make test` |
| `QEMU_CPU` | `qemu64,+avx,+avx2` | `-cpu` model; empty means QEMU's default |
| `REQUIRE_AVX2` | `1` | `0` builds a kernel that boots without AVX2 |
| `EXPECT_AVX2` | `yes` | what `make test` requires the guest to report (`no` for compat) |
| `EXPECT_RAID` | `no` | what `make test` requires for RAID: `no`, `healthy` or `degraded` |
| `EXPECT_NVME` | `no` | what `make test` requires for NVMe (`yes` needs `nvme0n1`) |
| `CC` | `crugc` | compiler for kernel and loader objects |
| `LD` | `ld` | linker for the PE32+ loader |
| `PYTHON` | `python3` | host scripts (boot/disk/EFI checks) |
| `QEMU` | `qemu-system-x86_64` | emulator binary |
| `OVMF_CODE` | `/usr/share/OVMF/OVMF_CODE_4M.fd` | OVMF firmware code image |
| `OVMF_VARS` | `/usr/share/OVMF/OVMF_VARS_4M.fd` | OVMF variable store (copied per run) |
| `QEMU_ACCEL` | empty | `-accel` backend, e.g. `tcg` or `kvm`; empty lets QEMU choose |
| `QEMU_DISPLAY` | `none` | `-display` value; `none` keeps runs headless |
| `QEMU_EXTRA` | empty | extra whitespace-separated QEMU arguments |
| `BUILD_ROOT` | `build` | output root; `test-compat` uses `build/compat` |
| `DISK_IMG` | `build/<arch>/disk.img` | raw disk image attached as `vda`; empty boots without virtio |
| `DISK_SIZE_MB` | `16` | size of the generated test disk |
| `RAID_DISK_A/B` | `build/<arch>/raid-a.img`, `raid-b.img` | RAID1 members attached as `vda`/`vdb` |
| `RAID10_DISK_A..D` | `build/<arch>/raid10-a.img`..`raid10-d.img` | RAID10 members attached as `vda`..`vdd` |
| `NVME_IMG` | `build/<arch>/nvme.img` | image attached as an NVMe namespace |
| `NVME_SERIAL` | `CRUGNVME0` | QEMU NVMe serial reported by OVMF |
| `EXTRA_DISKS` | empty | space-separated images attached after `DISK_IMG` |

## Version stamping

`VERSION` is the source of truth. The makefile reads it once and passes
`-DCRUG_VERSION_STR="<version>"` to every kernel and loader object, so both
report the same number. Direct compiler invocations fall back to `0.0.0-dev`
via `include/crug/version.h`. The ESP tree also gets `crugos/version` as a
plain text file. `make test` compares the kernel's `CRUGOS: version` line with
`VERSION`; a mismatch fails the boot gate. Release procedure and tag rules are
in [versioning.md](versioning.md). Because the version is a compile-time
define, every kernel and loader object also depends on `VERSION`; changing it
forces a full recompile rather than only a relink.

## Kernel link

The kernel is a freestanding ELF64 executable linked at a fixed address:

```
KERNEL_BASE = 0x00100000
```

`kernel/arch/x86_64/linker.ld` lays out `.text`, `.rodata`, `.data` and `.bss` on
4 KiB boundaries and exports:

```
__text_start __text_end   __bss_start __bss_end __kernel_end
```

Link flags that matter:

```
-nostdlib -static -no-pie -Wl,--build-id=none -Wl,-T,kernel/arch/x86_64/linker.ld
```

`--build-id=none` keeps the image reproducible and removes a non-loadable note.
The script discards `.comment`, `.note*`, `.eh_frame`, `.rela*` and dynamic
sections; the kernel must not need runtime relocations. C sources are compiled
with `-fno-pic -fno-pie -fno-builtin -fno-stack-protector -mno-red-zone`, no
SSE/FPU (`-mno-sse -mno-sse2 -mno-mmx -mno-80387`) and no AVX
(`-mno-avx -mno-avx2 -mno-avx512f`). The compiler therefore never emits vector
state save/restore in interrupt-adjacent code before the context-switch path
owns XSAVE state. AVX2 detection and `XCR0` setup are described in
[cpu.md](cpu.md).

## Loader link

The UEFI loader is a PE32+ EFI application, not an ELF. It is linked with GNU
`ld` directly:

```
ld -m i386pep --subsystem 10 -e efi_main \
   --image-base 0x10000000 --disable-reloc-section --no-insert-timestamp
```

Hard-won constraints (each of these was a real OVMF failure):

1. `efi_main` and every firmware function pointer use the Microsoft ABI
   (`__attribute__((ms_abi))`, `EFIAPI`). Mixing SysV and MS ABI raises `#UD`
   on the first indirect firmware call.
2. Compile loader objects with `-fno-ident`. Otherwise `.comment` lands at RVA 0
   and OVMF refuses the image (`Load Error`).
3. Do not use `-ffunction-sections` + `--gc-sections`; the entry point can be
   garbage collected and firmware reports no output.
4. Do not use a custom PE linker script or `--enable-reloc-section`. ELF-derived
   `.reloc` contents are invalid here and OVMF crashes loading the image.
5. No `mtools` is required: QEMU's `vvfat` driver exposes the ESP directory
   directly. The process `TMPDIR` must be writable.

## Firmware memory floor

OVMF 4M needs a substantial amount of RAM before it will boot from QEMU's
`vvfat` ESP. Measured with this QEMU/OVMF pair:

| `MEMORY_MB` | Result |
| --- | --- |
| 16, 32 | QEMU exits 0; no serial output |
| 64, 128 | OVMF reports `No bootable option or device was found` |
| 256 | booted once, then failed the same way on a re-run |
| 512 | reliable; used by all gates |

This is a firmware limit, not a kernel limit: the kernel load image is about
290 KiB. The specification's 16 MiB headless target needs a leaner boot path and
is tracked in [roadmap.md](roadmap.md).

## Test disk

`make disk` runs [scripts/mkdisk.sh](../scripts/mkdisk.sh) and produces a GPT
image with one type-`8300` partition named `crugroot` and an ext4 filesystem
containing `/hello.txt`. The filesystem has a 1 MiB JBD2 journal
(`-J size=1`) and disables `orphan_file`; the kernel replays a dirty journal
before using the filesystem. It requires `sgdisk`, `mke2fs`, `debugfs`,
`dd` and `truncate`. The image is attached as a legacy virtio-blk device:

```
-drive file=<disk.img>,if=none,id=disk0,format=raw
-device virtio-blk-pci,drive=disk0,disable-modern=on
```

`make run` builds the image if it is missing; `make test` always regenerates it
first. The kernel mounts it read-only during `storage_init`, then the boot self-test
remounts it read-write, creates and mutates files under `/selftest`, unmounts
cleanly and remounts read-only to verify the persisted data. It also queues a
dirty journal block, unmounts without clearing the journal, remounts
read-write and verifies replay. `make test` regenerates the image, boots the
kernel and then runs [scripts/check-disk.py](../scripts/check-disk.py), which
extracts the partition and requires `e2fsck -fn` to be clean (a leftover dirty
journal fails this gate). `make check-disk` runs only that disk check on the
existing image. The supported ext4 subset, write path, journal format and
failure modes are documented in [storage.md](storage.md).

`make test-nvme` regenerates `build/<arch>/nvme.img` with `mkdisk.sh`, boots
with only `NVME_IMG` attached to QEMU's `nvme` device, requires
`CRUGOS: nvme ready` plus `nvme0n1`, and then runs `check-disk.py` on the image.
The driver maps the 64-bit BAR above 4 GiB, disables any controller state left
by OVMF, creates queues and identifies namespace 1; see [nvme.md](nvme.md).

`make raid-disks` runs [scripts/mkraid.sh](../scripts/mkraid.sh) and
[scripts/raid-meta.py](../scripts/raid-meta.py) to build two 16 MiB GPT disks
with one type-`fd00` `crugraid` partition each, identical ext4 data and
checksummed RAID1 metadata in the last two sectors. `make test-raid` boots
with both images and requires a healthy array; `make test-raid-degraded` boots
with only `RAID_DISK_A` and requires a degraded boot.
[scripts/check-raid.py](../scripts/check-raid.py) validates both metadata
copies and geometry, extracts the data region, runs `e2fsck -fn`, checks the
persisted files, and after a healthy run compares the two data regions byte for
byte. The metadata format, assembly rules and limits are in
[raid.md](raid.md).

`make raid10-disks` runs [scripts/mkraid10.sh](../scripts/mkraid10.sh) with
the same `raid-meta.py` helper to build four 16 MiB members with slots 0..3,
level 10 and a 128-sector chunk. `make test-raid10` attaches all four images
and requires `raid healthy (4/4 members)`; `make test-raid10-degraded` attaches
only `RAID10_DISK_A` and `RAID10_DISK_C` (slots 0 and 2, one member per mirror
pair) and requires `raid degraded (2/4 members)`.
[scripts/check-raid10.py](../scripts/check-raid10.py) validates the metadata,
unique slots and pair coverage, rebuilds the logical array from one member per
pair, runs `e2fsck -fn` and checks the persisted files on every member.

## Reproducibility

`make repro` builds `kernel.elf` and `BOOTX64.EFI` twice under different
`BUILD_ROOT`s and compares them byte-for-byte. Any timestamp, path or hash
randomisation that leaks into the output will fail this target.

## Troubleshooting

- **`Load Error` from OVMF**: inspect `.comment`/first section alignment and
  confirm `-fno-ident` and `--disable-reloc-section`.
- **`!!!! Find image based on IP(...)`**: invalid or stripped relocation data;
  rebuild the loader with the exact link line above.
- **No serial output at all**: OVMF may have printed only to the framebuffer;
  check `build/<arch>/debug.log` (port `0xE9`) and re-run with `-display none`
  omitted.
- **`ExitBootServices failed`**: the memory map changed between snapshot and
  call; the loader retries three times, re-reading the map each time.
- **QEMU resets in a loop / no output after the handoff**: a page-table bug.
  Re-run with `-d int,cpu_reset -D qemu-int.log` and look for the first
  exception (`v=0e` with `e=0011` is an NX fault on instruction fetch, a common
  symptom of setting `NX` on a page-directory entry that points at a page
  table).
- **`#PF` in the kernel with a present+instruction-fetch error**: check that
  `.text` is mapped executable and that every page-table-referencing PDE has
  `NX` clear.
- **No boot at low `MEMORY_MB`**: see the firmware memory floor above; use
  `MEMORY_MB=512`.
- **`virtio : queue setup failed`**: the disk was attached as a modern-only
  device or without `disable-modern=on`; use the QEMU flags above.
- **NVMe timeout or `controller not ready`**: check CSTS at `0x1C` and AQA at
  `0x24`; a controller left enabled by OVMF must be disabled and RDY polled
  before the queues are programmed. Create SQ status `0x8201` means the
  Create CQ/SQ dwords are reversed (queue id low 16, size high 16). See
  [nvme.md](nvme.md).
- **`raid   : metadata invalid`**: a member's last two sectors are missing or
  disagree on magic, CRC, level, chunk, geometry, UUID, generation or slot.
  Regenerate with `make raid-disks` or `make raid10-disks`; inspect with
  `scripts/check-raid.py` or `scripts/check-raid10.py`.
- **`raid   : mirror pair N has no member`**: a RAID10 array lost both members
  of one pair. There is no rebuild; regenerate the images with
  `make raid10-disks`.
- **Healthy test boots degraded**: a virtio device did not appear or its
  metadata was rejected. Check the serial `block ready (N device(s))` line and
  QEMU's extra `-drive` pairs; the test scripts pass `EXTRA_DISKS`. A RAID10
  degraded gate is expected to show `2/4`.
- **`make test-raid` or `make test-raid10` fails e2fsck but the array
  booted**: members diverged or chunks were assembled from the wrong pair.
  `check-raid.py`/`check-raid10.py` report the mismatch; this indicates a
  mirror/stripe write-path bug, not a filesystem bug.
- **`storage: writable test failed`**: the in-kernel write path failed. The
  serial log names the step; `make check-disk` shows whether the on-disk state
  or the file contents are wrong. A bitmap-offset bug shows up as
  `Multiply-claimed block(s)` in `e2fsck`.
- **`e2fsck` reports bitmap differences or wrong link counts**: block bitmap bits
  are relative to the group's first block (`s_first_data_block + group *
  blocks_per_group`), not absolute block numbers. Check the allocation code in
  `kernel/fs/ext4.c`.
- **`storage: ext4 mount failed`**: the image has an unsupported feature or a
  dirty journal on a read-only mount. Extract the partition with `dd` and
  inspect it with `dumpe2fs -h`; `make disk` regenerates a known-good image.
- **`storage: journal selftest failed`**: replay did not restore the queued
  block. Check the descriptor/commit sequence and `s_start` handling in
  `kernel/fs/journal.c`; a read-only mount of a dirty journal is refused by
  design.
- **`e2fsck` reports a dirty journal or needs recovery**: the kernel did not
  checkpoint/clear `s_start` before unmount. The journal superblock is
  big-endian; inspect bytes 24..31 at the journal inode's first block.
- **QEMU exits before OVMF starts**: verify the firmware paths and that
  `OVMF_VARS` is writable (the run script copies it into `build/`).
- **Guest still reports the old version after a bump**: ensure `VERSION` is a
  prerequisite of the object files, not just of the link. The makefile has
  `$(KERNEL_OBJECTS): VERSION` and `$(LOADER_OBJECTS): VERSION` for this
  reason; a stale build tree otherwise keeps the old string.
