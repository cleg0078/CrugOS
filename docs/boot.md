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
```

## BootInfo

`include/crug/bootinfo.h` is the single contract between loader and kernel.
The structure is versioned (`magic = 0x43524731`, `version = 1`) and the kernel
refuses to boot if either does not match or if required fields are invalid.

| Field | Meaning |
| --- | --- |
| `kernel_base`, `kernel_size` | physical extent of the loaded kernel image |
| `memory_map`, `memory_map_count` | normalized `memory_region[]` |
| `fb` | GOP framebuffer base/size/resolution/stride/pixel format |
| `command_line`, `command_line_len` | optional, currently NULL/0 |
| `acpi_rsdp` | ACPI 2.0 (or 1.0) RSDP physical address |
| `fdt` | flattened device tree, 0 on x86_64 |
| `initrd_base`, `initrd_size` | optional initial ramdisk, currently 0 |
| `boot_cpu_id` | APIC id of the boot CPU (0 in phase 0) |
| `entropy_seed` | 64 bytes from EFI RNG, RDRAND or timing jitter |
| `boot_tsc` | TSC value captured by the loader before handoff |

Memory regions are classified as `USABLE`, `RESERVED`, `ACPI_RECLAIM`,
`ACPI_NVS`, `UNUSABLE`, `BOOTLOADER`, `FRAMEBUFFER` or `KERNEL`. The loader
splits regions so that the kernel image and the framebuffer are marked
precisely; the kernel and boot metadata are never reported as usable.

## ABI at handoff

- CPU: long mode, paging enabled by firmware, interrupts disabled by `_start`.
- Calling convention: System V AMD64 (`kernel_main(boot_info *)` in `rdi`).
- Stack: the kernel switches to its own 32 KiB boot stack in `.bss` before
  touching C code.
- Firmware services are gone: the kernel may only use memory and devices
  described by `BootInfo`.

## Exit protocol

Phase 0 has no shutdown path yet, so a successful boot ends by writing `0x10`
to QEMU's `isa-debug-exit` port `0xF4`. QEMU exits with status `(0x10 << 1) | 1
= 33`. A panic writes `0x11`, producing status `35`. The boot test treats any
other status (including timeouts) as failure.

Serial markers are part of the contract:

```
CRUGOS: hello
CRUGOS: phase 0 boot ok
```

`kernel/core/report.c` prints the machine description between them: kernel
extent, firmware pointers, memory totals, framebuffer mode, command line,
entropy and a stage-timing placeholder (TSC frequency calibration lands with
the timer workstream).

## Console

Before `console_init` only the 16550 UART at `0x3F8` (115200 8N1) is used. After
init, newlines are also mirrored to the GOP text console, which uses an embedded
8x16 font generated from a host PSF by `tools/gen-font.py`.

## Known phase 0 limitations

- Paging tables are whatever the firmware left behind; the kernel identity-
  maps low memory only implicitly.
- No interrupts, no timer, no scheduler.
- No MMU ownership of the memory map yet (the phase 1 allocator will consume it).
- Single CPU only.
- No shutdown/reset path; QEMU's debug port is the exit mechanism.
