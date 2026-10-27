# CrugOS

CrugOS is a small 64-bit operating system written in [Crug](https://github.com/),
a C dialect with Rust-style ownership and Go-style concurrency. It targets the
feel of AmigaOS: a compact monolithic kernel, message-passing userspace, a
Workbench shell and applications that are self-contained bundles.

The design contract lives in
[specs/CrugOS_complete_kernel_spec_v3.md](specs/CrugOS_complete_kernel_spec_v3.md).

## Status: phase 0 bootstrap (`v0.1.0`)

Phase 0 is the smallest end-to-end path from firmware to a running kernel:

```
OVMF -> BOOTX64.EFI (CrugOS UEFI loader) -> kernel.elf -> serial + framebuffer
```

The loader reads the kernel from its own boot device, loads the ELF64 segments at
their physical addresses, collects a GOP framebuffer, the ACPI RSDP, a
normalized memory map and entropy, then calls `ExitBootServices` and jumps to
`kernel_main(BootInfo *)`. The kernel prints a boot report to a 16550 serial
port and to the framebuffer, then exits QEMU deterministically for automated
testing.

Verified in this tree:

```
CRUGOS: hello
CRUGOS: version 0.1.0
kernel : 0x0000000000100000 size=44 KiB
memory : 37 regions, usable 474095 KiB (462 MiB)
fb     : 1024x768 stride=1024 format=BGRX8888 base=0x80000000
CRUGOS: phase 0 boot ok
```

## Quick start

```
make preflight   # crugc, GNU ld, QEMU, OVMF, python3
make             # build kernel.elf, BOOTX64.EFI and the ESP tree
make test        # boot under QEMU/OVMF and verify the boot contract
make run         # boot and keep the serial log for inspection
make test-host   # host unit tests (string, printf, ELF loader)
make version     # print the release version from VERSION
make size        # kernel budget: <1 MiB stretch, <2 MiB hard
make check-efi   # PE32+ structure checks on the loader
make repro       # two builds must be byte-identical
```

Build artifacts land in `build/x86_64/`; `make run` records
`build/x86_64/serial.log`, `debug.log` and `exit-code.txt`.

## Repository layout

| Path | Contents |
| --- | --- |
| `boot/efi/` | UEFI loader: file I/O, ELF loader, GOP, memory map, entropy, handoff |
| `include/crug/` | shared kernel/loader headers (types, BootInfo, I/O, string) |
| `kernel/arch/x86_64/` | boot code, linker script, port I/O, serial, debugcon, framebuffer |
| `kernel/core/` | kernel entry, console, printf, panic, boot report |
| `kernel/lib/` | freestanding string and formatting routines |
| `kernel/mm/` | memory management (phase 1) |
| `scripts/` | QEMU runner, boot/PE checks, size and reproducibility gates |
| `tests/host/` | host unit tests for freestanding code |
| `tools/` | host-side development tools (`gen-font.py`) |
| `docs/` | [build](docs/build.md), [boot](docs/boot.md), [versioning](docs/versioning.md), [roadmap](docs/roadmap.md) |

## Versioning

The release number lives in [`VERSION`](VERSION) and follows Semantic
Versioning. The build stamps it into the kernel and loader
(`-DCRUG_VERSION_STR`), the kernel prints `CRUGOS: version <version>`, and
`make test` fails if the guest version differs from `VERSION`. Releases are
annotated `vX.Y.Z` Git tags; `BOOTINFO_VERSION` is versioned separately. See
[docs/versioning.md](docs/versioning.md) for the release checklist and ABI
compatibility rules.

## Boot contract

`include/crug/bootinfo.h` defines `boot_info`: magic `0x43524731`, version 1,
the kernel extent, the normalized memory map, framebuffer mode, ACPI RSDP,
optional FDT/initrd, boot CPU id, a 64-byte entropy seed and the loader's TSC.
See [docs/boot.md](docs/boot.md) for the full handoff ABI and exit protocol.

## Toolchain notes

- `crugc` 0.2.3 (GCC 16.2.0) is a native x86_64 compiler. There is no
  `x86_64-crugos` cross target; the same host compiler builds the kernel with
  `-fcrug-kernel` (defines `__CRUG_KERNEL__`, disables `go`/`chan`/`select` and
  rejects ownership/borrow syntax).
- The UEFI loader must be compiled with `-fno-ident` and linked with
  `ld -m i386pep --disable-reloc-section`; every firmware entry point uses the
  Microsoft ABI (`ms_abi`). Details and failure modes are in
  [docs/build.md](docs/build.md).
- QEMU uses its `vvfat` driver as the ESP, so no FAT image or `mtools` is
  needed.

## Roadmap

Phase 0 is done. Phase 1 adds a 4 KiB bitmap allocator, W^X page tables, GDT/IDT
and timers; later phases cover IPC, EXT4 + RAID + crypto, USB/audio/graphics,
Workbench userspace, SMP/IOMMU and virtualisation. See
[docs/roadmap.md](docs/roadmap.md) for the full sequence and the known
specification gaps (Crug syntax and cross-architecture compilers).

## License

See [LICENSE](LICENSE).
