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
       - self-tests, boot report, deterministic exit
```

## BootInfo

`include/crug/bootinfo.h` is the single contract between loader and kernel.
The structure is versioned (`magic = 0x43524731`, `version = 1`) and the kernel
refuses to boot if either does not match or if required fields are invalid.
Phase 1 did not change the layout.

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
6. `timer_init`: TSC frequency, TSC-deadline or LAPIC periodic, `sti`.
7. `selftest_run`: allocator, mappings, W^X audit, `int3`, timer ticks.
8. `report_boot`, `CRUGOS: phase 1 boot ok`, `arch_exit_qemu(0x10)`.

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
CRUGOS: selftest ok
CRUGOS: phase 1 boot ok
```

`kernel/core/report.c` prints the machine description between them: kernel
extent, firmware pointers, memory totals, allocator totals, framebuffer mode,
command line, entropy, CPU features, W^X audit, per-stage microsecond timings
and the timer source/frequency.

## Console

Before `console_init` only the 16550 UART at `0x3F8` (115200 8N1) is used. After
init, newlines are also mirrored to the GOP text console, which uses an embedded
8x16 font generated from a host PSF by `tools/gen-font.py`.

## Known phase 1 limitations

- The identity map is limited to the low 4 GiB; user address spaces, a
  higher-half kernel and demand paging arrive with the phase 2 VM objects.
- There is no kernel heap yet (`kmalloc`/small-object caches land in phase 2).
- Single CPU: the LAPIC is enabled and `%gs` per-CPU data exists, but AP
  startup, IOAPIC and external interrupts are phase 5 work.
- `SMEP`/`SMAP` are enabled only when the CPU reports them; `qemu64` does not.
- No shutdown/reset path; QEMU's debug port is the exit mechanism.
