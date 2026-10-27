# Roadmap

The specification defines 38 phases/gates. Phase 0 established the smallest
end-to-end path (firmware -> loader -> kernel -> report -> deterministic exit);
phase 1 makes the kernel own physical memory, page tables, exceptions and
timers; phase 2 adds the execution model (heap, threads, scheduling, handles,
IPC and syscalls); phase 3A adds a working block/GPT/ext4 read-only path to a
real disk; phase 3B-1 makes that filesystem writable and gates it on
`e2fsck`. Every later phase keeps the same BootInfo contract, so the loader
does not change for memory management, storage, SMP or drivers.

## Phase 0 - bootstrap (done)

- UEFI loader: file I/O, ELF64 loading, GOP, RSDP, memory map, entropy,
  ExitBootServices, handoff.
- Kernel: freestanding Crug, serial/debugcon/framebuffer consoles, `ksnprintf`,
  panic with stack scan, boot report, size budget, host unit tests, QEMU boot test.

## Phase 1 - memory and CPU (done)

- Loader invariant: post-snapshot allocations (kernel, framebuffer, BootInfo and
  the normalized map itself) are marked `BOOTLOADER`/`KERNEL`/`FRAMEBUFFER`, and
  boot stops if any `USABLE` range overlaps them.
- Bitmap physical allocator over `BootInfo.memory_map`: 4 KiB frames, one bit
  per frame, 1 = used. Never allocates the kernel, bitmap, framebuffer, ACPI or
  boot metadata; allocation zeroes the frame and supports contiguous/aligned
  ranges.
- Kernel page tables: identity map of 0..4 GiB with 2 MiB huge pages, 4 KiB
  granularity for the kernel image and the boot stack. `W^X`: `.text` is
  read/execute, `.rodata` is read-only NX, `.data`/`.bss` are writable NX, and a
  4 KiB guard page sits below the boot stack.
- GDT/TSS, per-CPU data via `%gs`, IDT with 256 vectors, exception dispatch
  (`#BP` returns; other faults panic), double-fault IST stack and `int3` selftest.
- LAPIC + timer: PIC masked, TSC frequency from CPUID 0x15/0x16 or a PIT
  calibration, TSC-deadline when available, otherwise LAPIC periodic at 100 Hz;
  monotonic nanoseconds and calibrated boot-stage timings.
- AVX2 detection and XSAVE enablement (`CR4.OSXSAVE`, `XCR0=0x7`); the default
  build requires AVX2, `REQUIRE_AVX2=0` builds a compatibility kernel.

## Phase 2 - execution model (done)

- Kernel heap with eight slab classes (16..2048 bytes) and contiguous large
  allocations up to 1 MiB; interrupt-safe allocation and double-free detection.
- Threads with a 16 KiB stack and guard page, `cpu_switch` context switching,
  per-class run queues and the six thread states.
- Scheduling classes `IDLE`, `NORMAL`, `INTERACTIVE`, `REALTIME_SYSTEM`
  with round-robin quanta, timer preemption, sleep/wakeup queues, join and an
  idle thread.
- Per-process typed handle tables (256 entries, process-local ids, no kernel
  pointers) and a kernel process.
- `MsgPort` IPC with typed messages (1024-byte payload, depth 64), O(1)
  send/receive and blocking senders/receivers.
- A single syscall dispatcher behind an `int 0x80` DPL3 gate, with ping,
  gettid, yield, sleep, port create/send/recv, handle close and exit.
- Boot self-tests for the heap, handles, scheduler, IPC and syscalls.

## Phase 3A - storage vertical slice (done)

- Block layer with a device registry, bounds-checked reads/writes and
  partition offset views.
- Legacy PCI virtio-blk driver (`vda`), polled single-request I/O, 2..256-entry
  queue and a 500 ms completion timeout.
- GPT parsing: protective MBR, header and entry CRC32, up to 16 partitions,
  UTF-16 names; `storage_init()` prefers the `crugroot` partition.
- Read-only ext4: 1/2/4 KiB blocks, 32/64-byte group descriptors, extents and
  the legacy single-indirect mapping, linear directories, clean-state check.
  Rejects journal recovery, htree, `metadata_csum`, `inline_data`, `encrypt`
  and `meta_bg`.
- Generated 16 MiB GPT/ext4 test disk (`make disk`) with `/hello.txt`, read and
  verified during boot; `block ready`, `gpt ready`, `ext4 ready` and
  `storage ready` markers.

## Phase 3B-1 - writable ext4 (done)

- Write path: `ext4_create`, `ext4_mkdir`, `ext4_write_file`, `ext4_truncate`,
  `ext4_unlink`, `ext4_rmdir`, `ext4_rename` and `ext4_fsync`, with extent
  allocation and one extent-tree leaf level.
- Consistent allocation: block bitmap bits relative to
  `s_first_data_block + group * blocks_per_group`, inode bitmap bits relative to
  `group * inodes_per_group`, group free counts plus `used_dirs_count`, and
  parent link counts persisted on every directory change.
- Block-layer flush forwarding and the legacy virtio `VIRTIO_BLK_T_FLUSH`
  command, used by `ext4_fsync` and clean unmount.
- Boot self-test that creates, writes, renames, truncates, deletes, unmounts
  cleanly and remounts read-only to verify persistence.
- Host on-disk gate: `scripts/check-disk.py` extracts the partition, requires
  `e2fsck -fn` to be clean and checks the persisted `/hello.txt` and
  `/selftest/b.txt` contents. `make test` runs it automatically.

## Phase 3B - remaining storage work

- JBD2 journal replay and metadata checksums (the write path is not crash
  consistent yet).
- AHCI and NVMe drivers, multiple block devices and hotplug.
- RAID1/RAID10 over `block.device`, degraded-mode boot and rebuild.
- `crypt.device` with AES-256-XTS and Argon2id key derivation.
- Redundant ESP and a recovery path.

## Phase 4 - devices and userspace

- xHCI USB, HID, UAC2 audio.
- Software framebuffer -> VirtIO-GPU -> vendor GPU drivers.
- Workbench userspace, `.app` bundles, `.crpkg` packages, capability model.
- POSIX-like layer: 64-bit UID/GID, ACLs, process groups, signals.

## Phase 5 - scale-out

- SMP bring-up (APIC/SIPI), IOMMU, PCIe enumeration.
- Networking stack, Secure Boot, virtualisation (VMX/SVM).
- Containers (OCI) and a Docker-style microVM workflow.
- HPC fabric libraries and performance work.

## Spec gaps to resolve

1. The spec sketches Crug namespaces, `let`, attributes and kernel-safe
   ownership annotations; crugc 0.2.3 implements C plus `own/ref/mut/move/
   adopt/drop/borrow/slice/chan/select` and rejects ownership in kernel mode.
   Phases 0-3 therefore use plain C-style Crug. Compiler work is tracked as a
   prerequisite for the capability/ownership-heavy phases (G0.5).
2. The spec targets x86_64 -> aarch64 -> riscv64, but only a native x86_64
   `crugc` exists. aarch64/riscv64 bring-up requires a cross backend.
3. Boot performance targets (<250 ms) now have calibrated stage timings in the
   boot report, but the gate is not enforced yet; boot currently spends most of
   its time in PIT-based TSC calibration.
4. The spec's 16 MiB headless target cannot be met with OVMF: under this
   QEMU/OVMF pair `MEMORY_MB=16`, `32`, `64` and `128` fail before the loader
   runs (`No bootable option` or no output), and 256 MiB is not reliable. The
   kernel itself is small (about 200 KiB load, 128 KiB of that is the PMM
   bitmap for a 4 GiB cap), so the limit is firmware memory use, not kernel
   memory use. A leaner boot path or a lower RAM cap is future work.