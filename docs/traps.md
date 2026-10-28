# Trap policy and crash reports

Phase H3 replaces the way the kernel reacts to an exception. Before it, every
vector except `#BP` reached one shared handler that printed a single
`trap   : vector N (...) error=.. rip=..` line and panicked, and only the
double fault had a stack of its own. H3 turns that into a policy: a bounded,
uniform report, an explicit kernel-versus-user decision, and four IST stacks so
the handler still has somewhere to run when the fault is "the stack is gone".
Phase U1 then gave the policy a ring 3 to act on: a `DPL3` fault is charged to
the faulting thread and the machine keeps running, while a CPL0 frame still
panics.

The formatter and the classification are pure -- no `kprintf`, no serial port,
no framebuffer -- in `kernel/lib/trapfmt.c`, so the host suite can assert every
byte shape. The IDT glue stays in `kernel/arch/x86_64/idt.c`, the stacks are
reserved in `kernel/arch/x86_64/linker.ld`, and the fault injector is
`kernel/core/trap_test.c`.

## Why a trap policy

A kernel that panics on every exception is not wrong, but it is uninformative
and it cannot grow a ring 3. H3 asks three questions of every fault and answers
them the same way each time:

- **What happened?** One report shape carrying the vector, the error code, the
  IST the handler ran on, the saved frame and -- for a page fault only -- the
  decoded `CR2` address. No vector gets a bespoke one-liner.
- **Whose fault was it?** `trap_classify()` splits the vector space into
  handled, user-killable and fatal, using the frame's `cs` to tell a kernel
  frame from a CPL3 frame. Since phase U1 a user-killable fault from a `DPL3`
  frame kills only the faulting thread and the scheduler carries on; the same
  vector from a CPL0 frame is fatal.
- **Did the handler have a stack?** A double fault, NMI, machine check and
  debug exception are delivered on their own IST stack, so a report can be
  printed after the faulting stack has already been destroyed.

## The report contract

`kernel/arch/x86_64/idt.c` fills a `struct trap_report_info`:

```c
struct trap_report_info {
    u64 vector;
    u64 error;
    u32 ist;
    u64 cr2;
    int has_cr2;
    const char *name;
};
```

and passes it with the saved `trap_frame` to
`trap_format(const trap_frame *frame, const struct trap_report_info *info, char *out, usize cap)`.
The handler formats into a 1024-byte stack buffer, `console_write()`s the text
and only then runs the policy. The output is at most five lines, in this order:

```
CRUGOS: trap report <name> vector=<dec> error=0x<hex> ist=<dec> from=<kernel|user>
CRUGOS: trap rip=0x.. cs=0x.. rflags=0x.. rsp=0x.. ss=0x..
CRUGOS: trap fault present=<yes|no> write=.. user=.. reserved=.. instruction=.. cr2=0x..
CRUGOS: trap regs rax=0x.. rbx=0x.. rcx=0x.. rdx=0x.. rsi=0x.. rdi=0x.. rbp=0x..
CRUGOS: trap regs r8=0x.. r9=0x.. r10=0x.. r11=0x.. r12=0x.. r13=0x.. r14=0x.. r15=0x..
```

The rules are deliberately narrow:

- Hex is lowercase and **not** zero padded, so `0` prints as `0` and columns do
  not line up. The report is for humans reading one fault and for `grep`, not a
  table.
- `vector` and `ist` are decimal; `error` is hex even when it is zero.
- `from=kernel` when `cs & 3 == 0`, `from=user` when the frame came from
  CPL3. Phase U1 makes the second case real: a ring-3 program's fault prints
  `from=user` (the `make test-user` excerpt below shows one), while the fault
  injector and every kernel-frame fault still print `from=kernel`.
- The `fault` line exists **only** for vector 14 (`#PF`). `error` bit 0 is
  `present`, bit 1 `write`, bit 2 `user`, bit 3 `RSVD`, bit 4 `instruction`
  fetch, each printed as `yes`/`no` in that order, with `cr2` last.
- Everything is bounded by `cap`: `trap_format()` never writes past the end
  and stops at the buffer boundary rather than overrunning it.

A real divide-error boot -- `trap.test=de` -- is the reference sample:

```
CRUGOS: trap test de
CRUGOS: trap report divide error vector=0 error=0x0 ist=0 from=kernel
CRUGOS: trap rip=0x10509b cs=0x8 rflags=0x10246 rsp=0x1c0f70 ss=0x10
CRUGOS: trap regs rax=0x64 rbx=0x1de47000 rcx=0x1 rdx=0x0 rsi=0x12f693 rdi=0x136732 rbp=0x1c0f80
CRUGOS: trap regs r8=0x0 r9=0x1 r10=0x98 r11=0x1312e0 r12=0x1de47000 r13=0x1cb000 r14=0x1de47000 r15=0x1de49000
*** CRUGOS PANIC: divide error ***
CRUGOS: panic
```

A page fault adds the fault line between the frame and the register lines; this
excerpt keeps only the two lines that identify it:

```
CRUGOS: trap report page fault vector=14 error=0x2 ist=0 from=kernel
CRUGOS: trap fault present=no write=yes user=no reserved=no instruction=no cr2=0xffff800000000000
```

A ring-3 page fault has the same shape with `from=user`; the `user` bit in the
error code is set, and the report is followed by the U1 kill lines instead of a
panic. The excerpt keeps only the policy-relevant lines; the report's
`trap rip=` and two `trap regs` lines are elided:

```
CRUGOS: user fault writing above the window
CRUGOS: trap report page fault vector=14 error=0x6 ist=0 from=user
CRUGOS: trap fault present=no write=yes user=yes reserved=no instruction=no cr2=0x10000000000
CRUGOS: user fault page fault at rip=0x0000008000010064 err=0x0000000000000006 tid 10
CRUGOS: user fault killed tid 10 code -14
```

For a fatal fault -- every kernel-frame fault, and a CPL3 frame whose vector is
not handled and not user-killable -- the report is followed by the ordinary
panic path:
`*** CRUGOS PANIC: <name> ***`, the stack scan and `CRUGOS: panic`. The handler
then writes `0x11` to `isa-debug-exit`, so QEMU exits with status 35. A
`TRAP_USER` fault takes the phase U1 kill path instead: the report, the two
`CRUGOS: user fault ...` lines and no panic, so the boot continues and still
exits 33.

## The classification matrix

`include/crug/trap.h` keeps the `trap_frame` layout and adds the policy:

```c
enum trap_class { TRAP_HANDLED = 0, TRAP_USER = 1, TRAP_FATAL = 2 };
enum trap_class trap_classify(u64 vector, u64 cs);
```

`trap_classify()` reads the vector and the frame's code selector, and nothing
else -- it cannot be confused by a bad stack or a stale global:

| Vector | Name | Kernel frame | User frame |
| --- | --- | --- | --- |
| 0 | `divide error` | `TRAP_FATAL` | `TRAP_USER` |
| 1 | `debug` | `TRAP_FATAL` | `TRAP_USER` |
| 2 | `NMI` | `TRAP_FATAL` | `TRAP_FATAL` |
| 3 | breakpoint | `TRAP_HANDLED` | `TRAP_HANDLED` |
| 4 | overflow | `TRAP_FATAL` | `TRAP_HANDLED` |
| 5 | bound range exceeded | `TRAP_FATAL` | `TRAP_FATAL` |
| 6 | `invalid opcode` | `TRAP_FATAL` | `TRAP_USER` |
| 7 | device not available | `TRAP_FATAL` | `TRAP_FATAL` |
| 8 | `double fault` | `TRAP_FATAL` | `TRAP_FATAL` |
| 9 | coprocessor segment overrun | `TRAP_FATAL` | `TRAP_FATAL` |
| 10 | invalid TSS | `TRAP_FATAL` | `TRAP_FATAL` |
| 11 | segment not present | `TRAP_FATAL` | `TRAP_USER` |
| 12 | stack-segment fault | `TRAP_FATAL` | `TRAP_USER` |
| 13 | `general protection` | `TRAP_FATAL` | `TRAP_USER` |
| 14 | `page fault` | `TRAP_FATAL` | `TRAP_USER` |
| 15 | reserved | `TRAP_FATAL` | `TRAP_FATAL` |
| 16 | x87 floating point | `TRAP_FATAL` | `TRAP_USER` |
| 17 | alignment check | `TRAP_FATAL` | `TRAP_USER` |
| 18 | `machine check` | `TRAP_FATAL` | `TRAP_FATAL` |
| 19 | SIMD floating point | `TRAP_FATAL` | `TRAP_USER` |
| 20 | virtualization | `TRAP_FATAL` | `TRAP_FATAL` |
| 21 | control protection | `TRAP_FATAL` | `TRAP_USER` |
| 22-31 | reserved | `TRAP_FATAL` | `TRAP_FATAL` |
| 33-127, 129-255 | spurious / reserved | `TRAP_FATAL` | `TRAP_FATAL` |

The table reads as one sentence in the source: `#BP` is handled in both rings,
`#OF` is handled only for a user frame, the user-killable set
{0, 1, 6, 11, 12, 13, 14, 16, 17, 19, 21} is `TRAP_USER` when `cs & 3 == 3`
and fatal otherwise, and everything else is fatal in both rings.

Three vectors never reach `trap_classify()` in a running kernel: 32 is the
LAPIC timer tick, 128 is the `int 0x80` syscall gate, and `#BP` (3) returns from
its own branch before the report path -- it sets `g_int3_seen`, prints
`trap   : breakpoint handled at rip=...` and returns, which is what the
breakpoint self-test asserts. `trap_classify(3, ...)` is still exercised on the
host. That one legacy line is the only `trap   :`-prefixed output left in the
kernel.

`TRAP_USER` prints the report and then ends only the offending thread:
`kernel/arch/x86_64/idt.c` prints `CRUGOS: user fault <name> at rip=... err=...
tid N`, then `CRUGOS: user fault killed tid N code -<vector>` with the negated
vector as the thread's exit code, and calls `thread_kill_current()`; the
scheduler resumes and `thread_join_code()` hands the code to the parent. The H3
placeholder, `panic("user-mode <name>")`, is gone. The exception is a user-frame
`#DF`, `#NMI` or `#MC`: those are `TRAP_FATAL`, there is no healthy thread to
blame, and they still panic.

Phase U4 does not change this policy. `SYS_EXEC` replaces the calling image in
place - it rewrites the saved trap frame's `rip` and `rsp` - but the thread and
its tid stay the same, so a fault taken after an exec is reported against the
same thread id, and the kill path above ends that thread exactly as it does for
any other ring-3 program.

## IST stacks

A fault that arrives while the CPU is on a broken stack cannot be reported on
that stack. The four vectors that most often mean "the stack is the problem"
are delivered on dedicated IST stacks:

| Vector | IST | Stack symbol | Size |
| --- | --- | --- | --- |
| 8 `#DF` | `TRAP_IST_DF` (1) | `__df_stack` | 16 KiB |
| 2 `NMI` | `TRAP_IST_NMI` (2) | `__nmi_stack` | 8 KiB |
| 18 `#MC` | `TRAP_IST_MC` (3) | `__mc_stack` | 8 KiB |
| 1 `#DB` | `TRAP_IST_DB` (4) | `__db_stack` | 8 KiB |
| everything else | 0 | the current stack | -- |

`trap_ist_for(u64 vector)` returns that number and nothing else. The stacks
are reserved in `.bss` directly after the 32 KiB boot stack,
`kernel/arch/x86_64/cpu.c` loads them into `ist1`..`ist4` of the TSS, and the
service adds 32 KiB to the kernel image's bss overall.

`__df_stack` grew from 8 KiB to 16 KiB in H3. A double fault is the one vector
that fires when the CPU could not deliver some other exception, which usually
means the stack pointer is already bad; the handler then formats a 1024-byte
report, prints it and runs the panic stack scan. Sixteen kilobytes leaves that
work room without depending on the stack that just failed. The other three stay
at 8 KiB.

Phase H3 left `rsp0` pointing at the boot stack because the kernel never
`iretq`ed to CPL3. Phase U1 makes it per-thread: `cpu_set_kernel_stack()` is the
only writer of the field, and `schedule()` installs the incoming thread's kernel
stack top -- the thread's own kernel stack for a `THREAD_FLAG_USER` thread, the
boot stack top (`cpu_boot_stack_top()`) for a kernel thread -- so a fault taken
at CPL3 always lands on a valid kernel stack.

## Injecting faults from the command line

The gate cannot wait for a real bug, so H3 adds a deterministic injector.
`kernel/core/trap_test.c` parses one loader token and `kernel/core/main.c`
calls `trap_cmdline(bi)` right after `heap_cmdline(bi)`, then calls
`trap_test_run()` immediately before `arch_exit_qemu(0x10)`:

```
trap.test=<recipe>
```

Because the injector runs last, the boot has already printed the phase marker,
`CRUGOS: selftest ok` and the `K:exit-ok` trace when the fault lands. A control
boot with no token therefore exits 33, and only a fault turns the same boot into
exit 35. The loader copies `cmdline.txt` verbatim, so a leading space and a
trailing newline are part of the token stream; the parser treats space, tab, LF
and CR as separators, which is why a file written by `echo` still parses.

```
make run CMDLINE_EXTRA='trap.test=pf'
```

The recipe value is limited to 16 characters. An unknown value prints
`CRUGOS: trap test unknown <value>` and injects nothing -- it does not fall
back to a default fault.

| Recipe | What it injects | Vector | IST |
| --- | --- | --- | --- |
| `de` | `volatile u64 zero = 0; 100 / zero` | 0 divide error | 0 |
| `ud` | `ud2` | 6 invalid opcode | 0 |
| `gp` | load `ds` with selector `0x3b` (index 7), outside the seven-entry GDT | 13 general protection | 0 |
| `pf` | store to `0xffff800000000000`: high canonical, unmapped, `error=0x2` | 14 page fault | 0 |
| `df` | `movabsq $0x0000800000000000, %rsp` then `pushq $0` | 8 double fault | 1 |
| `nmi` | `int $2` | 2 NMI | 2 |
| `mc` | `int $18` | 18 machine check | 3 |
| `db` | `int $1` | 1 debug | 4 |

The `df` recipe is the interesting one: `0x0000800000000000` is the first
non-canonical address (the bottom of the canonical hole), so the stack pointer
itself is non-canonical. The `pushq` then raises `#GP`, delivering that `#GP`
faults again, and the CPU escalates to a real `#DF` on IST1:

```
CRUGOS: trap report double fault vector=8 error=0x0 ist=1 from=kernel
CRUGOS: trap rip=0x0 cs=0x8 rflags=0x10246 rsp=0x800000000000 ss=0x10
```

The `gp` row moved in phase U1. The recipe used to load `%ds` with `0x33`, which
was outside the five-entry GDT in H3; U1 turned index 6 (`0x33`) into the ring-3
data descriptor, so `0x33` is now a valid selector and the recipe has to stay
one slot further out. `0x3b` is index 7 with `RPL3`, past the seven-entry GDT, so
the %ds load still faults -- the fix is the selector, not the policy.

`int $N` for `nmi`, `mc` and `db` goes through the IDT gate like a software
interrupt, so the handler sees the vector and the IST routing is exercised even
though no hardware device raised the condition. Each hostile boot also prints
`CRUGOS: trap test <recipe>` before the report, so a missing injection is
visible in the log instead of being inferred from the exit code.

## The gate

`make test-trap` is the 46th gate. It runs
[`scripts/run-trap.py`](../scripts/run-trap.py), which boots nine crafted
configurations -- the control plus the eight recipes -- from per-case disk copies
under `build/x86_64/trap/`, each with `QEMU_DISPLAY=none`, `NET_MODE=none` and a
60-second budget. The eight hostile boots pass `CMDLINE_EXTRA='trap.test=<recipe>'`;
the control boot passes no token at all.

| Variable | Default | Meaning |
| --- | --- | --- |
| `TRAP_TIMEOUT_SEC` | `60` | wall-clock budget for each of the nine boots |

The control must exit 33 with the phase marker, `CRUGOS: selftest ok` and no
panic. Each hostile case must print `CRUGOS: trap test <recipe>`, the report
line's vector, IST and `from=kernel` fragments, `CRUGOS PANIC: <name>` and
`CRUGOS: panic`, and must exit 35. No case may print
`CRUGOS: trap test unknown` or `CRUGOS: trap test survived`.

A green run ends like this:

```
ok: trap control boot (exit 33)
ok: trap de -> divide error (ist=0, exit 35)
ok: trap ud -> invalid opcode (ist=0, exit 35)
ok: trap gp -> general protection (ist=0, exit 35)
ok: trap pf -> page fault (ist=0, exit 35)
ok: trap df -> double fault (ist=1, exit 35)
ok: trap nmi -> NMI (ist=2, exit 35)
ok: trap mc -> machine check (ist=3, exit 35)
ok: trap db -> debug (ist=4, exit 35)
ok: trap policy reported 8 crafted fault(s) (control boot exit 33)
```

Reading a failure:

- **exit 33 where the case expects 35** means the injection did not happen: the
  token was not parsed, the recipe name was mangled, or `trap_test_run()` ran
  before the recipe took effect.
- **A missing exit file, or exit 124** is a hang: the panic path never reached
  `arch_exit_qemu`, usually because the handler faulted again and never printed
  its report.
- **A report with the wrong vector or IST** points at the recipe or the routing:
  for example `df` arriving with `ist=0`, or `mc` on IST2.
- **`trap test survived`** means the handler returned to the faulting
  instruction instead of panicking.
- **`trap test unknown <value>`** is a command-line problem, not a trap
  problem: check the token spelling and the 16-character limit.

The same format rules are checked on the host. `tests/host/test_trap.c` feeds
every `trap_classify()` and `trap_ist_for()` input and every report shape
through `trap_format()`, including the `#PF`-only fault line, the
no-zero-padding rule and a cap too small for the full text.

## What is not covered

- The window is private per thread but slot 0 is shared: phase U2 gives every
  ring-3 thread its own PML4, so two user programs run at once and the same
  virtual address maps to different frames, but the kernel identity map is
  common to every space. That is isolation between user programs, not against
  the kernel.
- Window page tables are created once per space and released only at destroy:
  `vm_space_unmap_user_pages()` frees the leaf frame, not the intermediate PTs
  and PD, so a program that maps and unmaps in a loop keeps its tables.
- A user-frame `#DF`, `#NMI` or `#MC` still panics. Those vectors are fatal in
  both rings by design, and there is no policy for abandoning a whole address
  space, so the machine still stops rather than killing the thread.
- The kill path has no unwinding or per-thread report tail: the report and the
  two `user fault` lines go to the console, the thread is destroyed, and its
  user pages are unmapped without a symbolized backtrace of the faulting
  program.
- There is no stack unwinding. The panic path scans the stack for return
  addresses and prints raw frames; the report carries `rip`, `rsp` and the
  registers, not a symbolized backtrace.
- The IST stacks have no guard pages beneath them. They sit in `.bss` next to
  ordinary kernel data, so an IST stack overflow corrupts whatever is adjacent
  instead of faulting.
- There is no `#MC` recovery or thresholding: `int $18` exercises the delivery
  path, but there is no MCE bank walk, no `MCG_CAP` inspection and no policy
  for a corrected versus uncorrected error.
- There is no fault-injection fuzzer. The eight recipes are fixed and each boot
  injects exactly one fault; nothing mutates a frame, an address range or a
  sequence of faults.
- The report is bounded at 1024 bytes and ends silently at the cap. The five
  current lines fit with room to spare, but a future field that grows the text
  would be truncated without a marker.
