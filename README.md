# CrugOS

CrugOS is a small 64-bit operating system written in [Crug](https://github.com/),
a C dialect with Rust-style ownership and Go-style concurrency. It targets the
feel of AmigaOS: a compact monolithic kernel, message-passing userspace, a
Workbench shell and applications that are self-contained bundles.

The design contract lives in
[specs/CrugOS_complete_kernel_spec_v3.md](specs/CrugOS_complete_kernel_spec_v3.md).

## Status: phase 3B-1 writable ext4 (`v0.5.0`)

Phases 1-3B-1 keep the boot path under a kernel that owns memory, CPU state,
the execution model and a real writable filesystem:

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
LAPIC timer with a calibrated TSC. Phase 2 adds:

- a kernel heap: eight small-object slab classes from 16 to 2048 bytes plus
  contiguous large allocations up to 1 MiB, all interrupt-safe;
- kernel threads with 16 KiB stacks and a guard page, `cpu_switch` context
  switching, and preemptive round-robin scheduling in the `IDLE`, `NORMAL`,
  `INTERACTIVE` and `REALTIME_SYSTEM` classes;
- sleep queues, wait queues, thread join and a scheduler idle thread;
- per-process typed handle tables (256 entries, process-local ids, no kernel
  pointers) and a kernel process;
- `MsgPort` IPC with typed messages (1024-byte payload, depth 64) and O(1)
  send/receive with blocking waiters;
- a single syscall dispatcher behind an `int 0x80` gate, with a small phase 2
  call set (ping, gettid, yield, sleep, port create/send/recv, handle close,
  exit).

Phase 3A adds a working storage path from PCI to a real filesystem:

- a PCI configuration-space scanner and a legacy virtio-blk driver (`vda`),
  with a polled split virtqueue, a 500 ms completion timeout and virtio flush;
- a block-device registry with bounds-checked I/O, flush propagation and
  partition offset views;
- GPT parsing with protective-MBR, header and entry CRC32 validation;
- an ext4 driver (1/2/4 KiB blocks, extents, 32/64-byte group descriptors,
  linear directories) that rejects unsupported features instead of misreading
  them;
- a generated 16 MiB GPT/ext4 test disk (`make disk`) containing `/hello.txt`,
  read and verified during boot.

Phase 3B-1 makes that filesystem writable:

- create, mkdir, write, truncate, unlink, rmdir, rename and fsync, with extent
  allocation, inode/block bitmaps and group free counts kept consistent;
- a boot self-test that creates `/selftest`, writes a 5000-byte pattern,
  renames and truncates it to 1000 bytes, deletes a temporary file, unmounts
  cleanly and remounts read-only to verify the result;
- a host `e2fsck` gate (`scripts/check-disk.py`) that also extracts the image
  and checks the persisted file contents.

It detects AVX2, enables XSAVE state and, by default, refuses to boot without
AVX2. Self-tests run at boot, then the kernel prints a boot report to a 16550
serial port and the framebuffer and exits QEMU deterministically for automated
testing.

Verified in this tree:

```
CrugOS 0.5.0 (phase 3b) -- Amiga-inspired microkernel bring-up
CRUGOS: hello
CRUGOS: version 0.5.0
pmm    : 118433 pages (462 MiB), free 118432, bitmap 131072 B
CRUGOS: pmm ready
vm     : 4 GiB identity map, W^X kernel sections, guard=0x0000000000135000
CRUGOS: vm ready
CRUGOS: cpu ready
CRUGOS: timer ready
CRUGOS: heap ready
virtio : blk vda, 32768 sectors, io=0x6000
CRUGOS: block ready (vda, 32768 sectors)
gpt    : 1 partition(s), root 'crugroot' LBA 2048..26623
CRUGOS: gpt ready
CRUGOS: ext4 ready
CRUGOS: ext4 /hello.txt: hello from ext4
CRUGOS: storage ready
CRUGOS: sched ready
CRUGOS: syscall ready
CRUGOS: writable storage ok
CRUGOS: selftest ok
wx     : audit ok, identity map 0..4 GiB, guard unmapped
heap   : total=40960 B used=6800 B slab_pages=8 large_pages=2
boot   : pmm=26739 us cpu=383 us vm=589 us traps=329 us timer=65325 us heap=45 us storage=5613 us sched=291 us syscall=95 us total=99415 us
CRUGOS: phase 3b boot ok
```

Timing and memory values vary with the host and the QEMU CPU model.

## Quick start

```
make preflight   # crugc, GNU ld, QEMU, OVMF, python3
make             # build kernel.elf, BOOTX64.EFI and the ESP tree
make test        # boot under QEMU/OVMF and verify the boot contract
make run         # boot and keep the serial log for inspection
make test-compat # boot a non-AVX2 build on a non-AVX2 CPU model
make test-host   # host unit tests (string, printf, ELF, bitmap, VM math, handles)
make check-disk  # e2fsck + persisted-file checks on the latest test disk
make disk        # generate the GPT/ext4 test disk
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
| `include/crug/` | shared kernel/loader headers (types, BootInfo, I/O, string, heap, threads, IPC, syscalls) |
| `kernel/arch/x86_64/` | boot code, linker script, port I/O, serial, framebuffer, CPU/IDT/APIC/timer, context switch |
| `kernel/core/` | kernel entry, console, printf, panic, self-tests, scheduler, handles, MsgPort, syscall dispatcher, boot report |
| `kernel/lib/` | freestanding string, formatting and bitmap routines |
| `kernel/mm/` | physical allocator, W^X page tables, kernel heap |
| `kernel/block/` | block-device registry, bounds checks and partitions |
| `kernel/fs/` | GPT, writable ext4 and the storage boot policy |
| `drivers/` | PCI and block device drivers (legacy virtio-blk) |
| `scripts/` | QEMU runner, disk-image builder, boot/PE/disk checks, size and reproducibility gates |
| `tests/host/` | host unit tests for freestanding code |
| `tools/` | host-side development tools (`gen-font.py`) |
| `docs/` | [build](docs/build.md), [boot](docs/boot.md), [storage](docs/storage.md), [cpu/AVX2](docs/cpu.md), [versioning](docs/versioning.md), [roadmap](docs/roadmap.md) |

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

Phases 0, 1, 2, 3A and 3B-1 are done. Phase 3A covered the block layer, legacy
virtio-blk, GPT and read-only EXT4; phase 3B-1 added the writable EXT4 path and
an on-disk consistency gate. Remaining phase 3B work is JBD2 journal replay,
`crypt.device` (AES-256-XTS/Argon2id), RAID1/10 with a redundant ESP, and
AHCI/NVMe. Later phases cover USB/audio/graphics, Workbench userspace, SMP/IOMMU
and virtualisation. See
[docs/roadmap.md](docs/roadmap.md) for the full sequence and the known
specification gaps (Crug syntax, cross-architecture compilers and the OVMF
minimum RAM floor).

## License

See [LICENSE](LICENSE).
