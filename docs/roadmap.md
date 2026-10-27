# Roadmap

The specification defines 38 phases/gates. Phase 0 (this tree) establishes the
smallest end-to-end path: firmware -> loader -> kernel -> serial/framebuffer
report -> deterministic exit. Every later phase builds on the same BootInfo
contract, so the loader does not need to change for memory management, SMP or
drivers.

## Phase 0 - bootstrap (done)

- UEFI loader: file I/O, ELF64 loading, GOP, RSDP, memory map, entropy,
  ExitBootServices, handoff.
- Kernel: freestanding Crug, serial/debugcon/framebuffer consoles, `ksnprintf`,
  panic with stack scan, boot report, size budget, host unit tests, QEMU boot test.

## Phase 1 - memory and CPU

- Bitmap physical allocator over `BootInfo.memory_map` (4 KiB pages, never the
  kernel, framebuffer or boot metadata).
- Kernel page tables with W^X: no writable+executable mapping, NX on data.
- GDT/IDT, exception handlers, per-CPU data via `%gs`.
- APIC + HPET/TSC timer; TSC frequency calibration from ACPI/CPUID.

## Phase 2 - execution model

- Threads with kernel stacks, context switching, per-CPU run queues.
- Scheduling classes `IDLE`, `NORMAL`, `INTERACTIVE`, `REALTIME_SYSTEM`.
- Handles and a single syscall dispatcher table.
- `MsgPort` IPC with typed messages, O(1) send/receive.

## Phase 3 - storage and filesystem

- Block layer over AHCI/NVMe; GPT partition parsing.
- RAID1/RAID10 above `block.device`.
- crypt.device with AES-256-XTS and Argon2id key derivation.
- EXT4 read/write over crypt.device.

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
   Phase 0 therefore uses plain C-style Crug. Compiler work is tracked as a
   prerequisite for the capability/ownership-heavy phases (G0.5).
2. The spec targets x86_64 -> aarch64 -> riscv64, but only a native x86_64
   `crugc` exists. aarch64/riscv64 bring-up requires a cross backend.
3. Boot performance targets (<250 ms) need a timer calibration and boot-stage
   instrumentation harness; phase 0 only records a raw TSC delta.
4. The spec's single 16 MiB headless target is validated by `make run
   MEMORY_MB=16` once phase 1 memory management lands (the firmware itself
   needs more in phase 0).
