# Building CrugOS

This document describes the phase 0 build system: how the kernel and the UEFI
loader are compiled and linked, which invariants the link must preserve, and how
to diagnose the failure modes that are specific to this toolchain.

## Requirements

| Tool | Purpose | Notes |
| --- | --- | --- |
| `crugc` | Crug compiler | 0.2.3 / GCC 16.2.0, host `x86_64-pc-linux-gnu`; `-fcrug-kernel` |
| `ld` (GNU binutils) | loader link | must support `-m i386pep` |
| `qemu-system-x86_64` | boot test | 10.x, TCG works without `/dev/kvm` |
| OVMF | UEFI firmware | `/usr/share/OVMF/OVMF_CODE_4M.fd` + `OVMF_VARS_4M.fd` |
| `python3` | boot/PE checks | standard library only |

Run `make preflight` to check all of the above.

## Targets

```
make             # kernel.elf, BOOTX64.EFI and the ESP tree
make run         # boot the ESP tree under QEMU + OVMF
make test        # boot and verify serial markers + QEMU exit status
make test-host   # host unit tests for string/printf/ELF loader
make size        # kernel size budget (<1 MiB stretch, <2 MiB hard)
make check-efi   # PE32+ structure checks on the loader
make repro       # two independent builds must be byte-identical
make preflight   # toolchain/firmware availability
make clean       # remove build/<arch>
make distclean   # remove build/ and host test artifacts
```

Useful variables: `ARCH=x86_64` (only arch implemented), `MODE=release|debug|size`,
`V=1` for verbose command lines.

## Kernel link

The kernel is a freestanding ELF64 executable linked at a fixed address:

```
KERNEL_BASE = 0x00100000
```

`kernel/arch/x86_64/linker.ld` lays out `.text`, `.rodata`, `.data` and `.bss` on
4 KiB boundaries and exports:

```
__text_start __text_end   __bss_start __bss_end __kernel_end
```

Link flags that matter:

```
-nostdlib -static -no-pie -Wl,--build-id=none -Wl,-T,kernel/arch/x86_64/linker.ld
```

`--build-id=none` keeps the image reproducible and removes a non-loadable note.
The script discards `.comment`, `.note*`, `.eh_frame`, `.rela*` and dynamic
sections; the kernel must not need runtime relocations. C sources are compiled
with `-fno-pic -fno-pie -fno-builtin -fno-stack-protector -mno-red-zone` and no
SSE/FPU (`-mno-sse -mno-sse2 -mno-mmx -mno-80387`), so the compiler never emits
floating-point state save/restore in interrupt-adjacent code.

## Loader link

The UEFI loader is a PE32+ EFI application, not an ELF. It is linked with GNU
`ld` directly:

```
ld -m i386pep --subsystem 10 -e efi_main \
   --image-base 0x10000000 --disable-reloc-section --no-insert-timestamp
```

Hard-won constraints (each of these was a real OVMF failure):

1. `efi_main` and every firmware function pointer use the Microsoft ABI
   (`__attribute__((ms_abi))`, `EFIAPI`). Mixing SysV and MS ABI raises `#UD`
   on the first indirect firmware call.
2. Compile loader objects with `-fno-ident`. Otherwise `.comment` lands at RVA 0
   and OVMF refuses the image (`Load Error`).
3. Do not use `-ffunction-sections` + `--gc-sections`; the entry point can be
   garbage collected and firmware reports no output.
4. Do not use a custom PE linker script or `--enable-reloc-section`. ELF-derived
   `.reloc` contents are invalid here and OVMF crashes loading the image.
5. No `mtools` is required: QEMU's `vvfat` driver exposes the ESP directory
   directly. The process `TMPDIR` must be writable.

## Reproducibility

`make repro` builds `kernel.elf` and `BOOTX64.EFI` twice under different
`BUILD_ROOT`s and compares them byte-for-byte. Any timestamp, path or hash
randomisation that leaks into the output will fail this target.

## Troubleshooting

- **`Load Error` from OVMF**: inspect `.comment`/first section alignment and
  confirm `-fno-ident` and `--disable-reloc-section`.
- **`!!!! Find image based on IP(...)`**: invalid or stripped relocation data;
  rebuild the loader with the exact link line above.
- **No serial output at all**: OVMF may have printed only to the framebuffer;
  check `build/<arch>/debug.log` (port `0xE9`) and re-run with `-display none`
  omitted.
- **`ExitBootServices failed`**: the memory map changed between snapshot and
  call; the loader retries three times, re-reading the map each time.
- **QEMU exits before OVMF starts**: verify the firmware paths and that
  `OVMF_VARS` is writable (the run script copies it into `build/`).
