# CrugOS — Complete Kernel, Desktop, Virtualization, Container and HPC Development Specification

- **Status:** Initial engineering specification, implemented incrementally (progress lives in `docs/roadmap.md`)
- **Primary implementation language:** Crug (`crugc`)
- **Architecture order:** x86-64 only — the AArch64 and RISC-V 64 phases were dropped on 2026-10-30 at the project owner's request; the architecture-specific sections below (ports, virtualisation, HPC clusters) are kept as historical design notes and are explicitly out of scope
- **Kernel style:** Small monolithic core with strict subsystem boundaries and large policy components in user space
- **Desktop philosophy:** Amiga Workbench / Exec inspired, modernized for SMP, memory protection, GPU composition, security and HiDPI
- **Primary goals:** Tiny footprint, very fast boot, native message passing, encrypted storage, GPU/audio support, native HPC clustering, virtualization, OCI/Docker workflows, and eventual self-hosting

---

# 1. Project Goals

CrugOS is a new 64-bit operating system written primarily in Crug.

It is not a Linux clone. It borrows the best ideas from classic AmigaOS—lightweight tasks, message ports, asynchronous devices, libraries, Screens, a spatial Workbench desktop—and combines them with modern OS requirements:

- protected per-process virtual address spaces;
- multicore/SMP;
- asynchronous I/O;
- encrypted storage by default;
- application capabilities and signed packages;
- native multiuser identities, groups, sessions and Linux-style permissions;
- modern networking;
- USB;
- audio;
- GPU acceleration;
- virtualization;
- native containers;
- Docker/Linux-container compatibility;
- native HPC clustering;
- x86-64, AArch64 and RV64 support;
- a self-hosted Crug toolchain.

The project starts from scratch.

---

# 2. Permanent Architectural Rules

## 2.1 64-bit only

Supported architectures:

```text
x86_64
aarch64
riscv64
```

Never support:

```text
x86-32
AArch32
RV32
16-bit userspace
legacy executable ABIs
```

## 2.2 Crug-first

Assume `crugc` is mature and provides:

- freestanding compilation;
- GCC-quality optimized machine code;
- no mandatory runtime;
- no mandatory GC;
- no hidden allocation;
- explicit atomics and memory ordering;
- ownership/borrowing;
- exact C layout;
- packed/aligned structs;
- inline assembly and architecture intrinsics;
- kernel mode;
- x86-64, AArch64 and RV64 backends.

Target composition:

```text
Crug        98–99%
Assembly     1–2%
C            0%
```

Assembly is allowed for:

- reset/entry;
- syscall/trap entry;
- context switching;
- AP/hart startup;
- special low-level instructions.

## 2.3 Strict architecture boundary

Generic kernel code must never directly call architecture-specific instructions.

Required:

```crug
arch::irq::disable();
arch::vm::activate(space);
arch::cpu::send_ipi(cpu, IpiKind::Reschedule);
```

Forbidden outside `arch/`:

```crug
write_cr3(...);
apic_send_ipi(...);
riscv_write_satp(...);
arm_write_ttbr0(...);
```

## 2.4 Small kernel, large user space

Kernel owns mechanism:

- scheduler;
- address spaces;
- memory allocation;
- IPC;
- timers;
- interrupts;
- DMA/IOMMU;
- block I/O;
- device queues;
- protected GPU submission;
- virtualization primitives.

User space owns policy:

- Workbench;
- GUI toolkit;
- package manager;
- audio mixer;
- shader compiler;
- GPU UMD;
- container manager;
- VM manager;
- cluster scheduler;
- MPI layer;
- most HPC collectives.

## 2.5 Security by default

Default installation must use:

- encrypted root storage;
- verified/signed boot where supported;
- W^X;
- NX/PXN/UXN;
- protected userspace;
- signed packages by default;
- explicit app capabilities;
- IOMMU isolation where possible;
- safe key lifecycle;
- no plaintext swap;
- no raw RAM dumps by default.

---

# 3. Performance and Footprint Budgets

These are engineering budgets, not promises.

```text
Kernel entry -> usable Workbench:  < 250 ms target
                                     500 ms hard budget

Base stripped kernel:              < 1 MiB stretch goal
                                   < 2 MiB hard budget

Headless minimum RAM:               16 MiB target
Graphical minimum RAM:              32 MiB target
Comfortable desktop:                64 MiB+

Local MsgPort operations:           O(1)
Scheduler enqueue/dequeue:          O(1) average
```

Optional components must not inflate the minimal boot image unnecessarily.

Examples:

```text
nvidia.device
hda.device
hypervisor extensions
RDMA driver
debugger
development SDK
```

may be shipped as privileged system components loaded after the base kernel.

---

# 4. Repository Layout

```text
crugos/
├── kernel/
│   ├── arch/
│   │   ├── x86_64/
│   │   ├── aarch64/
│   │   └── riscv64/
│   ├── core/
│   ├── mm/
│   ├── sched/
│   ├── ipc/
│   ├── syscall/
│   ├── exec/
│   ├── fs/
│   ├── block/
│   ├── crypto/
│   ├── pci/
│   ├── net/
│   ├── usb/
│   ├── audio/
│   ├── gpu/
│   ├── iommu/
│   ├── virt/
│   ├── security/
│   └── lib/
│
├── drivers/
│   ├── nvme/
│   ├── virtio/
│   ├── net/
│   ├── usb/
│   ├── audio/
│   ├── gpu/
│   └── rdma/
│
├── user/
│   ├── init/
│   ├── services/
│   ├── workbench/
│   ├── shell/
│   ├── libraries/
│   ├── containerd/
│   ├── vmm/
│   ├── hpc/
│   └── sdk/
│
├── boot/
├── apps/
├── sdk/
├── tests/
├── tools/
└── docs/
```

---

# 5. Crug Kernel Compilation Contract

`crugc --kernel` must guarantee:

```text
no libc
no CRT dependency
no hidden malloc
no GC
no exceptions
no stack unwinding
no mandatory RTTI
panic=abort
predictable layout
LTO support
section control
freestanding ELF output
```

Required language constructs:

```text
@repr(C)
@packed
@align(N)
@section(...)
@percpu
volatile
atomic<T>
unsafe {}
own
borrow
move
```

Required atomic orders:

```text
relaxed
acquire
release
acq_rel
seq_cst
```

---

# 6. Common Architecture API

Generic kernel code depends on this type of contract:

```crug
namespace arch {
    fn early_init(info: BootInfo);
    fn late_init();

    namespace cpu {
        fn id() -> CpuId;
        fn count() -> usize;
        fn start_secondary(cpu: CpuId, entry: PhysAddr) -> Result;
        fn send_ipi(cpu: CpuId, kind: IpiKind);
        fn halt();
        fn relax();
    }

    namespace irq {
        fn disable() -> IrqState;
        fn restore(state: IrqState);
        fn enable();
        fn register(irq: Irq, handler: Handler);
        fn eoi(irq: Irq);
    }

    namespace vm {
        fn create_space() -> AddressSpaceRoot;
        fn map(...);
        fn unmap(...);
        fn protect(...);
        fn activate(...);
        fn flush(...);
    }

    namespace timer {
        fn monotonic_ns() -> u64;
        fn arm_deadline(ns: u64);
    }

    namespace context {
        fn switch(old: *Context, new: *Context);
        fn enter_user(frame: *UserFrame) -> !;
    }

    namespace virt {
        fn supported() -> bool;
        fn create_vm(...) -> Result<VmArch>;
        fn create_vcpu(...) -> Result<VcpuArch>;
        fn run(vcpu: *VcpuArch) -> VmExit;
    }
}
```

---

# 7. Common Boot Contract

Every architecture must produce:

```crug
struct BootInfo {
    memory_map: []MemoryRegion;
    framebuffer: Option<FramebufferInfo>;
    command_line: str;
    acpi_rsdp: Option<PhysAddr>;
    fdt: Option<PhysAddr>;
    initrd: Option<MemoryRange>;
    firmware_tables: []FirmwareTable;
    boot_cpu_id: u64;
    entropy_seed: [u8; 64];
}
```

Then call:

```crug
kernel_main(boot_info);
```

---

# 8. Phase A — x86-64 Bring-Up

x86-64 is the first production architecture.

## 8.1 Boot

Use UEFI only initially.

```text
UEFI
 -> CrugOS EFI loader
 -> kernel ELF
 -> memory map
 -> GOP framebuffer
 -> ACPI pointer
 -> entropy
 -> ExitBootServices
 -> kernel entry
```

Acceptance:

- boots in QEMU;
- boots on one physical UEFI machine;
- prints through serial and framebuffer;
- deterministic kernel entry.

## 8.2 CPU initialization

Implement:

- GDT;
- IDT;
- exception vectors;
- syscall MSRs;
- XSAVE/FPU policy;
- NX;
- SMEP/SMAP where available;
- APIC;
- I/O APIC;
- HPET/TSC-deadline or suitable timer;
- ACPI CPU discovery.

## 8.3 SMP

Bring up APs using the x86 startup trampoline.

Milestones:

1. all CPUs enter kernel;
2. per-CPU state;
3. per-CPU idle task;
4. IPIs;
5. reschedule IPI;
6. TLB shootdown.

Acceptance:

```text
1, 2, 4, 8+ vCPU QEMU boots
stress task switching for 1 hour
no missed wakeups
no global scheduler corruption
```

---

# 9. Early Debugging

Must exist before advanced subsystems:

- serial console;
- framebuffer console;
- minimal `printf`;
- panic;
- register dump;
- stack trace;
- per-CPU trace ring;
- timestamped boot profiler;
- QEMU debug exit;
- separate debug ELF.

Formatter:

```text
%c %s %d %u %x %p
```

No floating-point formatting in kernel.

---

# 10. Physical Memory

Initial allocator:

```text
4 KiB pages
bitmap metadata
```

Do not use a large `struct page` for every page.

API:

```crug
phys::alloc_page();
phys::free_page(page);
phys::alloc_pages(count, alignment);
```

Later:

- per-CPU page caches;
- hierarchical allocator if scaling requires it.

---

# 11. Kernel Heap

Structure:

```text
physical pages
 -> small-object caches
 -> generic kmalloc fallback
```

Suggested classes:

```text
16 32 64 128 256 512 1024 2048 4096
```

Rules:

- no large stack arrays;
- no recursion in kernel;
- bounded stack use;
- guard pages once VM exists.

---

# 12. Virtual Memory

Common VM objects:

```text
AddressSpace
VmArea
VmFlags
MapFlags
```

Policy:

```text
Writable => non-executable by default
```

User tasks receive protected private address spaces.

Multiple tasks may share one process address space.

Initial process creation:

```text
spawn()
```

Implement `fork()`/COW only later if POSIX compatibility needs it.

---

# 13. Interrupts and Timers

Hard interrupt handlers do minimum work.

Long processing is deferred to:

- worker task;
- completion queue;
- message port.

Common timer:

```text
monotonic nanoseconds
tickless deadline scheduling
```

Architecture mappings:

```text
x86-64:   IDT + APIC/IOAPIC
AArch64:  vector table + GICv3
RV64:     stvec + PLIC, later AIA
```

---

# 14. Scheduler

Initial scheduler:

- preemptive;
- per-CPU run queues;
- round-robin within priority;
- sleep/wakeup queues;
- CPU affinity;
- periodic load balancing.

Task states:

```text
RUNNING
RUNNABLE
BLOCKED
SLEEPING
STOPPED
ZOMBIE
```

Priority classes:

```text
IDLE
NORMAL
INTERACTIVE
REALTIME_SYSTEM
```

`REALTIME_SYSTEM` is reserved for trusted audio/device/system workers.

---

# 15. Exec-Like Native Object Model

## Process

Owns:

- address space;
- handles;
- security context;
- capabilities;
- tasks;
- environment;
- namespace;
- executable identity.

## Task

Schedulable execution context.

## MsgPort<T>

Primary IPC primitive.

Requirements:

- O(1) queue operations;
- SMP-safe;
- waitable;
- typed in Crug;
- move ownership where possible;
- no kernel pointer exposure.

Example:

```crug
own Message msg = Message::new();
port.send(move msg);
let reply = reply_port.recv();
```

## Signals

Compact event bitmap for simple wakeups.

Structured data always uses messages.

## Unified wait

A task can wait on:

```text
MsgPort
timer
IO completion
socket readiness
task exit
process exit
selected device event
```

---

# 16. Syscalls and Handles

No raw kernel pointer is exposed.

Objects use process-local handles:

```text
Handle<Task>
Handle<Port>
Handle<File>
Handle<Socket>
Handle<Device>
Handle<SharedMemory>
Handle<Vm>
Handle<Vcpu>
```

Architecture gateways:

```text
x86-64:   SYSCALL
AArch64:  SVC
RV64:     ECALL
```

All normalize to one generic dispatcher.

---

# 17. Executables

Use ELF64 on every architecture.

Initial support:

- `ET_EXEC`;
- `PT_LOAD`;
- R/W/X permissions;
- user stack;
- argv/env.

Later:

- PIE;
- ASLR;
- dynamic/shared libraries;
- TLS.

Use conventional ABIs where practical:

```text
x86-64 SysV-like
AAPCS64-like
RISC-V psABI-like
```

---

# 18. PCI/PCIe

PCI must be generic, not x86-specific.

Implement:

1. enumeration;
2. config-space access;
3. BARs;
4. capabilities;
5. MSI;
6. MSI-X;
7. PCIe capabilities;
8. DMA association;
9. IOMMU association.

PCIe is reused on x86-64, AArch64 and RISC-V.

---

# 19. Storage Stack

The storage stack must support one or many physical disks without changing VFS or EXT4 semantics.

```text
EXT4
  |
VFS
  |
crypt.device
  |
raid.device
  |
member partitions
  |
block layer
  |
NVMe / VirtIO block
```

For a single-disk system, `raid.device` may collapse to a simple pass-through logical block device.

For multi-disk installations, RAID is configured during installation and assembled before the encrypted root volume is unlocked. The normal ordering is therefore **RAID below encryption**: RAID provides redundancy and one logical block address space; encryption protects that logical device with one volume-master key.

A small EFI System Partition remains outside the encrypted RAID root. On redundant boot installations, a bootable ESP is created on each selected boot disk and kept synchronized by the system updater.

---

# 20. GPT

Implement:

- protective MBR recognition;
- primary/backup headers;
- CRC verification;
- partition entries;
- UUIDs;
- EFI System Partition;
- CrugOS encrypted-root GUID.

---

# 22. NVMe

Phases:

## NVMe-0

- PCI discover;
- BAR map;
- reset;
- admin queues;
- identify controller/namespace.

## NVMe-1

- I/O queues;
- read;
- write;
- flush;
- timeout.

## NVMe-2

- multiple queues;
- MSI-X;
- CPU queue affinity;
- scatter/gather.

## NVMe-3

- recovery/reset;
- health;
- power management.

---

# 21. Native Software RAID

CrugOS provides a native software RAID layer, conceptually `raid.device`, below encryption and above physical member partitions. It is independent of EXT4.

Goals:

- automatic redundancy when multiple installation disks are selected;
- deterministic array assembly;
- degraded boot;
- online rebuild/resynchronization;
- hot-spare support;
- architecture-independent operation on x86-64, AArch64 and RV64;
- no dependency on motherboard/fake-RAID.

## 21.1 Default policy

RAID is enabled by default when the installer is explicitly given multiple disks for the system volume. CrugOS must **never automatically erase unrelated disks merely because they are detected**.

Recommended safe defaults:

```text
1 disk      single logical device
2 disks     RAID1
3 disks     RAID1 + hot spare
4 disks     RAID10
5+ disks    RAID10, optional spare(s)
```

RAID0 is explicit-only and never the default.

## 21.2 RAID levels

Version 1:

```text
RAID1       required
RAID10      required
RAID0       optional, explicit-only
```

Later:

```text
RAID5
RAID6
```

Parity RAID is intentionally deferred because correct write-hole handling, partial-stripe writes, parity verification and crash recovery add substantial complexity. Reliability takes priority over capacity efficiency.

## 21.3 Storage layering

Default redundant encrypted root:

```text
NVMe0 -> member partition --\
                              +--> raid.device -> crypt.device -> EXT4
NVMe1 -> member partition --/
```

Advantages:

- one encrypted logical volume and one master key;
- RAID rebuild operates on ciphertext and does not require filesystem knowledge;
- a stolen member contains encrypted sectors;
- EXT4 sees an ordinary block device;
- disk replacement does not change filesystem semantics.

RAID metadata is not secret, but must be checksummed and strictly validated.

## 21.4 Member metadata

Each member stores redundant metadata containing at least:

```text
magic/version
array UUID
member UUID
RAID level
member slot/role
member count
logical sector size
chunk/stripe size
array generation
member generation
clean/dirty state
logical size
feature flags
checksum
```

Keep primary and backup metadata copies. Ambiguous or conflicting metadata must never be repaired destructively by guessing.

## 21.5 RAID1

Writes are mirrored to all required healthy members. Reads may be balanced between mirrors. On a read error:

```text
member A fails read
 -> read member B
 -> return valid data
 -> schedule repair of A if possible
```

## 21.6 RAID10

Use explicitly recorded mirrored stripe groups. Do not derive mirror pairing from discovery order.

RAID10 must support parallel reads/writes, degraded operation when each required mirror group retains a valid member, and online replacement.

## 21.7 Degraded boot

A redundant root array must boot whenever sufficient valid members remain.

Examples:

```text
RAID1:  one healthy mirror remains -> boot degraded
RAID10: each required mirror group has a survivor -> boot degraded
```

Workbench shows a prominent non-blocking storage warning rather than refusing to boot.

## 21.8 Rebuild and replacement

Support:

```text
failed member
 -> select/insert replacement
 -> initialize member metadata
 -> background resync
 -> verify
 -> mark healthy
```

Rebuild is dynamically throttled:

```text
idle             aggressive rebuild
interactive load reduced rebuild rate
heavy I/O        strong throttling
```

## 21.9 Dirty tracking

Use a persistent write-intent/dirty-region bitmap or equivalent mechanism so an unclean shutdown does not require a full-array resync when only a small region changed. Metadata ordering must be crash-safe.

## 21.10 Scrubbing

Provide online scrub:

```bash
raid scrub System:
```

Scrub reads redundant copies, detects unreadable sectors/mismatches, repairs when a trustworthy copy can be identified, and reports ambiguous corruption instead of silently choosing.

## 21.11 TRIM/discard

Propagate discard only when semantics are proven safe for the RAID level, every member, and the configured encryption policy. The first implementation may conservatively disable discard for encrypted RAID.

## 21.12 Redundant UEFI boot

UEFI firmware cannot be expected to understand CrugOS RAID. Every selected boot disk therefore receives its own EFI System Partition:

```text
Disk0: ESP + RAID member
Disk1: ESP + RAID member
...
```

Bootloader/kernel updates synchronize every healthy ESP.

Required fault test:

```text
power off
remove primary disk
power on
firmware boots another ESP
CrugOS assembles degraded RAID
Workbench starts
```

## 21.13 Hot spares

Support explicit hot spares. For three selected system disks, the recommended default is:

```text
2-way RAID1 + 1 hot spare
```

This deliberately favors reliability.

## 21.14 Management

Kernel implements the RAID data path. A privileged user-space service such as `storaged` implements policy and administration.

CLI examples:

```bash
raid status
raid list
raid create --level 1 nvme0 nvme1
raid add System: nvme2
raid replace System: nvme1 nvme3
raid remove System: nvme2
raid scrub System:
raid pause-rebuild System:
raid resume-rebuild System:
```

Workbench exposes a Storage panel with member state, rebuild progress, encryption state and filesystem state.

## 21.15 Permissions

Array modification requires:

```text
CAP_SYS_STORAGE
```

Ordinary users may query safe health information but cannot access raw active members.

## 21.16 Virtualization and containers

VM images stored on the RAID volume automatically inherit host redundancy. Active RAID members cannot be passed through directly to a VM. Containers see logical filesystems only and never RAID members.

## 21.17 HPC

System/user data should use RAID1/10. Administrators may explicitly configure RAID0 for disposable HPC scratch volumes where local bandwidth matters more than redundancy. RAID0 is never used automatically for system data.

## 21.18 Acceptance criteria

Before production status:

- RAID1 and RAID10 operate correctly;
- auto-assembly is deterministic;
- encrypted root works above RAID;
- degraded root boot succeeds after member removal;
- redundant ESP boot succeeds after losing the primary disk;
- failed reads recover from a mirror;
- online replacement/rebuild works;
- rebuild survives reboot/power interruption;
- dirty tracking is crash-safe;
- scrub detects injected corruption;
- permissions prevent unauthorized array mutation;
- VM/container paths cannot bypass member ownership;
- fault injection covers disk disappearance during active I/O.

# 23. Encryption by Default

Normal install:

```text
GPT
├── EFI System Partition
└── encrypted CrugOS root
    └── EXT4
```

Initial cipher:

```text
AES-256-XTS
```

Key hierarchy:

```text
password/PIN
 -> Argon2id
 -> key-encryption key
 -> encrypted random volume master key
 -> XTS keys
```

Requirements:

- multiple keyslots;
- recovery key;
- authenticated header;
- key zeroization;
- secrets excluded from logs;
- hardware AES where available;
- software fallback.

TPM mode:

```text
TPM + PIN
```

recommended where available.

Encrypted swap only.

No hibernation until encrypted image semantics are designed.

---

# 24. EXT4

Only required persistent filesystem.

## Stage A — read-only

Support a controlled feature profile:

- superblock;
- group descriptors;
- inode tables;
- directories;
- extents;
- files;
- symlinks;
- metadata validation.

Unknown incompatible feature:

```text
fail mount safely
```

## Stage B — writes

Add:

- allocation;
- create;
- mkdir;
- unlink;
- rename;
- truncate;
- extent updates;
- checksums;
- fsync.

## Stage C — journal

Implement required JBD2 semantics:

- replay;
- ordered writes;
- crash consistency;
- power-failure testing.

---

# 25. VFS and Amiga-Style Volumes

Small VFS objects:

```text
File
Directory
Mount
VfsNode
Path
```

User-visible namespaces:

```text
System:
Applications:
Projects:
Downloads:
Prefs:
RAM:
USB0:
```

Example:

```text
Applications:CrugIDE.app/
Projects:Kernel/
System:Libraries/
```

Assignments:

```text
SDK: -> System:Developer/SDK/
```

---

# 26. Networking

Initial stack:

```text
Ethernet
ARP
IPv4
ICMP
UDP
DHCP
TCP
DNS
sockets
```

Initial NICs:

```text
VirtIO-net
Intel e1000
```

Later:

- modern Intel NIC;
- dedicated RDMA-capable NIC.

Packet buffers should support ownership-aware zero-copy evolution.

---

# 27. USB

Initial host controller:

```text
xHCI only
```

Implement:

- enumeration;
- hubs;
- descriptors;
- control;
- bulk;
- interrupt;
- isochronous;
- hotplug;
- cancellation.

Initial classes:

```text
HID keyboard
HID mouse
USB Audio Class 2
```

Later:

```text
mass storage
USB MIDI
selected device classes
```

---

# 28. Audio

Architecture:

```text
apps
 -> audio.library
 -> user-space mixer
 -> audio.device
     -> UAC2
     -> HDA optional
     -> VirtIO sound optional
```

## UAC2 initial profile

- stereo output;
- stereo capture;
- PCM;
- 16/24/32-bit;
- 44.1/48/96 kHz;
- isochronous transfer support;
- feedback handling;
- hardware volume/mute when present.

## HDA

Optional generic onboard audio:

```text
Intel HDA / Azalia
```

Implement:

- controller;
- CORB/RIRB;
- codec enumeration;
- widget routing;
- basic DAC/ADC;
- DMA;
- jack detect later.

Latency goals:

```text
20 ms initial
10 ms next
~5 ms long-term
```

---

# 29. Graphics Stage 1 — Software

Desktop must work without 3D acceleration.

```text
firmware framebuffer
 -> software graphics.library
 -> compositor
 -> Workbench
```

Primitives:

- pixel;
- line;
- fill;
- blit;
- alpha;
- clipping;
- text;
- surfaces.

---

# 30. Graphics Stage 2 — VirtIO GPU

First accelerated target.

Goals:

- scanout;
- shared buffers;
- accelerated rendering;
- Venus/Vulkan where practical;
- Zink/OpenGL later.

This validates the user/kernel GPU split before physical GPU work.

---

# 31. GPU Kernel Boundary

Kernel knows:

```text
context
buffer
GPU VA
queue/channel
command submission
fence
interrupt
scanout
```

Kernel does not know:

```text
triangles
materials
shaders
OpenGL state
```

Suggested API:

```text
GPU_CREATE_CONTEXT
GPU_ALLOC_BUFFER
GPU_MAP_BUFFER
GPU_MAP_GPU_VA
GPU_SUBMIT
GPU_WAIT_FENCE
GPU_QUERY
```

---

# 32. NVIDIA Acceleration

Preferred stack:

```text
OpenGL
 -> Zink
 -> Vulkan
 -> NVK-derived/ported UMD
 -> CrugOS NVIDIA winsys
 -> nvidia.device
 -> GSP/GPU
```

Start with one specific Turing-or-newer generation/card.

Milestones:

1. PCI;
2. BAR;
3. firmware/GSP;
4. GPU info;
5. VRAM;
6. GPU VM;
7. command channel;
8. fences;
9. interrupts;
10. NVK interface;
11. Vulkan triangle;
12. texture/depth;
13. Zink;
14. OpenGL;
15. modesetting;
16. GPU Workbench;
17. reset/recovery.

Use open NVIDIA kernel modules and Nouveau/NVK as references.

Do not port Linux DRM wholesale.

---

# 33. IOMMU

Generic API:

```crug
iommu::domain_create();
iommu::attach(device, domain);
iommu::map(...);
iommu::unmap(...);
```

Backends:

```text
x86-64:    VT-d / AMD IOMMU
AArch64:   SMMUv3
RV64:      RISC-V IOMMU
```

Required for hardened:

- user-controlled GPU submission;
- RDMA;
- high-performance zero-copy device access.

---

# 34. Workbench-Style Desktop

Keep:

- spatial desktop;
- disks/volumes as objects;
- drawers;
- compact windows;
- global menu/screen bar;
- Screens;
- drag/drop;
- send-to-back/front;
- Commodities-style utilities.

Modernize:

- vector icons;
- HiDPI;
- GPU compositing;
- dark/light themes;
- modern typography;
- restrained transparency;
- 80–150 ms normal UI animation;
- multi-monitor later.

Architecture:

```text
Workbench
 -> intuition.library
 -> graphics.library
 -> compositor
     -> software
     -> Vulkan
```

---

# 35. Application Bundles

```text
MyApp.app/
├── manifest.toml
├── icon.svg
├── bin/
│   ├── x86_64/
│   ├── aarch64/
│   └── riscv64/
├── Libraries/
└── Resources/
```

Manifest includes:

- app ID;
- version;
- minimum OS;
- permissions;
- supported architectures;
- file associations;
- optional services.

---

# 36. Installation and Packages

Normal install:

```text
copy MyApp.app -> Applications:
```

Distribution:

```text
.crpkg
```

Pipeline:

```text
verify signature
 -> validate manifest
 -> show permissions
 -> atomic extraction
 -> immediate availability
```

Rules:

- no arbitrary installer scripts by default;
- app-local dependencies;
- stable system libraries;
- atomic updates;
- rollback;
- normal app install without admin rights;
- privileged `.systempkg` for drivers/system components.

---

# 37. Application Capabilities

Examples:

```text
network
microphone
camera
clipboard
notifications
background
Projects: read/write
Downloads: read
USB class
GPU compute
VM creation
container management
HPC cluster access
```

Apps do not receive unrestricted `System:` access automatically.

---


# 38. Multiuser, Groups and Linux-Style Permissions

Multiuser support is a core CrugOS security primitive. It is not an optional compatibility feature.

CrugOS natively implements the familiar Unix/Linux model:

```text
UID
primary GID
supplementary groups
file owner UID/GID
owner/group/other rwx bits
setuid
setgid
sticky bit
umask
ACLs
```

CrugOS capabilities are layered on top of this model for finer-grained privilege control.

## 37.1 Identity model

Use 64-bit internal identifiers:

```crug
type UserId = u64;
type GroupId = u64;
type SessionId = u64;
```

Conventions:

```text
UID 0 / GID 0    root/system administrator
low IDs           system/service identities
normal IDs        human users
```

The kernel authorizes numeric IDs and capabilities, never username strings.

Each user record contains at least:

```text
UID
login name
display name
primary GID
supplementary groups
home path
default shell
account state
authentication configuration
resource policy
```

## 37.2 Groups

Groups are first-class principals.

Typical groups:

```text
root
users
wheel
developers
audio
video
network
storage
virtualization
containers
hpc
```

Each process has:

```text
real UID/GID
effective UID/GID
saved UID/GID
supplementary GIDs
capability set
session ID
security-domain ID
```

## 37.3 Linux-style mode bits

Every ordinary VFS object exposes:

```text
owner UID
owner GID
owner rwx
group rwx
other rwx
```

Example:

```text
-rwxr-x--- alice developers compiler
```

equivalent to:

```text
0750
```

Required tools/semantics:

```bash
chmod 750 compiler
chown alice compiler
chgrp developers compiler
umask 027
```

Directory permissions follow Unix semantics:

```text
r = list entries
w = create/remove/rename entries
x = traverse/search
```

## 37.4 Special bits

Support:

```text
setuid
setgid
sticky
```

Setgid directories inherit the directory group for newly created children.

Sticky directories behave like Unix `/tmp`.

Setuid is supported for compatibility, but CrugOS should prefer narrow file capabilities for privileged system tools.

Untrusted/removable mounts default to:

```text
nosuid
```

## 37.5 ACLs

Support POSIX-style ACL behavior in addition to mode bits.

Example:

```text
owner:alice:rwx
group:developers:r-x
user:bob:rw-
group:qa:r--
other:---
```

Required commands:

```bash
getfacl file
setfacl -m u:bob:rw file
setfacl -m g:qa:r file
```

Directories support inherited/default ACLs.

## 37.6 Permission evaluation

Ordinary VFS access:

```text
1. validate process credentials
2. apply security-domain restrictions
3. apply privileged capability overrides
4. evaluate ACL when present
5. if effective UID == owner UID, use owner permissions
6. else if any process GID matches file GID/ACL, use group permissions
7. otherwise use other permissions
8. deny if operation is not granted
```

## 37.7 Root and capabilities

Keep:

```text
root = UID 0
```

for familiar administration and POSIX/Linux compatibility.

Internally, privileged actions use explicit capabilities instead of scattered `uid == 0` checks.

Initial capabilities:

```text
CAP_CHOWN
CAP_DAC_OVERRIDE
CAP_DAC_READ_SEARCH
CAP_SETUID
CAP_SETGID
CAP_KILL
CAP_NET_ADMIN
CAP_NET_RAW
CAP_SYS_MOUNT
CAP_SYS_STORAGE
CAP_SYS_MODULE
CAP_SYS_VM
CAP_SYS_CONTAINER
CAP_SYS_HPC
CAP_SYS_ADMIN
```

Root normally receives the full administrative capability set at login, but capabilities can be dropped.

## 37.8 File capabilities

Prefer file capabilities over broad setuid where possible.

Examples:

```text
System:Tools/ping
    CAP_NET_RAW

System:Tools/vmctl
    CAP_SYS_VM
```

Only trusted signed system packages may install privileged file capabilities.

## 37.9 Home directories

Each user receives a private home:

```text
Users:alice/
Users:bob/
```

At login:

```text
Home:
```

maps to the active user's home.

Typical layout:

```text
Home:Documents/
Home:Projects/
Home:Applications/
Home:Prefs/
Home:Data/
Home:Cache/
```

Another user's home is inaccessible unless mode bits/ACLs explicitly permit access.

## 37.10 Per-user applications

Use:

```text
Home:Applications/    per-user
Applications:         system-wide
```

Workbench merges both views.

Per-user installation requires no administrator privilege.

System-wide installation requires the appropriate package-management capability.

## 37.11 Authentication

User-space service:

```text
authd
```

supports:

- password;
- PIN where appropriate;
- TPM-backed authentication;
- passkeys/WebAuthn later;
- recovery credentials;
- remote/directory authentication later.

Passwords are stored only as versioned, salted memory-hard verifiers, initially Argon2id.

## 37.12 Login and sessions

Services:

```text
logind
sessiond
```

manage:

```text
login/logout
screen lock
session creation
fast user switching
remote sessions
seat assignment
idle policy
session accounting
```

Every interactive session has:

```text
SessionId
UID/GIDs
capability set
Workbench instance
Screens
clipboard namespace
notification namespace
audio policy
device grants
```

Multiple users may remain logged in simultaneously.

## 37.13 Multiple graphical Workbench sessions

Never assume one global Workbench.

Example:

```text
alice
  Workbench
  Development Screen

bob
  Workbench
  Music Screen

remote user
  remote Workbench session
```

Fast switching backgrounds one session without terminating it.

## 37.14 Device permissions

Groups can grant ordinary access:

```text
audio
video
virtualization
containers
hpc
```

But dangerous operations such as raw DMA, arbitrary MMIO, kernel-driver management or PCI passthrough require explicit privileged capabilities.

## 37.15 sudo-style elevation

Provide a native `sudo`-style service/tool.

Example:

```bash
sudo useradd bob
sudo pkg install-system nvidia.systempkg
```

Elevation:

1. checks policy;
2. authenticates if required;
3. grants only necessary capabilities;
4. audits the operation;
5. discards elevated credentials afterward.

Graphical Workbench sessions should never need to run entirely as root.

## 37.16 Administrative tools

Provide familiar tools or compatible equivalents:

```text
id
whoami
groups
useradd
userdel
usermod
groupadd
groupdel
groupmod
passwd
su
sudo
chmod
chown
chgrp
umask
getfacl
setfacl
who
w
last
```

Using familiar spellings significantly simplifies administration and software porting.

## 37.17 EXT4 requirements

CrugOS EXT4 must preserve and expose:

```text
UID
GID
mode bits
setuid
setgid
sticky
ACLs
extended attributes
```

Native CrugOS metadata not represented by ordinary permission fields may use namespaced extended attributes or a protected metadata layer.

## 37.18 Native containers

A native container has its own security domain and optional UID/GID mapping.

Container root must never imply host root.

Example mapping:

```text
container UID 0 -> host UID 100000
container UID 1 -> host UID 100001
...
```

The mapping mechanism should be simpler than Linux user namespaces while preserving the same security property.

## 37.19 Docker/Linux microVM

Linux containers inside the managed Docker microVM use normal Linux UIDs/GIDs internally.

Shared host folders require explicit mapping.

Linux guest UID 0 never maps automatically to CrugOS host UID 0.

## 37.20 VM ownership

Every VM has an owner/group and permissions.

Example:

```text
DevelopmentLinux
owner: alice
group: developers
```

Authorization controls:

```text
start
stop
console
modify
attach storage
modify networking
snapshot
delete
```

PCI passthrough always requires elevated capability.

## 37.21 HPC ownership

Every HPC job carries authenticated user/group identity.

Job metadata includes:

```text
JobId
UserId
GroupId
cluster ABI
allocated nodes
resources
security mode
```

Remote nodes must not trust numeric UID/GID values from unauthenticated machines.

Shared HPC storage obeys normal ownership/ACL rules.

## 37.22 Service users

System daemons run as dedicated restricted identities wherever possible:

```text
authd
networkd
pkgd
containerd
vmm
clusterd
```

Each receives only its required capabilities.

Do not run all services as root.

## 37.23 Audit

Audit at least:

```text
login success/failure
password changes
user/group modifications
sudo/elevation
system package installation
mount/unmount
VM privileged changes
container privilege changes
PCI passthrough
HPC administrative changes
security-policy changes
```

Records include:

```text
timestamp
UID
session
operation
target
result
origin
```

## 37.24 Multiuser acceptance criteria

Before CrugOS 1.0:

- UID/GID ownership works;
- primary and supplementary groups work;
- `rwx` mode bits work;
- `chmod/chown/chgrp/umask` work;
- setgid inheritance works;
- sticky directories work;
- ACLs persist on EXT4;
- file capabilities work;
- root capability behavior is tested;
- users cannot read private homes by default;
- multiple simultaneous shells work;
- multiple graphical Workbench sessions work;
- per-user applications work;
- container root cannot become host root;
- VM ownership is enforced;
- HPC job identity is authenticated;
- permission regression tests run in CI.



## 38.25 Virtual Consoles and Linux-Style VT Switching

CrugOS supports multiple independent local virtual consoles in the same spirit as Linux virtual terminals. This is a core local-session and recovery feature, not merely a shell convenience.

The default workstation layout is:

```text
VT1   text console
VT2   text console
VT3   text console
VT4   text console
VT5   text console
VT6   text console
VT7   graphical Workbench session
```

The default switching keys are:

```text
Ctrl + Alt + F1   -> VT1
Ctrl + Alt + F2   -> VT2
Ctrl + Alt + F3   -> VT3
Ctrl + Alt + F4   -> VT4
Ctrl + Alt + F5   -> VT5
Ctrl + Alt + F6   -> VT6
Ctrl + Alt + F7   -> graphical Workbench VT
```

Additional VTs such as `F8`, `F9`, and later keys may be configured.

### 38.25.1 VT architecture

A virtual terminal is a first-class session endpoint. Each VT owns at least:

```text
VT number
foreground process/session
login state
terminal input queue
terminal output buffer
terminal attributes
scrollback
keyboard state
display backend
seat assignment
owner/session identity
```

Input flow:

```text
USB keyboard
    |
input.device
    |
secure VT manager
    +---- VT1 -> login/shell
    +---- VT2 -> login/shell
    +---- VT3 -> login/shell
    +---- ...
    +---- VT7 -> Workbench/compositor
```

### 38.25.2 Independent login sessions

Each text VT can host an independent authenticated user session. For example:

```text
VT1  alice logged in
VT2  root administration shell
VT3  bob logged in
VT7  alice graphical Workbench
```

Switching VTs must not terminate or suspend unrelated sessions. Processes continue running normally while their VT is inactive.

Each login creates the same normal CrugOS session identity used elsewhere:

```text
SessionId
UID
primary GID
supplementary groups
capability set
environment
seat/terminal association
```

No VT gains privilege merely because it is local.

### 38.25.3 Workbench as a VT

The graphical Workbench is attached to a virtual terminal rather than being globally hard-wired to the display.

Default:

```text
VT7 -> Workbench
```

This allows immediate recovery from a broken desktop or compositor:

```text
Ctrl+Alt+F2
    -> text recovery console

Ctrl+Alt+F7
    -> return to the existing Workbench session
```

The Workbench session must retain its windows, Screens, applications and state while inactive.

CrugOS may also run multiple graphical VTs:

```text
VT7 -> alice Workbench
VT8 -> bob Workbench
VT9 -> test/development graphical session
```

This provides a natural implementation for fast graphical user switching.

### 38.25.4 Secure key handling

`Ctrl+Alt+Fn` is handled by the trusted input/VT layer before ordinary application input delivery.

Normal applications must not be able to intercept, suppress, or fake the VT-switch sequence.

Input path:

```text
key event
 -> input.device
 -> secure key check
     -> Ctrl+Alt+Fn: switch VT
     -> otherwise: deliver to active VT/session
```

This gives the user a trusted escape path even when a graphical application is hung or malicious.

### 38.25.5 Text-console functionality

Text VTs support at least:

```text
UTF-8
ANSI/VT-style control sequences
cursor movement
text attributes
scrollback
terminal-size reporting
control characters/signals
login and shell operation
```

The terminal implementation remains independent from Workbench.

### 38.25.6 Display and GPU fallback

Before acceleration:

```text
text VT -> framebuffer terminal renderer
Workbench VT -> software compositor -> firmware framebuffer
```

After acceleration:

```text
text VT -> framebuffer or GPU-backed text surface
Workbench VT -> Vulkan compositor -> GPU scanout
```

VT switching must continue working if the accelerated GPU driver crashes. A basic framebuffer/emergency-console path should remain available whenever the platform permits it.

### 38.25.7 Panic/emergency console

Kernel panic output is independent of the active VT. On panic:

```text
kernel
 -> force emergency console
 -> display panic/register/trace information
```

The emergency console has priority over preserving graphical display state.

### 38.25.8 Console device model

Native console objects should map cleanly to POSIX compatibility names.

Native view may expose:

```text
Console:
Console:1
Console:2
...
```

POSIX/Linux compatibility exposes the same underlying objects as:

```text
/dev/console
/dev/tty
/dev/tty1
/dev/tty2
...
```

Useful compatible administration commands include:

```text
tty
chvt
fgconsole
openvt
loginctl
```

### 38.25.9 Boot ordering

Text-console recovery must become available before the graphical environment is fully initialized:

```text
kernel
 -> scheduler
 -> input
 -> framebuffer console
 -> VT manager
 -> VT1 login available
 -> Workbench starts on VT7
 -> optional GPU acceleration activates later
```

This means Workbench, NVIDIA initialization, audio, network, containers, virtualization and HPC services do not block access to a local console.

### 38.25.10 Acceptance criteria

Before CrugOS 1.0:

- at least seven VTs exist by default;
- `Ctrl+Alt+F1` through `Ctrl+Alt+F7` switch reliably;
- VT switching works under CPU, storage, network and audio load;
- switching does not kill unrelated sessions;
- different authenticated users may occupy different text VTs;
- Workbench runs on VT7 by default;
- switching away from and back to Workbench preserves its state;
- a crashed/hung Workbench does not prevent switching to a text VT;
- ordinary applications cannot capture the secure VT-switch key sequence;
- console ownership and multiuser permissions are enforced;
- panic output works regardless of the active VT;
- console/TTY switching regression tests run in CI.

# 39. Crug Compiler and SDK

Targets:

```text
x86_64-crugos
x86_64-crugos-kernel

aarch64-crugos
aarch64-crugos-kernel

riscv64-crugos
riscv64-crugos-kernel
```

Roadmap:

1. Linux cross compiler;
2. CrugOS sysroot;
3. native linker;
4. native `crugc`;
5. compile native apps;
6. debugger;
7. Workbench IDE;
8. `crugc` compiles itself;
9. CrugOS builds its own kernel and system;
10. CrugOS produces its own install image.

---

# 40. Virtualization — Core Design

Virtualization is a first-class optional kernel facility.

Do not build a giant hypervisor entirely inside the kernel.

Split:

```text
                 VM application / Linux guest
                           |
                     vmm.service
                  device emulation
                  VM lifecycle
                  snapshots
                           |
                       vm.device
                  small kernel VMM API
                           |
        +------------------+------------------+
        |                  |                  |
       VMX/SVM             EL2             RISC-V H
        |                  |                  |
      x86-64             AArch64              RV64
```

Kernel responsibilities:

- create/destroy VM;
- create/destroy vCPU;
- map guest memory;
- run vCPU;
- receive VM exits;
- inject interrupts;
- protect guest memory;
- second-stage translation;
- expose virtual interrupt support;
- IOMMU isolation.

User-space VMM responsibilities:

- firmware;
- device model;
- VM configuration;
- VirtIO devices;
- disk image;
- network backend;
- console;
- snapshots;
- policy.

---

# 41. x86-64 Virtualization

Support both major hardware families.

## Intel

```text
VT-x / VMX
EPT
VPID later
APIC virtualization later
```

## AMD

```text
AMD-V / SVM
NPT
ASID
AVIC later
```

Do not require one vendor.

Common x86 virtualization API hides VMX/SVM differences.

Milestones:

### VIRT-X86-0

- detect virtualization capability;
- enable VMX/SVM;
- create VM;
- allocate guest RAM.

### VIRT-X86-1

- create one vCPU;
- enter guest;
- trap HLT/CPUID;
- return to host.

### VIRT-X86-2

- second-stage memory;
- guest page faults;
- MMIO exits;
- interrupt injection.

### VIRT-X86-3

- SMP guest;
- virtual APIC;
- timers;
- virtual serial console.

### VIRT-X86-4

- VirtIO block;
- VirtIO net;
- VirtIO console;
- VirtIO RNG.

### VIRT-X86-5

- boot Linux kernel;
- boot normal Linux distribution;
- stable multi-vCPU VM.

Acceptance:

```text
Linux guest boots to shell
4+ vCPUs
VirtIO disk/net
guest survives host stress
VM cannot access host memory outside assigned regions
```

---

# 42. AArch64 Virtualization

Use the Arm virtualization architecture.

```text
host kernel: EL2-aware design
guest kernel: EL1
guest apps: EL0
stage-2 translation
GIC virtualization
```

Preferred design:

- CrugOS retains normal host kernel semantics;
- hypervisor backend manages EL2;
- PSCI emulation for guests;
- virtual GIC;
- VirtIO devices.

Milestones mirror x86.

Acceptance:

```text
Linux AArch64 guest boots in QEMU and later physical hardware
multiple vCPUs
VirtIO block/net
guest isolation
```

---

# 43. RISC-V Virtualization

Require the RISC-V H extension for hardware virtualization.

Concepts:

```text
HS-mode host
VS-mode guest
two-stage address translation
virtualized CSRs
virtual interrupts
```

Initial target:

```text
QEMU RV64 with H extension
```

Milestones:

1. detect H extension;
2. enter guest VS-mode;
3. trap virtual instructions;
4. guest second-stage mapping;
5. interrupts/timers;
6. SMP guest;
7. VirtIO devices;
8. Linux RV64 guest.

Do not emulate virtualization in software for production if H is absent.

---

# 44. Common VM Object API

Possible user-space interface:

```crug
let vm = Vm::create(VmConfig {
    memory: 4 * GiB,
    cpus: 4,
})?;

let disk = vm.add_virtio_block("Linux.img")?;
let net  = vm.add_virtio_net(Network::Nat)?;

vm.boot(kernel_or_firmware)?;
```

Kernel object model:

```text
Vm
Vcpu
GuestMemory
IrqRoute
VmExit
```

Example VM exits:

```text
HLT/WFI
MMIO
port I/O
CPUID/system-register
page fault
interrupt window
shutdown
debug trap
```

---

# 45. Virtual Devices

VirtIO should be the standard guest hardware model.

Initial:

```text
virtio-blk
virtio-net
virtio-console
virtio-rng
```

Later:

```text
virtio-gpu
virtio-snd
virtio-fs
virtio-balloon
vsock
```

This same VirtIO code can often be reused for:

- CrugOS as a guest;
- Linux guests;
- development under QEMU.

---

# 46. VM Networking

Provide:

```text
NAT
host-only
bridged
isolated
```

Model:

```text
guest virtio-net
 -> virtual switch
 -> host network stack / physical NIC
```

Port forwarding:

```text
host:8080 -> guest:80
```

A VM must not receive raw host NIC access by default.

Later support:

```text
PCI passthrough
SR-IOV VF
```

only with IOMMU protection.

---

# 47. VM Storage

First:

```text
raw sparse disk image
```

Later define a CrugOS VM image format supporting:

- sparse extents;
- snapshots;
- backing image;
- checksums;
- optional encryption;
- trim/discard.

Do not block first Linux boot on designing an elaborate disk format.

---

# 48. PCI Passthrough

Later milestone.

Requirements:

- IOMMU;
- dedicated IOMMU group/domain;
- interrupt remapping;
- safe reset support;
- host driver detachment;
- explicit administrator permission.

Targets:

```text
NIC
NVMe
selected GPU
```

GPU passthrough is not required for first virtualization release.

---

# 49. Native CrugOS Containers

CrugOS containers are lighter than VMs.

A native container is:

```text
process tree
+ private filesystem namespace
+ private handle/capability namespace
+ optional network namespace
+ resource limits
+ identity/security domain
```

It uses the host kernel directly.

No hardware virtualization is involved.

Core objects:

```text
Container
ContainerNamespace
ContainerLimits
ContainerImage
ContainerNetwork
```

---

# 50. Native Container Isolation

Required primitives:

## Process isolation

Container sees its own logical process namespace.

## Filesystem namespace

Container receives its own root and mount/volume view.

## Handle isolation

Kernel handles cannot cross container security boundaries without explicit transfer.

## Network namespace

Optional independent:

- interfaces;
- addresses;
- routing;
- firewall;
- ports.

## Resource limits

Initial:

```text
memory maximum
CPU affinity/share
process/task maximum
open-handle maximum
network limits later
```

Do not clone Linux cgroups internally unless compatibility requires it.

Create a simpler CrugOS resource-domain API.

---

# 51. Native Container Image Format

Use OCI-compatible image concepts where practical.

Support:

- OCI image manifests;
- content-addressed blobs;
- layered root filesystems;
- architecture metadata;
- signatures;
- config/environment;
- entrypoint.

However, a native CrugOS container must contain **CrugOS executables**, not Linux executables.

Example:

```text
crugos/redis-like-service:1.0
architecture: x86_64
os: crugos
```

The image store lives outside the kernel.

---

# 52. Docker Compatibility — Important Rule

A normal Docker image typically contains Linux userspace binaries and expects Linux syscalls, `/proc`, namespaces, cgroups, Linux networking semantics and often overlayfs.

Therefore:

> Do not make the CrugOS kernel imitate all of Linux merely to run Docker images.

Support Docker in two layers.

---

# 53. Docker Mode A — Native OCI/CrugOS Containers

Provide a Docker-like CLI:

```text
crug container run ...
```

and optionally a Docker-compatible REST/API subset.

Examples:

```bash
container run crugos/nginx-equivalent
container ps
container stop <id>
container logs <id>
```

This uses native CrugOS process isolation.

Advantages:

- tiny overhead;
- instant startup;
- no Linux kernel;
- direct CrugOS networking;
- direct CrugOS filesystem;
- ideal for native server workloads.

---

# 54. Docker Mode B — Real Linux Containers Through a MicroVM

For actual Docker images:

```bash
docker run nginx
docker run postgres
docker compose up
```

run a managed Linux container host VM.

Architecture:

```text
Docker CLI
   |
docker.service on CrugOS
   |
managed Linux microVM
   |
dockerd/containerd
   |
Linux containers
```

This is conceptually similar to Docker-on-non-Linux systems: Linux containers execute on a Linux kernel, but management is integrated into the host OS.

CrugOS can make this mostly transparent.

---

# 55. Linux Container VM

Create a small dedicated Linux VM:

```text
DockerVM
├── minimal Linux kernel
├── containerd/dockerd
├── overlayfs
├── namespaces
├── cgroups
└── OCI runtime
```

Host provides:

```text
VirtIO block
VirtIO net
vsock
VirtIO FS later
```

The Linux VM starts only when needed.

Long-term goal:

```text
warm DockerVM resume < 100 ms class
```

not including container/application startup.

---

# 56. Docker CLI Integration

Expose:

```bash
docker ps
docker run
docker build
docker compose
```

by forwarding Docker API traffic to the managed Linux VM.

The user should not need to manually administer that VM.

Workbench may show Docker workloads as native objects:

```text
Containers:
├── nginx
├── postgres
└── redis
```

Possible states:

```text
running
stopped
starting
failed
```

---

# 57. Docker Filesystem Sharing

Do not expose arbitrary host filesystem access to the Linux VM.

Use explicit shares:

```text
Projects:
 -> virtio-fs/shared transport
 -> /mnt/projects inside Linux VM
```

Volume command example:

```bash
docker run -v Projects:website:/work nginx
```

The Docker integration layer translates CrugOS paths into guest shares.

Permissions remain explicit.

---

# 58. Docker Networking

Default:

```text
container
 -> Linux VM virtual bridge
 -> virtio-net
 -> CrugOS virtual switch
 -> NAT
 -> physical network
```

Port mapping:

```bash
docker run -p 8080:80 nginx
```

becomes:

```text
host port 8080
 -> DockerVM
 -> Linux container port 80
```

Later support bridged mode.

---

# 59. Docker Build Support

First implementation:

```text
Docker build runs inside DockerVM
```

Host CLI streams:

- Dockerfile;
- build context;
- secrets through protected channels.

Build cache remains inside a dedicated Docker data volume.

Later CrugOS can implement direct OCI image tooling, but that is not required for real Docker compatibility.

---

# 60. Virtualization Security

VM security rules:

- guest RAM inaccessible outside VM except explicit mappings;
- IOMMU for passthrough;
- no implicit host file access;
- virtual devices validate descriptors;
- bounded guest-controlled queues;
- guest input treated as hostile;
- firmware images authenticated where appropriate;
- VM management requires capability.

Fuzz:

- VirtIO descriptors;
- MMIO;
- port I/O;
- malformed disk requests;
- virtual network packets;
- VM-exit decoding.

---

# 61. Virtualization Performance Goals

Initial goals:

```text
CPU overhead:             low single-digit % for CPU-bound guests
VirtIO network:           near host software-network limit
VirtIO block:             near host block backend limit
VM entry/exit:            benchmarked per architecture
vCPU scheduling:          CPU-affinity aware
```

Later:

- huge guest pages;
- interrupt virtualization;
- posted interrupts;
- nested-page optimizations;
- vhost-like user/kernel fast paths;
- SR-IOV;
- PCI passthrough.

Nested virtualization is explicitly not a version-1 requirement.

---

# 62. Native HPC Architecture

HPC is first-class.

```text
Crug app
 -> hpc.library
 -> fabric.library
 -> shared-memory / TCP / RDMA
 -> fabric.device
 -> NIC/DMA/IOMMU
```

Core concepts:

```text
Cluster
Node
Rank
Group
RemotePort<T>
RegisteredBuffer<T>
RemoteBuffer<T>
Job
```

---

# 63. HPC Homogeneous CPU Rule

A compute job uses exactly one compatible CPU-family ABI.

Examples:

```text
CRUG-HPC-X86_64-v1
CRUG-HPC-AARCH64-v1
CRUG-HPC-RV64-v1
```

Allowed:

```text
x86_64 + x86_64
AArch64 + AArch64
RV64 + RV64
```

Forbidden in one compute group:

```text
x86_64 + AArch64
x86_64 + RV64
AArch64 + RV64
```

Nodes may share the management network but not the same HPC job if ABI differs.

Additional capability classes:

```text
x86-64-v3
AVX2
AVX-512
SVE/SVE2
RVV
```

Scheduler only assigns compatible nodes.

---

# 64. HPC API

Example:

```crug
let world = hpc.world();

let rank = world.rank();
let size = world.size();

let local = dataset.partition(rank, size);
let result = compute(local);

let final = world.gather(result, root: 0);
```

Collectives:

```text
barrier
broadcast
reduce
allreduce
gather
allgather
scatter
alltoall
```

RMA:

```text
put
get
remote atomic
```

No transparent distributed shared memory in version 1.

---

# 65. HPC Transports

Order:

## H0

TCP transport.

## H1

Same-node shared-memory fast path.

## H2

Registered buffers and zero-copy.

## H3

RDMA/RoCE/InfiniBand using one selected NIC family.

## H4

Topology-aware collectives.

## H5

GPU-aware transfers.

Data path must bypass the cluster manager after setup.

---

# 66. MPI Compatibility

Add a user-space MPI compatibility layer later.

Initial subset:

```text
MPI_Init
MPI_Finalize
MPI_Comm_rank
MPI_Comm_size
MPI_Send
MPI_Recv
MPI_Isend
MPI_Irecv
MPI_Wait
MPI_Barrier
MPI_Bcast
MPI_Reduce
MPI_Allreduce
MPI_Gather
MPI_Scatter
```

Native Crug HPC API remains preferred.

---

# 67. Boot Critical Path

Do not block Workbench on optional subsystem startup.

Critical:

```text
CPU
memory
interrupts
scheduler
NVMe
decrypt
EXT4
input
framebuffer
init
Workbench
```

Parallel background:

```text
network
USB
audio
GPU
virtualization service
container service
HPC service
package update service
```

Target boot trace:

```text
CPU/MM              0–20 ms
SMP                10–40 ms
NVMe               20–60 ms
crypto + EXT4      40–100 ms
init               70–130 ms
Workbench         100–250 ms
```

---

# 68. AArch64 Port

Do not begin until x86 generic architecture boundaries are stable.

AArch64 backend must implement the same common interfaces.

Primary components:

```text
EL1 kernel
4 KiB pages
stage-1 translation
GICv3
Generic Timer
PSCI
FDT
PCIe
SMMUv3 later
EL2 virtualization support
```

Port order:

1. serial boot;
2. exceptions;
3. page tables;
4. allocator;
5. timer;
6. GIC;
7. SMP/PSCI;
8. scheduler;
9. userspace/SVC;
10. VirtIO block;
11. EXT4/encryption;
12. VirtIO net;
13. USB/PCI where available;
14. framebuffer/VirtIO GPU;
15. Workbench;
16. virtualization/EL2;
17. containers;
18. HPC.

Acceptance:

```text
same user-space binaries rebuilt for AArch64
same Workbench behavior
same system tests
same container API
same HPC API
Linux AArch64 VM boots
```

---

# 69. RISC-V 64 Port

Baseline:

```text
RV64GC
S-mode
OpenSBI
Sv39
SBI HSM
FDT
PLIC first
AIA later
H extension for virtualization
```

Port order:

1. serial boot;
2. trap vector;
3. Sv39;
4. allocator;
5. SBI timer;
6. PLIC;
7. SMP/HSM;
8. scheduler;
9. userspace/ECALL;
10. VirtIO block;
11. EXT4/encryption;
12. VirtIO net;
13. PCIe;
14. VirtIO GPU;
15. Workbench;
16. H-extension virtualization;
17. containers;
18. HPC.

Acceptance mirrors AArch64.

---

# 70. Testing Strategy

## Unit tests

Run host-side where possible for:

- containers;
- allocators;
- parsers;
- EXT4 structures;
- network protocols;
- crypto wrappers;
- package manifests;
- HPC algorithms.

## QEMU integration tests

Every commit should be able to test:

```text
boot
SMP
memory
filesystem
network
USB where possible
userspace
Workbench smoke test
VM boot
container smoke test
HPC two-node virtual cluster
```

## Fuzzing

Mandatory fuzz targets:

```text
ELF parser
EXT4 metadata
GPT
network packets
USB descriptors
package manifests
container images
VirtIO descriptors
VM exits/MMIO
HPC control protocol
```

## Fault injection

Test:

- allocation failures;
- NVMe timeout;
- corrupted EXT4;
- bad journal;
- unplug USB;
- NIC reset;
- GPU reset;
- VM guest attack;
- cluster node disappearance.

---

# 71. Security Milestones

## SEC-0

- W^X;
- NX;
- user/kernel separation;
- handle validation.

## SEC-1

- encrypted root;
- secure random;
- key zeroization.

## SEC-2

- app signatures;
- capability model;
- developer mode.

## SEC-3

- IOMMU;
- DMA isolation;
- hardened GPU/fabric paths.

## SEC-4

- verified boot;
- TPM integration;
- measured boot.

## SEC-5

- VM/container hardening;
- sandboxed device emulation;
- attack/fuzz campaign.

---

# 72. Size Budgets

Initial targets:

```text
arch + core            200 KiB
MM + scheduler         150 KiB
IPC/syscalls            80 KiB
VFS/EXT4               180 KiB
network                180 KiB
USB core/xHCI          150 KiB
crypto                  70 KiB
misc                    70 KiB
--------------------------------
base target          ~1.0 MiB class
```

These are design budgets, not measured results.

Optional components should remain separately loadable where practical:

```text
HDA
NVIDIA KMD
RDMA driver
hypervisor backend extensions
debug tools
```

With virtualization enabled, a realistic stripped kernel may be closer to:

```text
~1.0–1.5 MiB target
<2 MiB hard budget
```

The user-space system may be much larger without violating the kernel-size goal.

---

# 73. Full Implementation Roadmap

## Phase 0 — Toolchain and build system

Deliver:

- `crugc --kernel`;
- linker script;
- UEFI loader;
- reproducible build;
- QEMU launch;
- symbol extraction;
- size report;
- boot-time report.

Acceptance:

```text
make run
```

boots a Crug kernel and prints `hello`.

---

## Phase 1 — x86-64 core

Deliver:

- exceptions;
- physical allocator;
- page tables;
- APIC;
- timer;
- SMP;
- scheduler;
- MsgPort.

Acceptance:

- 8-vCPU QEMU stress;
- task IPC stress;
- no leaks in repeated task creation.

---

## Phase 2 — protected userspace

Deliver:

- ring 3;
- syscall gateway;
- handles;
- ELF;
- `spawn`;
- init;
- shell.

Acceptance:

```text
/init
/bin/sh
```

run in isolated address spaces.

---

## Phase 3 — persistent storage

Deliver:

- PCI;
- NVMe;
- GPT;
- raid.device (RAID1/RAID10);
- redundant ESP handling;
- crypt.device;
- EXT4 read;
- EXT4 write;
- journal.

Acceptance:

- encrypted root boot;
- degraded RAID boot after member removal;
- online RAID rebuild;
- redundant ESP boot after primary disk loss;
- power-loss filesystem/RAID test suite;
- recovery key works.

---

## Phase 4 — networking

Deliver:

- Ethernet;
- IPv4;
- ICMP;
- UDP;
- DHCP;
- TCP;
- DNS;
- sockets.

Acceptance:

```text
ping
TCP client/server
HTTP test
```

---

## Phase 5 — USB/audio/input

Deliver:

- xHCI;
- keyboard;
- mouse;
- UAC2;
- audio mixer.

Acceptance:

- hotplug keyboard/mouse;
- 48 kHz audio playback;
- recording;
- no dropout under normal desktop load.

---

## Phase 6 — Workbench

Deliver:

- framebuffer graphics;
- Intuition-like event system;
- windows;
- menus;
- Screens;
- drawers;
- icons;
- app launching.

Acceptance:

- fully usable software-rendered desktop;
- no GPU dependency.

---

## Phase 7 — package/application security

Deliver:

- `.app`;
- `.crpkg`;
- signatures;
- permissions;
- atomic install/update;
- rollback.

Acceptance:

- install/remove/update through GUI and CLI.

---

## Phase 8 — GPU acceleration

Deliver:

- VirtIO GPU;
- Vulkan path;
- accelerated compositor;
- NVIDIA experimental backend later.

Acceptance:

- accelerated triangle;
- accelerated Workbench;
- fallback to software remains functional.

---

## Phase 9 — virtualization x86-64

Deliver:

- VMX;
- SVM;
- VM/vCPU kernel objects;
- EPT/NPT;
- interrupt injection;
- VirtIO block/net/console/rng;
- user-space VMM.

Acceptance:

- Linux x86-64 boots with 4 vCPUs;
- networking and disk work;
- guest isolation test passes.

---

## Phase 10 — containers and Docker

Deliver:

- native container security domains;
- OCI-compatible image store;
- native CrugOS container runtime;
- Linux Docker microVM;
- Docker API bridge;
- volume sharing;
- port forwarding.

Acceptance:

```text
container run <native-crugos-image>
docker run nginx
docker run -p 8080:80 nginx
docker compose up
```

work from CrugOS.

---

## Phase 11 — HPC

Deliver:

- cluster identity;
- `clusterd`;
- ranks;
- RemotePort;
- TCP transport;
- shared-memory transport;
- collectives;
- MPI subset;
- registered memory;
- RDMA later.

Acceptance:

- 2/4/8-node virtual cluster;
- distributed matrix multiply;
- allreduce benchmark;
- homogeneous CPU ABI enforcement.

---

## Phase 12 — self-hosting

Deliver:

- native linker;
- native `crugc`;
- debugger;
- SDK;
- CrugIDE;
- self-build.

Acceptance:

```text
CrugOS builds crugc
CrugOS builds kernel
CrugOS builds Workbench
CrugOS builds install image
```

without Linux.

---

## Phase 13 — AArch64 (dropped)

Removed from the plan on 2026-10-30 at the project owner's request: CrugOS ships
one architecture backend, x86_64. Nothing in this document obliges an AArch64
port any more; the AArch64 material elsewhere in the specification is retained
as design history only.

---

## Phase 14 — RISC-V 64 (dropped)

Removed from the plan on 2026-10-30 at the project owner's request, for the same
reason as Phase 13. RISC-V material elsewhere in the specification is retained
as design history only.

---

## Phase 15 — production hardening

Deliver:

- full fuzzing campaign;
- verified boot;
- IOMMU by default where possible;
- crash recovery;
- installer;
- update/rollback system;
- documentation;
- hardware compatibility matrix;
- stable ABI versioning.

---

# 74. Release Milestones

## M0 — Boot

```text
UEFI -> Crug -> framebuffer hello
```

## M1 — Kernel

```text
MM + interrupts + SMP + scheduler + MsgPort
```

## M2 — Userspace

```text
ELF + processes + shell
```

## M3 — Storage

```text
NVMe + encryption + EXT4
```

## M4 — Network

```text
TCP/IP usable
```

## M5 — Desktop

```text
software Workbench
```

## M6 — Audio

```text
USB audio + mixer
```

## M7 — Applications

```text
signed bundles + package manager
```

## M8 — GPU

```text
accelerated Workbench
```

## M9 — Virtualization

```text
Linux VM
```

## M10 — Containers

```text
native container + docker run nginx
```

## M11 — HPC

```text
multi-node native cluster
```

## M12 — Self-hosted

```text
CrugOS builds itself
```

## M13 — AArch64

```text
feature-parity architecture
```

## M14 — RV64

```text
feature-parity architecture
```

## M15 — 1.0

```text
secure
stable
self-hosting
multi-architecture
virtualized
container-capable
HPC-capable
```

---

# 75. Architectural Invariants

These rules should be reviewed in every major design change.

1. Generic code never directly depends on one CPU architecture.
2. UID/GID ownership, groups, Linux-style permission bits and ACLs are native VFS/security semantics.
3. Kernel mechanisms stay small; policy belongs in user space.
4. No hidden allocation in kernel hot paths.
5. No raw kernel pointers in userspace.
6. No application receives unrestricted capabilities by default.
7. Root storage is encrypted by default.
8. Workbench must boot without GPU acceleration.
9. Optional GPU/audio/HPC/virtualization components must not block desktop startup.
10. Native containers do not require Linux semantics.
11. Real Linux Docker containers run on a Linux kernel, normally through a managed microVM.
12. HPC jobs are homogeneous by cluster ABI/CPU family.
13. Remote memory is explicit; no transparent distributed shared memory.
14. IOMMU is used whenever exposing DMA-capable hardware to less-trusted contexts.
15. Every parser that consumes untrusted binary data must have fuzz tests.
16. Every architecture must pass the same generic conformance test suite.
17. `crugc` self-hosting is a release-quality objective, not an optional demo.
18. The system must remain useful without network connectivity or cloud services.

---


# 76. Definition of “Done” for CrugOS 1.0

CrugOS 1.0 is achieved when all of the following are true:

- x86-64 is stable on QEMU and selected physical hardware;
- AArch64 and RV64 boot and pass the generic core test suite;
- multicore scheduling is stable;
- userspace isolation is enforced;
- native UID/GID users, groups, Linux-style mode permissions and ACLs are enforced;
- multiple concurrent shell and Workbench user sessions are supported;
- Linux-style virtual consoles support `Ctrl+Alt+F1` through `Ctrl+Alt+F7`, with Workbench on VT7 by default;
- encrypted EXT4 root is default;
- NVMe works reliably;
- RAID1/RAID10, degraded boot and online rebuild work reliably;
- redundant boot ESPs survive loss of a boot disk;
- TCP/IP works;
- xHCI keyboard/mouse works;
- UAC2 playback/recording works;
- Workbench is fully usable without GPU acceleration;
- at least one accelerated GPU path works;
- signed app bundles and package management work;
- native containers work;
- `docker run` works through the managed Linux container VM;
- Linux guests run under the native hypervisor;
- a native homogeneous HPC cluster works across multiple machines;
- `crugc` runs natively;
- CrugOS can rebuild its own kernel and base system;
- automated tests, fuzzing and recovery tests pass;
- release kernel stays under the defined size budget;
- boot-to-Workbench remains within the performance budget on reference hardware.

---

# 77. Recommended First Development Sequence

Do not attempt all features in parallel.

The recommended actual order is:

```text
1.  crugc kernel target
2.  x86-64 UEFI boot
3.  serial/framebuffer debugging
4.  physical memory
5.  virtual memory
6.  interrupts/APIC/timer
7.  SMP
8.  scheduler
9.  MsgPort
10. protected userspace
11. ELF/spawn
12. PCI
13. NVMe
14. GPT
15. RAID1/RAID10 + degraded boot/rebuild
16. encryption
17. EXT4 read
18. shell/init
19. EXT4 write/journal
20. network
21. xHCI + input
22. virtual consoles/TTY + Ctrl-Alt-F1..F7 switching
23. Workbench software renderer
24. UAC2 audio
25. app bundles/security
26. VirtIO GPU
27. x86 virtualization
28. Linux guest
29. native containers
30. Docker microVM integration
31. HPC TCP/shared-memory
32. native compiler/self-hosting
33. NVIDIA acceleration
34. RDMA HPC
35. AArch64 port
36. AArch64 virtualization
37. RV64 port
38. RV64 virtualization
39. hardening and 1.0
```

This ordering deliberately reaches a usable desktop before tackling the most complex optional hardware stacks.

---

# 78. Final Product Vision

CrugOS should feel like a machine designed as one coherent system rather than a collection of historical Unix layers.

A user powers on the machine and receives a usable Workbench-style desktop almost immediately.

Applications are self-contained signed bundles.

Native programs communicate using lightweight message ports.

Storage is encrypted by default.

USB audio, networking and GPU acceleration are first-class.

Developers compile Crug applications directly on the machine.

A developer can run:

```bash
docker run nginx
```

without turning CrugOS itself into Linux.

They can also create a virtual machine:

```bash
vm run linux.vm
```

or launch a native cluster workload:

```bash
hpc run --nodes 8 --ranks 128 simulation.app
```

The same operating-system architecture scales from:

```text
one tiny RISC-V computer
```

to:

```text
a modern x86 workstation
```

to:

```text
a homogeneous multi-node HPC cluster
```

while keeping a small, understandable kernel and a distinctive Amiga-inspired user experience.
