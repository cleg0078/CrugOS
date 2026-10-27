# Building CrugOS

This document describes the phase 3B-2 build system: how the kernel, the UEFI
loader and the test disk image are built, which invariants the link must
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
| `sgdisk`, `mke2fs`, `debugfs`, `e2fsck`, `dd`, `truncate` | test disk | only for `make disk` / `make check-disk`; e2fsprogs |

Run `make preflight` to check all of the above.

## Targets

```
make             # kernel.elf, BOOTX64.EFI and the ESP tree
make run         # boot the ESP tree under QEMU + OVMF
make test        # boot, verify markers/version/AVX2, then e2fsck the disk
make test-compat # boot a REQUIRE_AVX2=0 build on a non-AVX2 CPU model
make test-host   # host unit tests for string/printf/ELF/bitmap/VM math/handles
make disk        # build the GPT/ext4 test disk at build/<arch>/disk.img
make check-disk  # e2fsck and persisted-file checks on the current disk
make size        # kernel size budget (<1 MiB stretch, <2 MiB hard)
make check-efi   # PE32+ structure checks on the loader
make repro       # two independent builds must be byte-identical
make preflight   # toolchain/firmware availability
make version     # print the release version from VERSION
make clean       # remove build/<arch>
make distclean   # remove build/ and host test artifacts
```

Useful variables:

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
| `EXPECT_AVX2` | `yes` | what `make test` requires the guest to report |
| `BUILD_ROOT` | `build` | output root; `test-compat` uses `build/compat` |
| `DISK_IMG` | `build/<arch>/disk.img` | raw disk image attached as `vda` |
| `DISK_SIZE_MB` | `16` | size of the generated test disk |

## Version stamping

`VERSION` is the source of truth. The makefile reads it once and passes
`-DCRUG_VERSION_STR="<version>"` to every kernel and loader object, so both
report the same number. Direct compiler invocations fall back to `0.0.0-dev`
via `include/crug/version.h`. The ESP tree also gets `crugos/version` as a
plain text file. `make test` compares the kernel's `CRUGOS: version` line with
`VERSION`; a mismatch fails the boot gate. Release procedure and tag rules are
in [versioning.md](versioning.md).

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
230 KiB. The specification's 16 MiB headless target needs a leaner boot path and
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

`make run` and `make test` depend on `disk`, so the first boot builds the image.
The kernel mounts it read-only during `storage_init`, then the boot self-test
remounts it read-write, creates and mutates files under `/selftest`, unmounts
cleanly and remounts read-only to verify the persisted data. It also queues a
dirty journal block, unmounts without clearing the journal, remounts
read-write and verifies replay. `make test` regenerates the image, boots the
kernel and then runs [scripts/check-disk.py](../scripts/check-disk.py), which
extracts the partition and requires `e2fsck -fn` to be clean (a leftover dirty
journal fails this gate). `make check-disk` runs only that disk check on the
existing image. The supported ext4 subset, write path, journal format and
failure modes are documented in [storage.md](storage.md).

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