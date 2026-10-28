# Ring 3 and the per-process address space

Phase U1 is the "protected userspace" milestone the specification deferred until
the kernel could afford one: a DPL3 segment pair, an address-space window that
belongs to user code, a copy boundary on every syscall pointer, and a fault
policy that charges a ring-3 fault to the program instead of the kernel. Phase
U2 then turned the single shared window into one PML4 per ring-3 thread, so two
programs run at the same time with the same virtual address pointing at
different frames.

The current gate is `make test-user2` (gate 49): it boots with `user.test=2`,
runs the three U1 programs in sequence and then two instances of an isolation
program concurrently, and checks that each sees its own word at the shared
address.

Nothing in these phases changes the boot paths, the disk image format or the
loaded program ABI the UEFI loader uses. The kernel gains `SYS_WRITE` and one
syscall mode switch; every non-user gate still boots with no user program at all.

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

Phase U2 completes the isolation: every ring-3 thread owns a page-table root, so
the window is private rather than a shared range in the kernel's tables. The
rest of the work is plumbing around those pieces: an ELF loader that maps
segments with the right permissions, a per-thread kernel stack so the trap that
arrives from ring 3 lands somewhere valid, and selftests that exercise each wall
from real programs.

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

The TSS `rsp0` field is per thread: `cpu_set_kernel_stack(u64 top)` is the only
writer, and `schedule()` installs the incoming thread's kernel stack top before
it switches (for a kernel thread, the boot stack top). Without that, the first
ring-3 trap would push its frame onto the user stack and the handler would run
on attacker-chosen memory.

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
| `USER_STACK_TOP` | `0x0000008040000000` | top of the four-page program stack |

Each `vm_space` carries two populated slots:

- slot 0 is copied from the kernel's own root when the space is created, so the
  supervisor identity map (kernel image, stacks, MMIO) is present in every
  space. The kernel runs on all of them.
- slot 1 is the 512 GiB to 1 TiB user window. Only leaves with `U/S=1` exist
  there, and the hardware refuses ring-3 access to anything below `USER_BASE`.

Because slot 0 is shared, this is address-space isolation *between user
programs*, not isolation between a program and the kernel - that separation is
still the U/S bit on the leaves.

## One PML4 per ring-3 thread

`include/crug/vm.h` declares the type and the API; `kernel/mm/vm_space.c` is the
implementation:

```c
typedef struct vm_space {
    u64 *pml4;
    u64  phys;
    u32  user_pages;
    u32  pad;
} vm_space;

int  vm_space_create(vm_space *space);
void vm_space_destroy(vm_space *space);
int  vm_space_map_user_pages(vm_space *space, u64 va, u64 pages, u64 flags);
void vm_space_unmap_user_pages(vm_space *space, u64 va, u64 pages);
int  vm_space_translate(const vm_space *space, u64 va);
void vm_space_activate(vm_space *space);
u64  vm_kernel_root_phys(void);
```

`vm_space.c` is a separate, host-testable unit: it needs only
`pmm_alloc_page`/`pmm_free_page` and the identity translations, never CR3 or the
LAPIC. The CR3 write lives in `kernel/mm/vm.c`, where `vm_space_activate` is the
only writer.

Creating a space copies slot 0 from the kernel root (registered once by
`vm_space_set_kernel_root`, and available as `vm_kernel_root_phys()`) and leaves
slot 1 empty. Mapping a run of pages then:

- upgrades every intermediate entry to `PRESENT|WRITE|USER`, because a leaf
  below a supervisor-only table is unreachable from CPL3;
- clears `VM_GLOBAL` on the leaf (a global user page is never the intent) and
  forces `VM_USER`;
- is validated first: page-aligned VA, inside the window, the whole run inside
  the window, and no leaf already present. A failure partway through a run rolls
  back exactly the pages that call had mapped, not the earlier ones.

Data pages are mapped `VM_WRITE|VM_NX`, executable pages neither, so W^X is the
caller's choice encoded in `flags`. `vm_space_destroy` walks slot 1 and frees
every leaf frame, every PT, every PD, the PDPT and the root page itself.

`kernel/mm/vm.c` keeps the active root in a static `g_root`; `pte_for` and
`vm_translate` follow it, so kernel-side uaccess always reads the running
space. `vm_space_activate(space)` swaps CR3 only when the root actually changes,
which keeps a switch between two threads of the same space free.

## Loading an ELF from `/bin`

`kernel/exec/elf_exec.c` is the whole pipeline; `include/crug/exec.h` holds the
constants and the result struct. Each exec owns a fresh `vm_space`:

1. `elf_exec_run(path, &res)` reads the file from the mounted root with
   `ext4_read_file` into a kernel buffer of at most `ELF_MAX_BYTES` (512 KiB).
2. The image is parsed with the same `boot/efi/elf.c` code the UEFI loader uses;
   the kernel links that file in and only adds the segment-mapping policy, so
   the two loaders cannot drift.
3. Each `PT_LOAD` is validated by `harden_user_segment_ok(va, size)`: page
   aligned, inside the window, and small enough to be representable. Empty
   segments are ignored - the linker emits an empty R+W `PT_LOAD` at vaddr 0 for
   some layouts, and mapping it would be a second, bogus segment.
4. Pages are reserved in that exec's space with `vm_space_map_user_pages` using
   the segment's permissions (no `VM_WRITE` and no `VM_NX` for executable
   segments; `VM_WRITE|VM_NX` for data), then zeroed and filled.
5. Contents are copied through `pmm_phys_to_virt(pa)`, never through the user
   mapping, so a later SMAP-enabled CPU cannot trap the loader while it writes
   the program's own pages.
6. The stack is `USER_STACK_PAGES` (4) pages ending at `USER_STACK_TOP`.
   Nothing is mapped below it, so a stack overflow that walks down lands in an
   unmapped page and faults in the program instead of silently corrupting
   something.
7. The space is handed to `thread_create_user(path, entry, sp, space,
   SCHED_NORMAL)`, which makes the thread runnable with its own root.

The entry point is checked by `harden_user_entry_ok`, and `thread_create_user`
checks the initial user stack pointer with the same validator. A program whose
entry or stack is outside the window simply fails to start, and the space it was
going to use is destroyed.

One bug worth remembering from landing U2: `vm_init` used to publish the root
only after mapping the kernel sections, but the section mapper walks the root, so
the boot page-faulted before the IDT existed. It now publishes
`g_root`/`vm_space_set_kernel_root` immediately after allocating the root page.

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
- `uaccess_pages_mapped(addr, len)` walks the page tables through
  `vm_translate`, which follows the active root, and requires every page in the
  range to be present, so an unmapped user page returns `UACCESS_EFAULT` (the
  syscall layer reports `SYS_EFAULT`, -14) instead of taking a kernel `#PF`.
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

`SYS_WRITE` (16) is the U1 call: `fd` must be 1 or 2, the length is clamped to
4096 bytes, and the kernel copies in 256-byte chunks into a stack buffer
(`SYS_BOUNCE_MAX` covers the larger message ports) and `kprintf`s each chunk.
`SYS_MAX` is 17. `SYS_EXIT` (8) from CPL3 records the exit code and ends only that
thread; the kernel continues. `SYS_EBUSY` (-16) is the error a caller gets when a
thread cannot be created.

## Scheduler and user threads

A user thread is an ordinary `thread` with `THREAD_FLAG_USER` set, plus
`user_rip`, `user_rsp` and a `vm_space *space`. The old
`thread_user_map[8]` region list is gone: the thread record no longer remembers
individual mappings, because the space owns them.

`schedule()` installs the incoming thread's kernel stack top into `rsp0` and then
calls `vm_space_activate(next->space)`; a kernel thread carries the kernel root,
so the call is a no-op for it. `thread_user_start` is the first instruction a
user thread runs, and `thread_destroy` destroys the space, freeing every mapping
it still holds.

`thread_create_user(name, entry, user_rsp, space, sched_class)` no longer refuses
a second user thread: two ring-3 programs run concurrently, each on its own
PML4, and `thread_join_code` returns the exit code the fault path or `SYS_EXIT`
recorded. `thread_kill_current` is the single non-returning exit used by both
the fault path and `SYS_EXIT`.

## When a ring-3 program faults

A fault taken at CPL3 writes the same crash report as any other exception (phase
H3, see [traps.md](traps.md)) and then branches on the classification:

- `TRAP_USER` from any vector other than `#NMI` (2), `#DF` (8) and `#MC` (18)
  prints two lines and kills the faulting thread with code `-vector`:

  ```
  CRUGOS: user fault page fault at rip=0x0000008000010054 err=0x0000000000000006 tid 10
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
the window, which is why `cr2` is `0x10000000000` and no page is present. The
report names the tid, so with several user threads alive the fault line says
exactly which program died.

## The user programs

`user/` holds the sources and the linker scripts; `make user` builds them and
`scripts/mkdisk.sh` writes them into `/bin` when `USER_DIR` is set. The U1 three
run one after another; the U2 isolation pair runs at the same time.

| Program | What it proves | Expected exit |
| --- | --- | --- |
| `/bin/hello` | a ring-3 program can run, call `SYS_WRITE` and return | 0 |
| `/bin/badptr` | a kernel pointer is refused with `-EFAULT`, not dereferenced | 0 |
| `/bin/fault` | a store above the window kills only the program | -14 |
| `/bin/iso` | two instances at the same VA see different frames | 0 |

`hello.c` prints `CRUGOS: user hello from ring 3` and returns 0. `badptr.c` calls
`SYS_WRITE` with `0x00100000` (a kernel address) and checks that the return value
is `-14`; if it is not, it prints a failure line and returns 1. `fault.c` prints
a line, stores to `0x0000010000000000` (the first byte past the window) and, if
it survives, returns 2 with a "did not happen" line.

`iso.c` is the isolation proof. Its `.iso` section pins a writable page at
`0x0000008000030000`; each instance writes `0x15500000 | (tid << 8)`, yields 300
times so the other instance runs, then re-reads its own word and fails if it
changed. `user_test.c` starts two instances, checks that the **same virtual
address** maps to two different physical frames (it prints
`CRUGOS: user isolation frames 0x28000 vs 0x38000 at 0x8000030000`), then joins
both and counts mismatches. Two words `0x15500b00` and `0x15500c00` mean tids 11
and 12 both kept their own value.

## Building a user program

A user program is a freestanding ELF64 linked by `user/user.ld` (the isolation
program uses `user/iso.ld` because it needs the extra pinned section):

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
`u_strlen`, `u_puts`, `u_putnum` and `u_exit`.

The build rule is a single Makefile target:

```sh
make user
```

`USER_PROGRAMS` is `hello badptr fault iso`. The common rule compiles
`user/*.c` with `-fcrug-kernel -ffreestanding -mcmodel=large -nostdlib -no-pie
-Wl,-T,user/user.ld` into `build/x86_64/user/<name>`; `iso` has an explicit rule
of its own because it links with `user/iso.ld` and
`ISO_LDFLAGS := -nostdlib -static -no-pie -Wl,--build-id=none
-Wl,-T,user/iso.ld`.

`scripts/mkdisk.sh` reads `USER_DIR` from the environment (the existing
raid/nvme/crypt call sites are unchanged) and writes each program into `/bin`
with `debugfs`. A missing program is a warning at disk-build time and a
"did not start" failure at run time.

## Markers

| Line | When | Meaning |
| --- | --- | --- |
| `CRUGOS: user selftest start` | `user.test=` boot | the kernel is about to run the programs |
| `CRUGOS: user hello from ring 3` | from `/bin/hello` | CPL3 code reached `SYS_WRITE` |
| `CRUGOS: user program /bin/hello finished (exit 0)` | after join | the first ring-3 program returned |
| `CRUGOS: user badptr blocked (EFAULT)` | from `/bin/badptr` | `SYS_WRITE` refused a kernel pointer |
| `CRUGOS: user program /bin/badptr finished (exit 0)` | after join | the pointer gate held |
| `CRUGOS: user fault writing above the window` | from `/bin/fault` | the faulting store is about to happen |
| `CRUGOS: user fault page fault at rip=... err=... tid N` | trap path | the fault was charged to the program |
| `CRUGOS: user fault killed tid N code -14` | trap path | only that thread died |
| `CRUGOS: user program /bin/fault finished (exit -14)` | after join | the kernel survived and saw the code |
| `CRUGOS: user isolation start` | `user.test=2` | the isolation pair is about to start |
| `CRUGOS: user isolation frames ... at 0x8000030000` | before the pair | the same VA has two different frames |
| `CRUGOS: iso tid N word 0x... ok` | from `/bin/iso` | an instance kept its own word |
| `CRUGOS: user isolation ok (2 program(s), 0 mismatch(es))` | end of pair | both instances kept their value |
| `CRUGOS: user selftest ok (3 program(s), 1 fault(s))` | end | the U1 programs behaved |

The counting line is unchanged: it counts the sequential U1 programs, not the
isolation pair. The crash report lines (`CRUGOS: trap report ...`) are
documented in [traps.md](traps.md) and appear between the fault line and the
kill line.

## The gate

`make test-user2` is gate 49 and the only gate that sets both `EXPECT_USER=yes`
and `EXPECT_USERS=yes`:

```sh
make test-user2
```

It rebuilds the ESP and the disk with all four programs (`disk` depends on
`user`), boots with `user.test=2` on the kernel command line under a
`USER2_RUN_TIMEOUT_SEC` (240) budget, then runs `scripts/boot-check.py` with the
new final argument `EXPECT_USERS` (`argv[28]`, with `EXPECT_USER` still at
`argv[27]`) plus `check-disk`. The argument-count guard is now
`tuple(range(3, 30))`. Every other gate keeps `EXPECT_USER=no` and
`EXPECT_USERS=no`, so boot-check fails a plain boot that prints any user or
isolation marker - the feature must be dormant unless the token is present.
`make test-user` (gate 48) stays as it was, running `user.test=1` with the three
U1 programs only.

Captured output from the gate in order (the report's `trap rip=` line and the two
`trap regs` lines are elided - they carry the same frame the `user fault` line
repeats):

```
CRUGOS: user selftest start
CRUGOS: user hello from ring 3
CRUGOS: user program /bin/hello finished (exit 0)
CRUGOS: user badptr blocked (EFAULT)
CRUGOS: user program /bin/badptr finished (exit 0)
CRUGOS: user fault writing above the window
CRUGOS: user fault page fault at rip=0x0000008000010054 err=0x0000000000000006 tid 10
CRUGOS: user fault killed tid 10 code -14
CRUGOS: user program /bin/fault finished (exit -14)
CRUGOS: user isolation start
CRUGOS: user isolation frames 0x28000 vs 0x38000 at 0x8000030000
CRUGOS: iso tid 11 word 0x15500b00 ok
CRUGOS: iso tid 12 word 0x15500c00 ok
CRUGOS: user isolation ok (2 program(s), 0 mismatch(es))
CRUGOS: user selftest ok (3 program(s), 1 fault(s))
CRUGOS: phase u2 boot ok
```

Between `user fault writing above the window` and the `user fault page fault`
line the report formatter prints the two `trap report`/`trap fault` lines quoted
in the fault section above (they are unchanged from U1: same vector, same error
bits, same `cr2`).

The boot check that accepts it:

```
ok: boot markers found, version 0.36.0 avx2 yes raid no nvme no rebuild no ahci no crypt no aes any net no tcp no dns no usb no mouse no audio no gfx no wb no menu no shell no keys no desk no icons no drawer no select no heap no user yes isolated yes storage disk QEMU exit status 33
```

The `user yes` and `isolated yes` fields are the user fields; all others are the
same as the H4 gate. The boot timing line is unchanged in shape:

```
boot   : pmm=28671 us cpu=418 us vm=708 us traps=346 us timer=65333 us heap=464 us storage=96362 us sched=300 us syscall=102 us total=192709 us
```

`check-disk` runs after the boot and reports the root filesystem intact:

```
ok: ext4 root is clean, /hello.txt and /selftest/b.txt persisted
```

Reading a failure:

- no `user selftest start` means the token did not reach the kernel (check the
  `CMDLINE_EXTRA` wiring) or the disk lacks the programs;
- `user selftest failed (/bin/... did not start)` means the loader refused the
  image - check the segment policy and the linker script;
- `(... exited X, expected Y)` names the exact mismatch: badptr returning 0
  instead of a clean `-EFAULT`, or fault not dying with -14;
- a missing `user isolation ...` line means `user.test=2` did not reach
  `user_test.c`; a `iso tid ... word ... mismatch` line means two programs saw
  each other's write - the per-space mapping or the CR3 switch is broken;
- a panic during the run means a fault was classified `TRAP_FATAL` - the panic
  line names the vector;
- an unexpected `CRUGOS: user ...` line on a non-user gate is itself a failure
  (boot-check requires the matching `EXPECT_*` flag).

## Limits

- Address-space isolation is between user programs, not between a program and
  the kernel: slot 0 is still the shared supervisor identity map in every space.
- The user window's intermediate page tables (the PML4 slot 1 PDPT, its PD and
  its PTs) are created once per space and released only at `vm_space_destroy`;
  `vm_space_unmap_user_pages` frees the leaf frame, not the tables.
- No `fork` or runtime `exec`: `elf_exec_run` is called by the kernel (selftest,
  future shell), not by a program, and one program per thread is still the rule.
- No `argv` or environment: the kernel enters at `_start` with an aligned stack
  and nothing else; the test programs take no arguments.
- No ring-3 preemption promises beyond the ordinary scheduler: the iretq enables
  interrupts before the first user instruction, and the timer path is the same as
  for kernel threads.
- SMAP/SMEP enforcement is code-present but the QEMU CPU the gate uses does not
  expose SMAP, so only the `stac`/`clac` pair is skipped on this host; the range
  check and the page walk always run, and a CPU with SMAP takes the same path
  with the access window opened around the copy.
- A user image is capped at `ELF_MAX_BYTES` (512 KiB) and the stack is four
  pages with no growth: a program that needs more has to change these constants
  first.
- The report formatter still caps a report at 1024 bytes (phase H3 limit); a very
  early ring-3 fault in a large frame could truncate its tail.
