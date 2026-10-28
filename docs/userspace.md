# Ring 3 and the first user program

Phase U1 is the "protected userspace" milestone the specification deferred until
the kernel could afford one: a DPL3 segment pair, an address-space window that
belongs to user code, a copy boundary on every syscall pointer, and a fault
policy that charges a ring-3 fault to the program instead of the kernel. The
phase ends with three real ELF programs running from `/bin` under `make test-user`
(gate 48): a hello, a program that passes a kernel pointer to `SYS_WRITE` and must
get `-EFAULT`, and a program that stores above the user window and must be killed
while the machine keeps running.

Nothing in this phase changes the boot paths, the disk image format or the
loaded program ABI the UEFI loader uses. The kernel gains one syscall
(`SYS_WRITE`) and the first syscall mode switch; every existing gate still boots
with no user program at all.

## Why ring 3 now

The kernel is a single ring-0 address space: a supervisor-only identity map over
the low 4 GiB, page tables built by `vm_init`, and traps that always panic. That
was enough while every pointer reaching a syscall was a kernel pointer, but it
means there is no hardware boundary a program can fail at: a bad pointer is a
kernel fault, and a kernel fault is a panic.

Phase U1 adds the boundary in three pieces:

- a GDT with ring-3 code and data descriptors, so `iretq` can change CPL;
- a user window in the top half of the low canonical range, mapped with `U/S=1`
  while the kernel identity map stays supervisor-only, so ring 3 cannot touch
  kernel memory even with a valid pointer;
- a syscall entry that never dereferences a ring-3 pointer directly, plus a trap
  path that kills the offending thread when the fault frame says CPL3.

The rest of the phase is plumbing around those three: an ELF loader that maps
segments with the right permissions, a per-thread kernel stack so the trap that
arrives from ring 3 lands somewhere valid, and a selftest that exercises each
wall from a real program.

## Segments and the GDT

`include/crug/segment.h` names the selectors, and `kernel/arch/x86_64/cpu.c`
builds the table itself (GDT has grown from five entries to seven):

| Index | Selector | Ring | Use |
| --- | --- | --- | --- |
| 0 | 0x00 | - | null descriptor |
| 1 | 0x08 | 0 | kernel code (`SEG_KERNEL_CS`) |
| 2 | 0x10 | 0 | kernel data (`SEG_KERNEL_DS`) |
| 3 | 0x18 | 0 | 64-bit TSS low half (`SEG_TSS`) |
| 4 | - | 0 | 64-bit TSS high half |
| 5 | 0x2B | 3 | user code (`SEG_USER_CS`, RPL 3) |
| 6 | 0x33 | 3 | user data (`SEG_USER_DS`, RPL 3) |

`GDT_ENTRIES` is 7. The ring-3 descriptors are ordinary DPL3 code/data entries;
the TSS keeps the IST1..IST4 stacks phase H3 installed (see [traps.md](traps.md)),
so a `#DF`, `#NMI`, `#MC` or `#DB` taken from ring 3 still lands on a stack that
was never user-controlled.

The TSS `rsp0` field is now per thread: `cpu_set_kernel_stack(u64 top)` is the
only writer, and `schedule()` installs the incoming thread's kernel stack top
before it switches (for a kernel thread, the boot stack top). Without that, the
first ring-3 trap would push its frame onto the user stack and the handler would
run on attacker-chosen memory.

Note for the trap recipes: index 6 / selector 0x33 used to be past the end of the
GDT, so phase H3's `trap.test=gp` recipe loaded `%ds` with 0x33 to fault. Phase U1
made 0x33 the valid ring-3 data descriptor, so the recipe moved one slot further
out and now loads `0x3b` (index 7), which is past the seven-entry table. The
recipe table in [traps.md](traps.md) states that selector.

## Crossing the boundary: the iretq frame

`kernel/arch/x86_64/user.S` provides `thread_user_start`, the first instruction a
fresh user thread runs on its kernel stack. It builds the iretq frame from the
thread record and drops to CPL3:

```asm
cli
pushq %rax      /* ss  = 0x33 */
pushq %rdx      /* rsp = thread->user_rsp */
pushq $0x202    /* rflags: IF=1, IOPL=0 */
pushq %rbx      /* cs  = 0x2B */
pushq %rcx      /* rip = thread->user_rip */
movw %ax, %ds
movw %ax, %es
iretq
```

Interrupts stay off until the iretq restores `RFLAGS`, so the thread cannot be
preempted between the stack switch and the first user instruction. The frame is
built from `thread_user_frame()` in `kernel/core/thread.c`, which fills `rip`,
`rsp`, `ss` and `cs` from the thread's `user_rip` / `user_rsp` fields.

## The user window

User mappings live in one fixed window, defined in `include/crug/vm.h`:

| Name | Value | Meaning |
| --- | --- | --- |
| `USER_BASE` | `0x0000008000000000` | 512 GiB, start of PML4 slot 1 |
| `USER_TOP` | `0x0000010000000000` | 1 TiB, one byte past the window |
| `USER_PAGE` | `CRUG_PAGE_SIZE` | 4096 |

The kernel identity map occupies PML4 slot 0 and stays supervisor-only, so a
`U/S=1` leaf can only exist above `USER_BASE`; the hardware itself then refuses
ring-3 access below it. `kernel/mm/vm.c` adds three helpers on top of the normal
mapper:

```c
int  vm_user_map_page(u64 va, u64 pa, u64 flags);
int  vm_user_map_pages(u64 va, u64 pages, u64 flags);
void vm_user_unmap_pages(u64 va, u64 pages);
```

`vm_user_map_page` creates every intermediate PDPTE/PDE with `VM_USER`, because a
leaf below a supervisor-only table is unreachable from CPL3. It strips
`VM_GLOBAL` (a global user page is never the intent) and the caller supplies
W^X: executable segments are mapped read+execute, data segments read+write plus
`VM_NX`.

## Loading an ELF from `/bin`

`kernel/exec/elf_exec.c` is the whole pipeline; `include/crug/exec.h` holds the
constants and the result struct.

1. `elf_exec_run(path, &res)` reads the file from the mounted root with
   `ext4_read_file` into a kernel buffer of at most `ELF_MAX_BYTES` (512 KiB).
2. The image is parsed with the same `boot/efi/elf.c` code the UEFI loader uses;
   the kernel links that file in and only adds the segment-mapping policy, so
   the two loaders cannot drift.
3. Each `PT_LOAD` is validated by `harden_user_segment_ok(va, size)`: page
   aligned, inside the window, and small enough to be representable. Empty
   segments are ignored - the linker emits an empty R+W `PT_LOAD` at vaddr 0 for
   some layouts, and mapping it would be a second, bogus segment.
4. Pages are reserved with `vm_user_map_pages` using the segment's permissions
   (no `VM_WRITE` and no `VM_NX` for executable segments; `VM_WRITE|VM_NX` for
   data), then zeroed and filled.
5. Contents are copied through `pmm_phys_to_virt(pa)`, never through the user
   mapping, so a later SMAP-enabled CPU cannot trap the loader while it writes
   the program's own pages.
6. The stack is `USER_STACK_PAGES` (4) pages ending at `USER_STACK_TOP`
   (`0x0000008040000000`). Nothing is mapped below it, so a stack overflow that
   walks down lands in an unmapped page and faults in the program instead of
   silently corrupting something.
7. `thread_create_user(path, entry, sp, maps, count, ...)` records the image
   (up to `THREAD_USER_MAP_MAX` = 8 mapped regions) and makes the thread
   runnable.

The entry point is checked by `harden_user_entry_ok`, and `thread_create_user`
checks the initial user stack pointer with the same validator. A program whose
entry or stack is outside the window simply fails to start.

## The syscall boundary

The dispatcher in `kernel/core/syscall.c` serves both rings. It decides once per
call:

```c
static int from_user(const trap_frame *frame)
{
    return (frame->cs & 3u) == 3u;
}
```

A CPL0 caller keeps the old identity-map path, so the in-kernel selftests behave
exactly as before. A CPL3 caller goes through `kernel/mm/uaccess.c` instead:

- `uaccess_check(addr, len)` is pure arithmetic (`harden_user_range_ok`): the
  range must lie inside the window and must not wrap.
- `uaccess_pages_mapped(addr, len)` walks the page tables and requires every
  page in the range to be present, so an unmapped user page returns
  `UACCESS_EFAULT` (the syscall layer reports `SYS_EFAULT`, -14) instead of
  taking a kernel `#PF`.
- `stac`/`clac` bracket only the actual copy, and only when CPUID exposes SMAP.

The range check and the page walk are the real boundary; `stac`/`clac` exists so
the copy can touch a user page on CPUs that enforce SMAP. That ordering is why
the `badptr` program gets a clean `-EFAULT` rather than a report: a kernel address
fails the window check before any page-table walk.

The three copy primitives are the only way the kernel touches user memory:

```c
int uaccess_user_read (void *kdst, u64 uaddr, u64 len);
int uaccess_user_write(u64 uaddr, const void *ksrc, u64 len);
int uaccess_user_str  (char *kdst, u64 uaddr, u64 cap);
```

`uaccess_user_str` reports an unterminated string as `UACCESS_EFAULT`; it never
scans past `cap`.

`SYS_WRITE` (16) is the new call: `fd` must be 1 or 2, the length is clamped to
4096 bytes, and the kernel copies in 256-byte chunks into a stack buffer
(`SYS_BOUNCE_MAX` covers the larger message ports) and `kprintf`s each chunk.
`SYS_MAX` is 17. `SYS_EXIT` (8) from CPL3 records the exit code and ends only that
thread; the kernel continues. `SYS_EBUSY` (-16) is the new error, used when a
second live user thread is requested.

## Scheduler and user threads

A user thread is an ordinary `thread` with `THREAD_FLAG_USER` set, plus
`user_rip`, `user_rsp` and a list of up to `THREAD_USER_MAP_MAX` (8) mapped
regions. The scheduler does not special-case it beyond installing its kernel
stack top into `rsp0`; `thread_user_start` is its first instruction.

`thread_create_user` refuses a second live user thread with `-EBUSY` - one ring-3
program at a time in this phase. `thread_destroy` unmaps the recorded user pages,
so a finished program leaves no window mappings behind, and `thread_join_code`
returns the exit code the fault path or `SYS_EXIT` recorded. `thread_kill_current`
is the single non-returning exit used by both.

## When a ring-3 program faults

A fault taken at CPL3 writes the same crash report as any other exception (phase
H3, see [traps.md](traps.md)) and then branches on the classification:

- `TRAP_USER` from any vector other than `#NMI` (2), `#DF` (8) and `#MC` (18)
  prints two lines and kills the faulting thread with code `-vector`:

  ```
  CRUGOS: user fault page fault at rip=0x0000008000010056 err=0x0000000000000006 tid 10
  CRUGOS: user fault killed tid 10 code -14
  ```

- `TRAP_USER` for 2, 8 or 18 still panics: a bad `#NMI`/`#DF`/`#MC` frame cannot be
  attributed to the program safely, so the machine stops.
- Any CPL0 fault still panics, unchanged.

The report lines themselves say `from=user` because `trap_classify` saw `cs & 3 ==
3`; the fault sample the gate captures is:

```
CRUGOS: trap report page fault vector=14 error=0x6 ist=0 from=user
CRUGOS: trap fault present=no write=yes user=yes reserved=no instruction=no cr2=0x10000000000
```

That is a store (`error` bit 1) by user code (bit 2) to the first address above
the window, which is why `cr2` is `0x10000000000` and no page is present.

## The three programs

`user/` holds the source and the linker script; `make user` builds them and
`scripts/mkdisk.sh` writes them into `/bin` when `USER_DIR` is set.

| Program | What it proves | Expected exit |
| --- | --- | --- |
| `/bin/hello` | a ring-3 program can run, call `SYS_WRITE` and return | 0 |
| `/bin/badptr` | a kernel pointer is refused with `-EFAULT`, not dereferenced | 0 |
| `/bin/fault` | a store above the window kills only the program | -14 |

`hello.c` prints `CRUGOS: user hello from ring 3` and returns 0. `badptr.c` calls
`SYS_WRITE` with `0x00100000` (a kernel address) and checks that the return value
is `-14`; if it is not, it prints a failure line and returns 1. `fault.c` prints
a line, stores to `0x0000010000000000` (the first byte past the window) and, if
it survives, returns 2 with a "did not happen" line.

## Building a user program

A user program is a freestanding ELF64 linked by `user/user.ld`:

| Rule | Value | Why |
| --- | --- | --- |
| entry | `_start` in `user/start.S` | the kernel iretqs to the ELF entry |
| base | `0x0000008000010000` | inside the window, one page above `USER_BASE` |
| text | `PT_LOAD` R+X | executable, never writable |
| data | `PT_LOAD` R+W, `VM_NX` | writable, never executable |
| segments | page aligned | the loader maps whole pages and refuses unaligned p_vaddr |

`start.S` aligns the stack, calls `main`, and turns main's return value into
`SYS_EXIT` - there are no arguments and no `argv`. It cannot use `cli` or `hlt`,
which are privileged. `user/lib.h` is the whole runtime: inline `int $0x80`
wrappers (`nr` in `rax`, arguments in `rdi`, `rsi`, `rdx`, then `r10`), plus
`u_strlen`, `u_puts` and `u_exit`.

The build rule is a single Makefile target:

```sh
make user
```

which compiles `user/*.c` with `-fcrug-kernel -ffreestanding -mcmodel=large
-nostdlib -no-pie -Wl,-T,user/user.ld` into `build/x86_64/user/{hello,badptr,fault}`.
`scripts/mkdisk.sh` reads `USER_DIR` from the environment (the existing
raid/nvme/crypt call sites are unchanged) and writes each program into `/bin`
with `debugfs`. A missing program is a warning at disk-build time and a
"did not start" failure at run time.

## Markers

| Line | When | Meaning |
| --- | --- | --- |
| `CRUGOS: user selftest start` | `user.test=1` boot | the kernel is about to run the three programs |
| `CRUGOS: user hello from ring 3` | from `/bin/hello` | CPL3 code reached `SYS_WRITE` |
| `CRUGOS: user program /bin/hello finished (exit 0)` | after join | the first ring-3 program returned |
| `CRUGOS: user badptr blocked (EFAULT)` | from `/bin/badptr` | `SYS_WRITE` refused a kernel pointer |
| `CRUGOS: user program /bin/badptr finished (exit 0)` | after join | the pointer gate held |
| `CRUGOS: user fault writing above the window` | from `/bin/fault` | the faulting store is about to happen |
| `CRUGOS: user fault page fault at rip=... err=... tid N` | trap path | the fault was charged to the program |
| `CRUGOS: user fault killed tid N code -14` | trap path | only that thread died |
| `CRUGOS: user program /bin/fault finished (exit -14)` | after join | the kernel survived and saw the code |
| `CRUGOS: user selftest ok (3 program(s), 1 fault(s))` | end | all three programs behaved |

The crash report lines (`CRUGOS: trap report ...`) are documented in
[traps.md](traps.md) and appear between the fault line and the kill line.

## The gate

`make test-user` is gate 48 and the only gate that sets `EXPECT_USER=yes`:

```sh
make test-user
```

It rebuilds the ESP and the disk with the user programs (`disk` now depends on
`user`), boots with `user.test=1` on the kernel command line under a
`USER_RUN_TIMEOUT_SEC` (240) budget, then runs `scripts/boot-check.py` with the
new final argument `EXPECT_USER` (`argv[27]`) and `check-disk`. Every other gate
keeps `EXPECT_USER=no`, so boot-check fails a plain boot that prints any user
marker - the feature must be dormant unless the token is present.

Captured output from the gate (the report's `trap rip=` line and the two
`trap regs` lines are elided - they carry the same frame the `user fault` line
repeats):

```
CRUGOS: user selftest start
CRUGOS: user hello from ring 3
CRUGOS: user program /bin/hello finished (exit 0)
CRUGOS: user badptr blocked (EFAULT)
CRUGOS: user program /bin/badptr finished (exit 0)
CRUGOS: user fault writing above the window
CRUGOS: trap report page fault vector=14 error=0x6 ist=0 from=user
CRUGOS: trap fault present=no write=yes user=yes reserved=no instruction=no cr2=0x10000000000
CRUGOS: user fault page fault at rip=0x0000008000010056 err=0x0000000000000006 tid 10
CRUGOS: user fault killed tid 10 code -14
CRUGOS: user program /bin/fault finished (exit -14)
CRUGOS: user selftest ok (3 program(s), 1 fault(s))
CRUGOS: phase u1 boot ok
```

The boot check that accepts it:

```
ok: boot markers found, version 0.35.0 avx2 yes raid no nvme no rebuild no ahci no crypt no aes any net no tcp no dns no usb no mouse no audio no gfx no wb no menu no shell no keys no desk no icons no drawer no select no heap no user yes storage disk QEMU exit status 33
```

The `user yes` field is the new one; all other fields are the same as the H4
gate. The boot timing line is unchanged in shape:

```
boot   : pmm=28541 us cpu=418 us vm=657 us traps=328 us timer=65263 us heap=426 us storage=93624 us sched=305 us syscall=94 us total=189659 us
```

Reading a failure:

- no `user selftest start` means the token did not reach the kernel (check the
  `CMDLINE_EXTRA` wiring) or the disk lacks the programs;
- `user selftest failed (/bin/... did not start)` means the loader refused the
  image - check the segment policy and the linker script;
- `(... exited X, expected Y)` names the exact mismatch: badptr returning 0
  instead of a clean `-EFAULT`, or fault not dying with -14;
- a panic during the run means a fault was classified `TRAP_FATAL` - the panic
  line names the vector;
- an unexpected `CRUGOS: user ...` line on a non-user gate is itself a failure
  (boot-check requires `EXPECT_USER=yes`).

## Limits

- One user thread at a time: `thread_create_user` returns `-EBUSY` for a second
  live program, and the selftest runs the three executables strictly in sequence.
- One shared CR3: the window is a fixed range in the kernel's own page tables, so
  two user programs could not be isolated from each other yet. A per-process
  address space is future work.
- No `fork` or runtime `exec`: `elf_exec_run` is called by the kernel (selftest,
  future shell), not by a program.
- No `argv` or environment: the kernel enters at `_start` with an aligned stack
  and nothing else; the three test programs take no arguments.
- No ring-3 preemption promises beyond the ordinary scheduler: the iretq enables
  interrupts before the first user instruction, and the timer path is the same as
  for kernel threads.
- SMAP/SMEP enforcement is code-present but the QEMU CPU the gate uses does not
  expose SMAP, so only the `stac`/`clac` pair is skipped on this host; the range
  check and the page walk always run, and a CPU with SMAP takes the same path
  with the access window opened around the copy.
- A user image is capped at `ELF_MAX_BYTES` (512 KiB), a thread records at most
  `THREAD_USER_MAP_MAX` (8) mapped regions, and the stack is four pages with no
  growth: a program that needs more has to change these constants first.
- The user window's intermediate page tables (the PML4 slot 1 PDPT, its PD and
  the PTs) are allocated once and never released; only the leaf data pages are
  freed when a user thread is destroyed.
- The report formatter still caps a report at 1024 bytes (phase H3 limit); a very
  early ring-3 fault in a large frame could truncate its tail.
