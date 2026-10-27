# CrugOS

CrugOS is a small 64-bit operating system written in [Crug](https://github.com/),
a C dialect with Rust-style ownership and Go-style concurrency. It targets the
feel of AmigaOS: a compact monolithic kernel, message-passing userspace, a
Workbench shell and applications that are self-contained bundles.

The design contract lives in
[specs/CrugOS_complete_kernel_spec_v3.md](specs/CrugOS_complete_kernel_spec_v3.md).

## Status: phase 3B-3 RAID1 (`v0.7.0`)

Phases 1-3B-3 keep the boot path under a kernel that owns memory, CPU state,
the execution model, a real crash-consistent filesystem and a mirrored root:

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

Phase 3B-2 adds JBD2 journaling:

- the JBD2 V2 journal in inode 8, with descriptor/commit blocks and a 1 MiB
  journal in the generated test disk;
- ordered metadata journaling: file data is flushed before the commit, then
  inodes, bitmaps, group descriptors, directories and extent leaves are
  journaled, checkpointed and the journal is cleared;
- replay at mount: a dirty journal is validated and applied before the
  filesystem is used; a read-only mount of a dirty journal is refused;
- a boot self-test that leaves a dirty journal, unmounts, remounts and proves
  replay restored the block, while `e2fsck` still passes on the host.

Phase 3B-3 mirrors the root filesystem with RAID1:

- up to eight legacy virtio-blk disks (`vda`..`vdh`) are discovered by
  scanning every PCI function, each with its own virtqueue and flush support;
- each member carries a checksummed 512-byte metadata record in its last two
  sectors (magic `CRA1`, level, slot, member count, geometry, generation,
  array/member UUIDs);
- `raid_assemble()` validates the members and registers `md0`; reads take the
  first healthy member, writes and flushes go to every healthy member, and a
  broken member is dropped for the rest of the boot;
- `storage_init()` still prefers a direct `crugroot` partition, otherwise it
  assembles `fd00` members and boots from the array, including a degraded
  boot with one member missing;
- host tools `mkraid.sh`, `raid-meta.py` and `check-raid.py`; `make
  test-raid` gates a healthy array and `make test-raid-degraded` gates a
  degraded one, with metadata, `e2fsck` and data-region checks.

It detects AVX2, enables XSAVE state and, by default, refuses to boot without
AVX2. Self-tests run at boot, then the kernel prints a boot report to a 16550
serial port and the framebuffer and exits QEMU deterministically for automated
testing.

Verified in this tree:

```
CrugOS 0.7.0 (phase 3b3) -- Amiga-inspired microkernel bring-up
CRUGOS: hello
CRUGOS: version 0.7.0
pmm    : 118402 pages (462 MiB), free 118401, bitmap 131072 B
CRUGOS: pmm ready
cpu    : AuthenticAMD family 15 model 107 stepping 1 nx=yes apic=yes avx2=yes required=yes
vm     : 4 GiB identity map, W^X kernel sections, guard=0x000000000013c000
CRUGOS: vm ready
CRUGOS: cpu ready
timer  : 2688128803 Hz (pit), mode=lapic-periodic, 100 Hz tick
CRUGOS: timer ready
CRUGOS: heap ready
virtio : blk vda, 32768 sectors, io=0x6080
virtio : blk vdb, 32768 sectors, io=0x6000
CRUGOS: block ready (2 device(s))
CRUGOS: gpt ready
CRUGOS: raid ready
CRUGOS: raid healthy (2/2 members)
CRUGOS: ext4 ready
CRUGOS: ext4 /hello.txt: hello from ext4
CRUGOS: journal ready
CRUGOS: storage ready
CRUGOS: sched ready
CRUGOS: syscall ready
CRUGOS: journal selftest ok (1 blocks recovered)
CRUGOS: writable storage ok
CRUGOS: selftest ok
wx     : audit ok, identity map 0..4 GiB, guard unmapped
heap   : total=61440 B used=10400 B slab_pages=13 large_pages=2
boot   : pmm=34311 us cpu=482 us vm=730 us traps=391 us timer=65363 us heap=52 us storage=9673 us sched=324 us syscall=118 us total=111447 us
CRUGOS: phase 3b3 boot ok
```

Timing and memory values vary with the host and the QEMU CPU model.

## Quick start

```
make preflight   # crugc, GNU ld, QEMU, OVMF, python3
make             # build kernel.elf, BOOTX64.EFI and the ESP tree
make test        # boot under QEMU/OVMF and verify the boot contract
make test-raid   # boot a healthy two-member RAID1 root and check both disks
make test-raid-degraded  # boot RAID1 with one member missing
make run         # boot and keep the serial log for inspection
make test-compat # boot a non-AVX2 build on a non-AVX2 CPU model
make test-host   # host unit tests (string, printf, ELF, bitmap, VM math, handles)
make check-disk  # e2fsck + persisted-file checks on the latest test disk
make disk        # generate the GPT/ext4 test disk
make raid-disks  # generate the two RAID1 member disks
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
| `kernel/block/` | block-device registry, partitions, bounds checks and RAID1 |
| `kernel/fs/` | GPT, writable ext4, JBD2 journal and the storage boot policy |
| `drivers/` | PCI and block device drivers (legacy virtio-blk) |
| `scripts/` | QEMU runner, disk/RAID image builders, boot/PE/disk/RAID checks, size and reproducibility gates |
| `tests/host/` | host unit tests for freestanding code |
| `tools/` | host-side development tools (`gen-font.py`) |
| `docs/` | [build](docs/build.md), [boot](docs/boot.md), [storage](docs/storage.md), [RAID1](docs/raid.md), [cpu/AVX2](docs/cpu.md), [versioning](docs/versioning.md), [roadmap](docs/roadmap.md) |

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

Phases 0, 1, 2, 3A, 3B-1, 3B-2 and 3B-3 are done. Phase 3A covered the block
layer, legacy virtio-blk, GPT and read-only EXT4; phase 3B-1 added the writable
EXT4 path and an on-disk consistency gate; phase 3B-2 added JBD2 journal replay
and ordered metadata journaling; phase 3B-3 added a RAID1 root with healthy and
degraded boots. Remaining phase 3B work is journal/metadata checksums,
`crypt.device` (AES-256-XTS/Argon2id), RAID10/rebuild with a redundant ESP,
and AHCI/NVMe. Later phases cover USB/audio/graphics, Workbench userspace,
SMP/IOMMU and virtualisation. See
[docs/roadmap.md](docs/roadmap.md) for the full sequence and the known
specification gaps (Crug syntax, cross-architecture compilers and the OVMF
minimum RAM floor).

## License

See [LICENSE](LICENSE).
