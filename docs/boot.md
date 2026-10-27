# Boot flow and the boot contract

## Chain

```
OVMF (UEFI firmware)
  -> EFI/BOOT/BOOTX64.EFI          CrugOS loader
       - serial + ConOut console
       - read \crugos\kernel.elf from the boot device
       - load PT_LOAD segments at their physical addresses
       - program GOP to 1024x768
       - find the ACPI RSDP in the configuration table
       - normalize the UEFI memory map
       - gather entropy
       - ExitBootServices
  -> kernel.elf _start             CrugOS kernel
       - disable interrupts, set a boot stack, zero .bss
       - call kernel_main(boot_info *)
       - physical allocator, page tables + W^X
       - GDT/TSS, %gs per-CPU data, IDT and exceptions
       - LAPIC + TSC/LAPIC timer
       - kernel heap, virtio-blk + GPT + writable ext4, JBD2 replay, storage self-test
       - scheduler, handles and MsgPort IPC
       - syscall dispatcher behind int 0x80
       - self-tests, boot report, deterministic exit
```

## BootInfo

`include/crug/bootinfo.h` is the single contract between loader and kernel.
The structure is versioned (`magic = 0x43524731`, `version = 1`) and the kernel
refuses to boot if either does not match or if required fields are invalid.
Phases 1 through 3B-2 did not change the layout.

| Field | Meaning |
| --- | --- |
| `kernel_base`, `kernel_size` | physical extent of the loaded kernel image |
| `memory_map`, `memory_map_count` | normalized `memory_region[]` |
| `fb` | GOP framebuffer base/size/resolution/stride/pixel format |
| `command_line`, `command_line_len` | optional, currently NULL/0 |
| `acpi_rsdp` | ACPI 2.0 (or 1.0) RSDP physical address |
| `fdt` | flattened device tree, 0 on x86_64 |
| `initrd_base`, `initrd_size` | optional initial ramdisk, currently 0 |
| `boot_cpu_id` | APIC id of the boot CPU (0 in phase 1) |
| `entropy_seed` | 64 bytes from EFI RNG, RDRAND or timing jitter |
| `boot_tsc` | TSC value captured by the loader before handoff |

Memory regions are classified as `USABLE`, `RESERVED`, `ACPI_RECLAIM`,
`ACPI_NVS`, `UNUSABLE`, `BOOTLOADER`, `FRAMEBUFFER` or `KERNEL`.

### Loader ownership invariant

Everything the kernel must not allocate is marked before
`ExitBootServices`: the kernel image (`KERNEL`), the framebuffer
(`FRAMEBUFFER`), the `boot_info` structure and the normalized map buffer
itself (`BOOTLOADER`). The loader then scans the final map and refuses to hand
off if any `USABLE` region overlaps those extents. Allocation sizes are page
rounded, and the map buffer is sized with a 32-descriptor margin so the
post-snapshot allocations cannot push it over capacity.

## ABI at handoff

- CPU: long mode, paging enabled by firmware, interrupts disabled by `_start`.
- Calling convention: System V AMD64 (`kernel_main(boot_info *)` in `rdi`).
- Stack: the kernel switches to its own 32 KiB boot stack in `.bss` before
  touching C code. A 4 KiB unmapped guard page sits directly below it.
- Firmware services are gone: the kernel may only use memory and devices
  described by `BootInfo`.
- FPU/SSE/AVX state is not inherited; the kernel enables XSAVE itself.

## Kernel initialization

`kernel/core/main.c` runs, in order:

1. `serial_init`, `bootinfo_validate`, `fb_init`, `console_init`, banner and
   `CRUGOS: hello`.
2. `pmm_init`: reserve everything, then free frames fully inside `USABLE`
   regions. The bitmap lives right after the kernel image.
3. `cpu_early_init`: CPUID feature scan, `EFER.NXE`, GDT/TSS, `%gs` per-CPU
   base, `CR0.WP`.
4. `vm_init`: build the identity map, enforce W^X, load `CR3`, enable
   `CR4.PGE`, `SMEP`/`SMAP` when available.
5. `cpu_late_init`: IDT (256 gates, double-fault IST stack), LAPIC enable,
   8259 PIC mask. `CRUGOS: cpu ready`.
6. `timer_init`: TSC frequency, TSC-deadline or LAPIC periodic. Interrupts
   stay disabled until the execution model exists.
7. `heap_init`: slab caches and large-allocation headers. `CRUGOS: heap ready`.
8. `storage_init`: `block_init`, legacy virtio-blk, GPT, the `crugroot`
   partition and a read-only ext4 mount used for the boot file check. A
   journaled filesystem is opened through JBD2 and replayed at mount time if
   necessary. `CRUGOS: block ready`, `CRUGOS: gpt ready`,
   `CRUGOS: ext4 ready`, `CRUGOS: journal ready`, `CRUGOS: storage ready`.
9. `sched_init`: per-class run queues, boot thread id 0 and the idle thread.
   `CRUGOS: sched ready`.
10. `process_init`: the kernel process and its handle table.
    `CRUGOS: syscall ready`.
11. `sti`, then `selftest_run`: allocator, mappings, W^X audit, `int3`, timer,
    heap, block/GPT/ext4/storage (including the read-write round trip and a
    journal replay test), handles, scheduler, message ports and syscalls.
12. `report_boot`, `CRUGOS: phase 3b2 boot ok`, `arch_exit_qemu(0x10)`.

Storage is still brought up with interrupts disabled: the virtio driver polls
its used ring between `heap_init` and `sched_init`.

## Page tables

The kernel builds its own four-level tables: one PML4, one PDPT and four page
directories covering the low 4 GiB. Everything is identity mapped with 2 MiB
huge pages except:

- the first 2 MiB, split to 4 KiB so `.text`, `.rodata`, `.data` and `.bss` get
  exact permissions;
- the boot stack guard page, which is left not-present;
- the framebuffer and LAPIC ranges, mapped with `PCD|PWT` (uncached).

Permissions: `.text` present/read/execute, `.rodata` present/read-only NX,
`.data`/`.bss` present/write NX. No page is writable and executable.
`vm_audit_wx()` checks the section symbols and the guard, and its result is
printed as `wx     : audit ok`; a failure panics.

To guarantee `NX` is honoured, `EFER.NXE` is set before the first `CR3` load.

## Exceptions and timer

The IDT covers all 256 vectors. Vectors 0-31 use assembly stubs that normalize
the error code, save the general-purpose registers and call `trap_dispatch`.
`#BP` is handled and resumed (used by the self-test); `#PF` prints `CR2` and a
decoded error code before panicking; `#DF` and `NMI` run on the IST stack. A
panic prints registers, scans the stack and exits QEMU with `0x11`.

The LAPIC timer runs on vector 32. TSC frequency comes from CPUID leaf 0x15
(crystal clock ratio), leaf 0x16 (base MHz) or a PIT channel-2 calibration; the
source is printed. TSC-deadline is used when CPUID advertises it, otherwise the
LAPIC timer runs in periodic mode at 100 Hz. `timer_monotonic_ns()` converts
TSC ticks with quotient/remainder arithmetic so it cannot overflow at 64 bits.

## Execution model

**Kernel heap.** `kmalloc`/`kzalloc`/`kfree` use eight slab classes from 16 to
2048 bytes. Each slab page starts with a 64-byte header (magic, class, object
and used counts, a 256-bit used map and a free list); objects follow it.
Allocations larger than 2048 bytes, up to the 1 MiB cap, take contiguous PMM
pages with a 64-byte large-allocation header. Slab pages are not
returned to the PMM when they become empty (a phase 2 simplification); large
allocations are returned immediately. All heap paths run with interrupts
disabled.

**Threads and scheduler.** A `thread` owns a 16 KiB kernel stack (four pages)
plus one unmapped guard page, an entry function and argument, a state, a
scheduling class and a name. `cpu_switch` saves the callee-saved registers and
switches stacks; a new thread starts in `thread_entry`, which calls the entry
function and then `thread_exit`. States are `RUNNING`, `RUNNABLE`,
`BLOCKED`, `SLEEPING`, `STOPPED` and `ZOMBIE`; joiners wait on a wait queue
and destroy the thread after it exits.

Each CPU has per-class run queues and picks the highest non-empty class:
`IDLE`, `NORMAL`, `INTERACTIVE`, `REALTIME_SYSTEM`. Within a class it is
round-robin; quanta are 1, 4, 2 and 8 ticks respectively. The timer tick
decrements the quantum and preempts on expiry or when a higher class becomes
runnable. Sleepers wake on tick deadlines. The boot context is registered as
thread 0 and an idle thread executes `hlt` when nothing else is runnable.

**Handles.** Each process owns a 256-entry typed handle table (thread, port,
process). Handles are process-local `index + 1` values; the kernel never
returns raw object pointers to callers. Rights are read, write or both, and
closing a handle clears the slot.

**MsgPort.** A port is an O(1) FIFO of typed messages with a depth of 64 and a
1024-byte payload cap. Senders block when the queue is full, receivers when it
is empty, and both sides use the scheduler wait queues. Message nodes come from
the kernel heap.

**Syscalls.** Vector `0x80` is installed as a DPL3 interrupt gate and routes to
one dispatcher. Phase 2 passes the call number in `rax` and arguments in
`rdi`, `rsi`, `rdx` and `r10`; the result is returned in `rax`, with
negative `-errno` values for errors. The call set is ping, gettid, yield,
sleep ticks, port create/send/recv, handle close and exit. There is no user
mode yet: the self-test invokes the same gate from ring 0.

## Storage

The boot disk is a legacy PCI virtio-blk device (`vda`). `storage_init()`
parses the GPT, prefers the partition named `crugroot`, creates a `vda1`
partition view and mounts it read-only as ext4. It then reads `/hello.txt` into
a static buffer and the self-test re-checks the mount, the directory listing and
the file contents.

The same self-test then remounts the partition read-write and exercises the
write path: it removes stale `/selftest` entries, creates `/selftest` and
`a.txt`, writes and `fsync`s a 5000-byte pattern, renames the file to `b.txt`,
truncates it to 1000 bytes, creates and unlinks `tmp.txt`, unmounts cleanly
(marking the filesystem clean and flushing the virtio cache), and finally
remounts read-only to verify that `/selftest/b.txt` still holds the first 1000
bytes of the pattern.

A journal check follows: the test writes `/selftest/j.bin`, queues a dirty
block through `journal_test_dirty()`, unmounts *without* clearing the journal,
remounts read-write (which replays it through `journal_open()`), and reads the
physical block back. The host gate then extracts the partition and requires
`e2fsck -fn` to pass. The complete stack, supported ext4 subset, write path,
journal format/recovery and known limits are documented in
[storage.md](storage.md).

## Exit protocol

A successful boot ends by writing `0x10` to QEMU's `isa-debug-exit` port
`0xF4`. QEMU exits with status `(0x10 << 1) | 1 = 33`. A panic writes `0x11`,
producing status `35`. The boot test treats any other status (including
timeouts) as failure.

Serial markers are part of the contract:

```
CRUGOS: hello
CRUGOS: pmm ready
CRUGOS: vm ready
CRUGOS: cpu ready
CRUGOS: timer ready
CRUGOS: heap ready
CRUGOS: block ready
CRUGOS: gpt ready
CRUGOS: ext4 ready
CRUGOS: journal ready
CRUGOS: storage ready
CRUGOS: sched ready
CRUGOS: syscall ready
CRUGOS: writable storage ok
CRUGOS: journal selftest ok
CRUGOS: selftest ok
CRUGOS: phase 3b2 boot ok
```

`kernel/core/report.c` prints the machine description between them: kernel
extent, firmware pointers, memory totals, allocator totals, framebuffer mode,
command line, entropy, CPU features, W^X audit, heap totals, scheduler switches,
per-stage microsecond timings and the timer source/frequency.

## Console

Before `console_init` only the 16550 UART at `0x3F8` (115200 8N1) is used. After
init, newlines are also mirrored to the GOP text console, which uses an embedded
8x16 font generated from a host PSF by `tools/gen-font.py`.

## Known phase 3 limitations

- The identity map is limited to the low 4 GiB; user address spaces, a
  higher-half kernel and demand paging arrive with user-mode address spaces.
- Execution is kernel-mode only. There is no user mode, no address-space switch
  and no `SYSCALL` MSR gateway yet; `int 0x80` reaches the same dispatcher.
- Only the boot CPU runs. The LAPIC is enabled and `%gs` per-CPU data exists,
  but AP startup, IOAPIC and external interrupts are phase 5 work.
- Empty slab pages are not returned to the PMM, and page-table pages created
  while splitting huge pages are not reclaimed.
- Storage is writable with JBD2 replay and ordered metadata journaling, but
  one legacy virtio-blk disk, polled one-sector requests, a single
  extent-tree level, one descriptor per transaction, no journal or metadata
  checksums, no RAID1/10 and no `crypt.device` yet.
- `SMEP`/`SMAP` are enabled only when the CPU reports them; `qemu64` does not.
- No shutdown/reset path; QEMU's debug port is the exit mechanism.
