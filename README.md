# CrugOS

CrugOS is a small 64-bit operating system written in [Crug](https://github.com/),
a C dialect with Rust-style ownership and Go-style concurrency. It targets the
feel of AmigaOS: a compact monolithic kernel, message-passing userspace, a
Workbench shell and applications that are self-contained bundles.

The design contract lives in
[specs/CrugOS_complete_kernel_spec_v3.md](specs/CrugOS_complete_kernel_spec_v3.md).

## Status: phase 1 memory and CPU (`v0.2.0`)

Phase 1 keeps the phase 0 path from firmware to a running kernel and makes the
kernel own the machine underneath it:

```
OVMF -> BOOTX64.EFI (CrugOS UEFI loader) -> kernel.elf -> serial + framebuffer
```

The loader reads the kernel from its own boot device, loads the ELF64 segments at
their physical addresses, collects a GOP framebuffer, the ACPI RSDP, a
normalized memory map and entropy, marks every page the kernel must not use and
calls `ExitBootServices`. It then jumps to `kernel_main(BootInfo *)`.

The kernel owns its physical memory through a 4 KiB bitmap allocator, builds
its own identity page tables with W^X permissions and an unmapped stack guard,
installs a GDT/TSS and a 256-vector IDT with exception handlers, and runs a
LAPIC timer with a calibrated TSC. It detects AVX2, enables XSAVE state and, by
default, refuses to boot without AVX2. Self-tests run at boot, then the kernel
prints a boot report to a 16550 serial port and the framebuffer and exits QEMU
deterministically for automated testing.

Verified in this tree:

```
CRUGOS: hello
CRUGOS: version 0.2.0
pmm    : 118479 pages (462 MiB), free 118478, bitmap 131072 B
CRUGOS: pmm ready
cpu    : AuthenticAMD ... nx=yes apic=yes avx2=yes required=yes
vm     : 4 GiB identity map, W^X kernel sections, guard=0x0000000000128000
CRUGOS: vm ready
CRUGOS: cpu ready
CRUGOS: timer ready
CRUGOS: selftest ok
wx     : audit ok, identity map 0..4 GiB, guard unmapped
boot   : pmm=27314 us cpu=365 us vm=1101 us traps=302 us timer=65282 us total=94367 us
timer  : 2687826024 Hz (pit), mode=lapic-periodic, 100 Hz tick
CRUGOS: phase 1 boot ok
```

Timing and memory values vary with the host and the QEMU CPU model.

## Quick start

```
make preflight   # crugc, GNU ld, QEMU, OVMF, python3
make             # build kernel.elf, BOOTX64.EFI and the ESP tree
make test        # boot under QEMU/OVMF and verify the boot contract
make run         # boot and keep the serial log for inspection
make test-compat # boot a non-AVX2 build on a non-AVX2 CPU model
make test-host   # host unit tests (string, printf, ELF, bitmap, VM math)
make version     # print the release version from VERSION
make size        # kernel budget: <1 MiB stretch, <2 MiB hard
make check-efi   # PE32+ structure checks on the loader
make repro       # two builds must be byte-identical
```

Build artifacts land in `build/x86_64/`; `make run` records
`build/x86_64/serial.log`, `debug.log` and `exit-code.txt`. Useful variables
include `MEMORY_MB`, `QEMU_CPU`, `REQUIRE_AVX2` and `MODE`; see
[docs/build.md](docs/build.md).

## Repository layout

| Path | Contents |
| --- | --- |
| `boot/efi/` | UEFI loader: file I/O, ELF loader, GOP, memory map, entropy, handoff |
| `include/crug/` | shared kernel/loader headers (types, BootInfo, I/O, string) |
| `kernel/arch/x86_64/` | boot code, linker script, port I/O, serial, framebuffer, CPU/IDT/APIC/timer |
| `kernel/core/` | kernel entry, console, printf, panic, self-tests, boot report |
| `kernel/lib/` | freestanding string, formatting and bitmap routines |
| `kernel/mm/` | physical allocator and W^X page tables |
| `scripts/` | QEMU runner, boot/PE checks, size and reproducibility gates |
| `tests/host/` | host unit tests for freestanding code |
| `tools/` | host-side development tools (`gen-font.py`) |
| `docs/` | [build](docs/build.md), [boot](docs/boot.md), [cpu/AVX2](docs/cpu.md), [versioning](docs/versioning.md), [roadmap](docs/roadmap.md) |

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
- AVX2 is detected and its XSAVE state enabled. The default build panics if the
  CPU lacks AVX2; `REQUIRE_AVX2=0` builds a compatibility kernel. Do not use
  `-cpu max` or `-cpu Haswell` with this OVMF; see [docs/cpu.md](docs/cpu.md).

## Roadmap

Phases 0 and 1 are done. Phase 2 adds threads and context switching, scheduling
classes, handles and a single syscall dispatcher, a kernel heap and `MsgPort`
IPC; later phases cover EXT4 + RAID + crypto, USB/audio/graphics, Workbench
userspace, SMP/IOMMU and virtualisation. See [docs/roadmap.md](docs/roadmap.md)
for the full sequence and the known specification gaps (Crug syntax,
cross-architecture compilers and the OVMF minimum RAM floor).

## License

See [LICENSE](LICENSE).