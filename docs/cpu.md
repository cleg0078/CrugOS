# CPU baseline and AVX2

CrugOS requires a 64-bit x86-64 CPU and, by default, AVX2. AVX2 is not yet
used to execute kernel code: phase 1 detects and enables the state so that the
AVX register file is usable once context switching lands.

## Why AVX2 is required

AVX2 doubles the integer vector width and adds fused multiply-add. The
specification's HPC target lists `x86-64-v3` and AVX2 as capability classes
(section 73), and userspace will target `x86-64-v3` from phase 4 onwards.
Requiring it keeps a single system ABI instead of carrying a second
`x86-64-v2` kernel path.

## Detection and enablement

`kernel/arch/x86_64/cpu.c` checks, in order:

1. `CPUID.1:ECX[27]` (OSXSAVE) and `CPUID.1:ECX[28]` (AVX);
2. `CPUID.7.0:EBX[5]` (AVX2);
3. `CPUID.0xD:0` for the valid XCR0 bits.

If all are present, the kernel sets `CR4.OSXSAVE` and `XSETBV` with
`XCR0 = 0x7` (x87, SSE, AVX) masked by the CPU-supported bits. The boot report
prints `avx2=yes|no required=yes|no`.

The kernel itself is compiled with `-mno-avx -mno-avx2 -mno-avx512f` in
addition to `-mno-sse -mno-sse2`. Until the context-switch path saves YMM
state, no compiler-generated AVX instruction may execute.

## Requirement policy

| Build | Behaviour |
| --- | --- |
| `REQUIRE_AVX2=1` (default) | `kernel_main` panics with `AVX2 is required but unavailable` when CPUID lacks AVX2 |
| `REQUIRE_AVX2=0` | boot continues and the report prints `avx2=no required=no` |

`make test` therefore runs on an AVX2-capable model by default:

```
make test            # QEMU_CPU=qemu64,+avx,+avx2, REQUIRE_AVX2=1
make test-compat     # QEMU_CPU=qemu64, REQUIRE_AVX2=0
```

## QEMU models

`qemu64` does not advertise AVX2, SMEP, SMAP or TSC-deadline. Adding the
features explicitly is reliable:

```
make test QEMU_CPU=qemu64,+avx,+avx2
```

Do not use `-cpu max` or `-cpu Haswell` with this QEMU/OVMF pair: both models
raise a general-protection fault in OVMF during `ExitBootServices` after the
loader entry, before the kernel starts. This is a firmware/QEMU interaction,
not a kernel bug.

