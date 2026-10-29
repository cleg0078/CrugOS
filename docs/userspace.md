# Ring 3 and the per-process address space

Phase U1 is the "protected userspace" milestone the specification deferred until
the kernel could afford one: a DPL3 segment pair, an address-space window that
belongs to user code, a copy boundary on every syscall pointer, and a fault
policy that charges a ring-3 fault to the program instead of the kernel. Phase
U2 then turned the single shared window into one PML4 per ring-3 thread, so two
programs run at the same time with the same virtual address pointing at
different frames. Phase U3 puts a real shell in ring 3: new file syscalls reach
the ext4 root, the kernel passes `argc`/`argv` on the initial stack, and
`/bin/sh` reads a script from the disk and writes a file back. Phase U4 adds a
process model to ring 3: `SYS_SPAWN` starts a program image as a child thread,
`SYS_WAIT` blocks for its exit code, and `SYS_EXEC` replaces the calling image
in place while keeping its tid. Phase U5 adds the other half of the process
model: `SYS_FORK` copies the caller's whole user window into a new address
space and resumes the child immediately after the `int 0x80`, so a program can
duplicate itself and the two copies stop sharing memory. Phase U6 makes that
copy lazy: the parent's writable frames are shared read-only and copied on the
first store on either side, so a fork costs a page table and a few reference
counts rather than one frame per page.

The current gate is `make test-user6` (gate 53): it boots with `user.test=6`,
runs the U1 trio, the U2 isolation pair, the U3 shell, the U4 spawn/exec probe,
the U5 two-page fork and then `/bin/forkbig`, which forks 300 pages and proves
the two sides never share a writable frame. The earlier gates keep their levels:
`make test-user5` (gate 52) stops after the two-page fork, `make test-user4`
(gate 51) after the spawn/exec probe, `make test-user3` (gate 50) at the shell,
`make test-user2` (gate 49) at the isolation pair.

Nothing in these phases changes the boot paths, the disk image format or the
loaded program ABI the UEFI loader uses. The kernel gains `SYS_WRITE`, the file
syscalls, the spawn/wait/exec calls and `SYS_FORK` plus one syscall mode switch;
every non-user gate still boots with no user program at all.

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
8. When the caller has arguments, `elf_exec_run_args` copies the argv block into
   the top of the stack pages before the thread starts; the next section covers
   the layout. `elf_exec_run` is the `argc == 0` wrapper.

The entry point is checked by `harden_user_entry_ok`, and `thread_create_user`
checks the initial user stack pointer with the same validator. A program whose
entry or stack is outside the window simply fails to start, and the space it was
going to use is destroyed.

One bug worth remembering from landing U2: `vm_init` used to publish the root
only after mapping the kernel sections, but the section mapper walks the root, so
the boot page-faulted before the IDT existed. It now publishes
`g_root`/`vm_space_set_kernel_root` immediately after allocating the root page.

## Arguments on the user stack

`kernel/exec/argv.c` builds the initial stack block, and
`elf_exec_run_args(path, argc, argv, out)` is the entry point that uses it. The
block lives in a 4 KiB area (`EXEC_ARGS_BLOCK_MAX`) at the top of the four stack
pages and is 16-byte aligned; it holds the argument strings
(`EXEC_ARGS_STR_MAX` 2048 bytes in total), the `argv[]` array
(`EXEC_ARGS_MAX` 16 entries), the `envp[]` array, and the `argc` word.

The thread is started with `rsp` pointing at that block, so a user program sees:

| Register | Value |
| --- | --- |
| `rdi` | `argc` |
| `rsi` | `argv` |
| `rdx` | `envp` |

`user/start.S` loads the three values from the initial stack before it aligns
`rsp`, then calls `main(argc, argv, envp)`. `elf_exec_run(path, out)` is the old
`argc == 0` wrapper, so the U1 programs still start with an empty argument list.

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

`SYS_WRITE` (16) started as the console call: `fd` 1 or 2, the length clamped to
4096 bytes, and the kernel copies in 256-byte chunks into a stack buffer
(`SYS_BOUNCE_MAX` covers the larger message ports) and `kprintf`s each chunk. In
U3 the same call also writes to an open file when `fd >= 3`; the console
behaviour is unchanged. `SYS_MAX` is 25. `SYS_EXIT` (8) from CPL3 records the exit
code and ends only that thread; the kernel continues. `SYS_EBUSY` (-16) is the
error a caller gets when a thread cannot be created.

## File descriptors and the file syscalls

Phase U3 gives ring 3 a filesystem: four new calls in `kernel/core/syscall.c`
export the mounted ext4 root, and `include/crug/ufile.h` / `kernel/fs/ufile.c`
hold the per-thread descriptor table.

| nr | Call | Arguments | Returns |
| --- | --- | --- | --- |
| 17 | `SYS_OPEN` | `(path, flags, mode)` | a descriptor, or a negative error |
| 18 | `SYS_READ` | `(fd, buf, len)` | bytes read, 0 at end of file |
| 19 | `SYS_CLOSE` | `(fd)` | 0 |
| 20 | `SYS_LISTDIR` | `(path, buf, len)` | bytes written into the buffer |

Descriptors 0, 1 and 2 are permanent: 0 is stdin (`SYS_READ` returns 0, i.e.
EOF), 1 and 2 are the console for `SYS_WRITE`, and closing any of them is a no-op
that returns 0. Files start at `UFILE_FD_BASE` (3), and the descriptor is simply
`3 + slot` in a table with `UFILE_FD_MAX` (8) entries. The table is per thread:
`thread_destroy` calls `ufile_close_all`, and a descriptor opened in one thread
is not visible in another. `UFILE_PATH_MAX` is 64 bytes.

Open flags are the four values in `include/crug/syscall.h`:

| Flag | Value | Meaning |
| --- | --- | --- |
| `SYS_O_RDONLY` | 0 | read the file |
| `SYS_O_WRONLY` | 1 | open for writing (the file must exist) |
| `SYS_O_CREAT` | 2 | create the file if it is missing |
| `SYS_O_TRUNC` | 4 | truncate it |

Read-only opens snapshot the whole file into the heap at open time, so a later
`SYS_READ` never touches the filesystem: the read path copies from that buffer
and returns 0 once it is exhausted. `UFILE_MAX_BYTES` (64 KiB) caps both the
snapshot and the write buffer.

Writable opens buffer in the heap and flush on `SYS_CLOSE`:

- when `SYS_O_CREAT` actually created the file, the flush creates it;
- when bytes were buffered, the flush writes the whole buffer from offset 0;
- otherwise the flush truncates the file to zero bytes;
- the file is fsynced last.

The flush brackets the filesystem with an optional `begin_write`/`end_write`
hook. On the ext4 backend those hooks are `storage_remount_rw()` and
`storage_remount_ro()`, because the boot mounts the root read-only and only a
pending write should remount it; the serial log shows
`CRUGOS: storage remounted read-write` and then
`CRUGOS: storage remounted read-only` around the write.

`SYS_LISTDIR` copies newline-separated names into the user buffer and NUL
terminates the result, so a user program can list `/` without knowing the
on-disk directory format. `SYS_MAX` is 25, and any number at or above it returns
`SYS_ENOSYS`. `SYS_ENOENT` (-2) is the new error for a path that does not exist.

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
  CRUGOS: user fault page fault at rip=0x0000008000010064 err=0x0000000000000006 tid 10
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
run one after another; the U2 isolation pair runs at the same time; the U3 shell
runs on its own, after the pair has been joined; and the U4 spawn test runs
after the shell, because it starts and reaps a program of its own; the U5 fork
probe runs last, because its child must be reaped before the boot ends.

| Program | What it proves | Expected exit |
| --- | --- | --- |
| `/bin/hello` | a ring-3 program can run, call `SYS_WRITE` and return | 0 |
| `/bin/badptr` | a kernel pointer is refused with `-EFAULT`, not dereferenced | 0 |
| `/bin/fault` | a store above the window kills only the program | -14 |
| `/bin/iso` | two instances at the same VA see different frames | 0 |
| `/bin/sh` | a user program reads a script from ext4 and writes a file back | 7 |
| `/bin/spawn` | spawn, wait and in-place exec all work from ring 3 | 5 |
| `/bin/child` | a spawned image receives its argv and exits for its parent | 5 |
| `/bin/fork` | a forked child writes its own frame, not the parent's | 0 |

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

`spawn.c` starts `/bin/child` with `("/bin/child", "from-spawn")`, prints the
returned tid, waits for exit 5 and checks the code, then execs `/bin/child` with
`from-exec`, so one program proves both the child path and the in-place
replacement. `child.c` prints its tid, `argc` and `argv[1]`, then returns 5.
A spawn failure makes spawn return 1, a wrong child code 2 and an exec failure 3,
so the gate can tell which step broke.

`fork.c` calls `SYS_FORK` and branches on the return value: 0 means it is the
child, which writes 8738 into the global and exits 3; anything else is the
parent, which waits for that tid, checks exit 3 and checks that its own global
is still 4369. It exits 0 when the copy is real and 1, 2 or 3 when the fork, the
wait or the memory check failed.

## The ring-3 shell

`user/sh.c` is `/bin/sh`, linked by `user/sh.ld` (text R+X at
`0x8000010000`, data R+W at `0x8000020000`, `ENTRY(_start)`). It is a real
program at CPL3: every file it touches goes through `SYS_OPEN`, `SYS_READ`,
`SYS_WRITE`, `SYS_CLOSE` and `SYS_LISTDIR`, and every character it prints goes
through `SYS_WRITE` on fd 1.

There is no user heap and no `malloc` in ring 3 yet: the shell keeps its line
buffer, path buffer and command buffer as static arrays inside its data segment.

With no argument the shell runs `/etc/sh.rc`; `argv[1]` overrides the script
path. It executes one command per line:

| Command | Effect |
| --- | --- |
| `ver` | print `CrugOS ring-3 shell (phase 6i)` |
| `echo TEXT` | print `TEXT` |
| `cat FILE` | read the file and write its bytes to the console |
| `ls PATH` | list a directory |
| `write FILE TEXT` | write `TEXT` plus a newline to `FILE` |
| `exit N` | stop with exit code `N` |

`scripts/mkdisk.sh` writes the shipped `/etc/sh.rc` into the disk image:

```sh
ver
echo hello from the user shell
cat /hello.txt
ls /
write /shout.txt hello-from-user-shell
cat /shout.txt
exit 7
```

That script is the write proof. `cat /hello.txt` reads 16 bytes that were placed
in the image at build time; `write /shout.txt hello-from-user-shell` creates a
new file through the syscall layer, and the two remount lines appear around the
flush; the second `cat` reads back the 22 bytes the shell wrote (21 characters
plus the newline that `write` appends). `exit 7` is the code `make test-user3`
expects, and `check-disk --expect-shell-write` checks the bytes on disk after a
clean umount and `e2fsck`.

## Starting another program: `SYS_SPAWN`

`SYS_SPAWN` (21) takes `(path, argc, argv)` and starts a new program image as
a new ring-3 thread, returning its tid. It is the first time a user program -
not the kernel - decides what runs next.

`elf_spawn_user(path, argc, argv, parent_id, out)` in `kernel/exec/elf_exec.c`
is the old `elf_exec_run` with two additions: it copies an `argv` block onto
the new stack, and it stores the caller's tid in the new `parent_id` field of
`struct thread`. `elf_exec_run(path, out)` remains the `argc == 0` wrapper and
`elf_exec_run_args(path, argc, argv, out)` passes parent id 0, which is how the
kernel-launched programs (selftest, shell) differ from children.

The child gets a fresh `vm_space`, a four-page stack and its own descriptor
table; it inherits nothing from the caller. `/bin/spawn` proves the path: it
prints `CRUGOS: user spawn start`, spawns `/bin/child` with
`{"/bin/child", "from-spawn"}`, prints its tid and waits for exit 5.

## Waiting: `SYS_WAIT`, `parent_id` and `thread_find`

`SYS_WAIT(tid)` (22) blocks until that child exits and returns its code. The
new `thread_find(id)` in `kernel/core/thread.c` walks a global all-threads
list held by the scheduler; the list keeps zombies, so a wait that arrives after
the child has already exited still finds the record and its stored exit code.
Ids are monotonic, so a stale tid never matches a recycled thread.

The kernel accepts the wait only when the target's `parent_id` is the caller's
tid and the target is not the caller; anything else returns the new
`SYS_ESRCH` (-3). A successful wait reaps the child through the existing
`thread_join_code()`, and only then is the thread removed.

## exec: replacing the image in place

`SYS_EXEC(path, argc, argv)` (23) does not start a thread: it replaces the
calling image. `elf_exec_in_place()` loads the ELF into a fresh `vm_space`,
points the current thread's `space`, `user_rip` and `user_rsp` at it, and
calls `vm_space_activate()` **before** destroying the old space, because CR3
still points at the old tables while the loader runs; destroying first would
free the page tables the CPU is walking.

Back in the syscall layer the saved trap frame is rewritten (`frame->rip`,
`frame->rsp` and `frame->rax = 0`) before the `iretq`, so the return from the
syscall lands at the new entry point with the new stack. The thread keeps its
tid and its kernel stack: `/bin/spawn` execs `/bin/child` with `from-exec`
and the log prints `CRUGOS: user exec /bin/child (tid 14)` on the same tid that
spawned the child. A CPL0 caller gets `SYS_EINVAL`, because the kernel has no
image to replace.

## Passing arguments across the syscall boundary

All three calls take a user `argv` array of pointers plus a count.
`syscall_argv()` in `kernel/core/syscall.c` turns that into a kernel copy:

- a CPL3 caller is range-checked with `uaccess_check` and copied with
  `uaccess_user_str`/`uaccess_user_read` into the static
  `g_argv_buf[EXEC_ARGS_STR_MAX]`, one bounded string at a time;
- a CPL0 caller keeps the identity-map path, `harden_range_ok` plus `memcpy`;
- both end with `exec_args_ok(argc, str_bytes)`, a pure predicate in
  `kernel/exec/argv.c` that rejects more than `EXEC_ARGS_MAX` (16) arguments
  or more than `EXEC_ARGS_STR_MAX` (2048) bytes of strings.

The arguments are enforced before the ELF is loaded, so an oversized list cannot
allocate a space. `exec_args_build()` then lays out the strings, `envp[]`,
`argv[]` and the `argc` word in a 4 KiB block at the top of the new user stack,
16-byte aligned, and `rsp` points at the `argc` word.

## Fork: the ABI and the trampoline

`SYS_FORK` (24, so `SYS_MAX` is 25) takes no arguments. In the parent it
returns the new child's tid; in the child it returns 0 and the first user
instruction is the one right after the `int 0x80`, exactly as if the call had
returned twice. The child is a new thread with its own `vm_space`, its own
kernel stack and its own descriptor table.

Phase U5 built the child's space with `vm_space_clone()` in
`kernel/mm/vm_space.c` by allocating a fresh frame and `memcpy`ing every user
page, under a `VM_SPACE_FORK_MAX_PAGES` cap of 256 pages. Phase U6 keeps the
same call and the same cap shape but shares the frames instead of copying them;
[Copy-on-write fork (U6)](#copy-on-write-fork-u6) below describes the mechanism,
and the cap is now 4096 pages.

The child does not enter through `thread_entry`. `thread_create_forked()`
keeps a copy of the parent's `trap_frame` in the thread (`THREAD_FLAG_FORKED`,
`fork_frame`) with `rax` set to 0, and starts the thread at
`kernel/arch/x86_64/user.S:thread_fork_enter`:

```asm
thread_fork_enter:
    cli
    call thread_fork_resume
    movq %rax, %rsp
    jmp  isr_return
```

`thread_fork_resume()` parks the saved frame `THREAD_FORK_FRAME_GAP` (1024)
bytes below the top of the child's kernel stack, restores `%ds`/`%es` to
`SEG_USER_DS`, calls `cpu_set_kernel_stack()` with the child's stack top and
returns the frame pointer in `%rax`. `kernel/arch/x86_64/isr.S` now exports
the label `isr_return` just before its `popq %r15`, so the trampoline shares
the normal interrupt epilogue (`pop` r15 down to rax, `addq $16`, `iretq`)
and returns to CPL3 through the same path as any interrupt.

The child's descriptor table is deliberately empty - `thread_create` calls
`ufile_table_init`. Inherited descriptors cannot be shared safely between two
threads, and an empty table guarantees a buffered file write is never flushed
twice.

`user/fork.c` was the U5 probe: it held `static volatile u64 g_word = 0x1111`
as a global, the child stored `0x2222` (8738) into its copy and called
`u_exit(3)`, and the parent `u_wait()`ed the tid, required exit code 3 and
re-read `g_word` for its own `0x1111` (4369). If the child had aliased the
parent's frame instead of receiving a copy, the parent would have read `0x2222`.
The program returned 0 on success and 1, 2 or 3 when the fork, the wait or the
word check failed. The U6 version of the probe adds a second page and a parent
store; [Copy-on-write fork (U6)](#copy-on-write-fork-u6) has its markers.

The U5 markers (gate 52) read:

```
CRUGOS: user fork test start
CRUGOS: user fork start
CRUGOS: user fork tid 16 -> 17 (6 page(s))
CRUGOS: user fork tid 17
CRUGOS: user fork child word 8738
CRUGOS: user fork reap exit 3
CRUGOS: user fork parent word 4369
CRUGOS: user fork ok (child exit 3, memory isolated)
CRUGOS: user fork test ok (child exit 3, memory isolated)
```

One trap is worth recording from U5. `vm_space_map_user_pages()` only ORs
`VM_USER` into the flag word it is given; it does not add `PRESENT`. The first
clone passed `VM_WRITE | VM_NX` and therefore built a child whose leaf was never
present - it ended with `dst->user_pages` 1 against the parent's 2,
`vm_space_translate()` returned 0 for the missing page and the host clone test
segfaulted afterwards. The eager clone had to pass
`VM_PRESENT | VM_WRITE | VM_NX`; the U6 clone maps a shared leaf through
`space_map_shared()` with the presence bit always set, which is the same lesson
in a different place.

## Copy-on-write fork (U6)

A fork that copies every page is expensive, and the U5 cap (256 pages) existed
only because each fork paid one frame per page. Phase U6 makes the copy lazy:
the child maps the parent's frames read-only and both sides take a private copy
on the first store. The cap rises to `VM_SPACE_FORK_MAX_PAGES` = 4096 pages,
and a fork of a 300-page program now costs a page table and a few reference
counts instead of 300 fresh frames.

### The `VM_COW` bit and the reference table

`include/crug/vm.h` adds `VM_COW`, a software PTE bit at `1ull << 9` that is
part of `VM_PTE_FLAG_MASK` (the hardware ignores it). A leaf carries `VM_COW`
when the frame behind it may be shared, and the bit is cleared once a side owns
its own copy.

`include/crug/cow.h` and `kernel/mm/cow.c` keep one reference count per shared
physical frame in a flat open-addressing table:

- `COW_TABLE_SLOTS` is 4096 entries, one per distinct shared frame, so the
  table also bounds how many pages one fork chain can hold together;
- the slot index is `(pa >> 12) * 2654435761` folded with `x ^= x >> 16`;
  collisions probe forward and a released entry becomes a tombstone so probe
  chains stay intact;
- `cow_share(pa)` registers one more owner, returning 0 on success and -1 when
  the table has no free slot or tombstone. The first share of a frame records
  two owners - the space that already maps it plus the caller;
- `cow_refs(pa)` reports the recorded owners, or 0 for a frame that was never
  shared;
- `cow_release(pa)` drops one owner and returns 1 when the caller is now the
  last owner and must free the frame, 0 when another address space still maps
  it. An untracked frame always reports 1, so a space that never forked frees
  exactly as before;
- `cow_reset()` and `cow_tracked()` reset and count the table. The module only
  needs `<crug/types.h>`, so it links into the host tests without CR3 or
  interrupts.

### What the clone does

`vm_space_clone()` is no longer const and no longer copies pages. It walks the
parent's slot-1 page tables exactly as before, but for every present leaf it:

- calls `cow_share(pa)`, so every shared frame - including read-only text - has
  an entry in the table;
- maps the parent's frame into the child read-only with
  `space_map_shared()`, copying the parent's `PRESENT`/`USER`/`NX`/`PWT`/`PCD`
  bits and adding `VM_COW` when the leaf is writable or was already `VM_COW`
  (a nested fork must not hand out a read-only leaf that can never fault);
- when the leaf was writable, rewrites the parent's PTE without `VM_WRITE` and
  with `VM_COW`, so the frame is never writable on both sides at once.

A read-only text leaf is shared as a plain read-only mapping: it never takes
`VM_COW` and never faults, but it is still entered in the refcount table, so
the frame is freed only when the last space that maps it is destroyed. A
`cow_share()` that returns -1 (a full table) destroys the half-built child and
fails the fork with `ENOMEM` rather than falling back to an eager copy. Any
mapping failure does the same, and a parent above the 4096-page cap is refused
before anything is built.

`kernel/mm/vm.c` tracks the active space beside the active page table, and
`vm_active_space()` returns it after `vm_space_activate()` installed it, so the
fault path and the copy-out path can find the space a user address belongs to.

### The fault taxonomy

`vm_space_cow_fault(vm_space *s, u64 va)` resolves one leaf and returns:

- 0 - there is nothing to resolve: the space or the PTE is absent, or the leaf
  is present without `VM_COW`;
- 1 - the frame was copied: `cow_refs()` was above one, a fresh frame was
  allocated, `memcpy`ed and mapped writable without `VM_COW`, and the old
  frame was released (and freed if that release was the last);
- 2 - the caller was the sole owner: `cow_refs()` was at most one, so the leaf
  is repaired in place with its write bit back rather than copying a page that
  would stay identical - this is the path a parent takes after its child has
  exited;
- -1 - the fresh frame could not be allocated.

Two callers resolve leaves. The user page-fault path in
`kernel/arch/x86_64/idt.c` runs before the ordinary trap report: for vector 14
with the error code's U/S and W/R bits set and `trap_classify()` =
`TRAP_USER`, it calls `vm_space_cow_fault()` on the current thread's space, and
a positive answer flushes the page and returns to CPL3, printing

```
CRUGOS: user cow fault tid 16 va=0x000000803fffffc8 (copy)
CRUGOS: user cow fault tid 16 va=0x0000008000011000 (sole owner)
```

A 0 or -1 falls through to the normal report and, for a user frame, the
kill-only-that-thread path. The other caller is
`kernel/mm/uaccess.c:uaccess_pages_writable()`, which resolves `VM_COW` leaves
(a `vm_space_cow_fault()` plus `vm_flush_page()`) before a kernel copy-out
writes through a user pointer, so a `SYS_WRITE`-style boundary crossing never
stores into a still-shared frame. `uaccess_user_read()` keeps the plain mapped
check because a read cannot break a share.

After the child exists, `SYS_FORK` calls `vm_flush_all()`: the parent's TLB may
still cache the pre-fork writable leaf, and a stale entry would let the parent
store without faulting. The fork then prints its shared-page count:

```
CRUGOS: user fork tid 16 -> 17 (7 page(s) shared, COW)
```

### The probes

`user/fork.c` now holds two data pages, `g_word` (`0x1111`) and `g_word2`
(`0x3333`). The child writes `0x2222` and `0x5555` (8738, 21845) into its copies
and exits 3; the parent reaps it and must still read `0x1111` (4369) and
`0x3333` (13107). It then stores `0x4444` (17476) into the first page - the
child has exited, so `cow_refs()` is one and that store takes the sole-owner
path - and prints `CRUGOS: user fork parent write 17476`. The success marker is
`CRUGOS: user fork ok (child exit 3, memory isolated, 2 COW page(s))`.

`user/forkbig.c` is the large-fork probe: it maps 300 pages (1.2 MiB) of
`.bss`, sets the first byte to `0xA5`, forks, and the child writes `0x5A` to the
first and last byte and exits 7. The parent reaps exit 7, checks that its
sentinel survived and that its last byte is still 0, then prints
`CRUGOS: user forkbig ok (child exit 7, 300 page(s) isolated)`. Levels other
than `user.test=6` never run it, and `SYS_FORK` does not grow with the image:
the cost is one PTE update per page plus one table entry per distinct frame.

`user.test=6` adds the large-fork probe after the two-page one;
`scripts/mkdisk.sh` installs `/bin/forkbig`, so the disk carries nine programs.

The gate-53 fork markers in order:

```
CRUGOS: user fork test start
CRUGOS: user fork start
CRUGOS: user fork tid 16 -> 17 (7 page(s) shared, COW)
CRUGOS: user cow fault tid 16 va=0x000000803fffffc8 (copy)
CRUGOS: user cow fault tid 17 va=0x0000008000011000 (copy)
CRUGOS: user fork tid 17
CRUGOS: user fork child word 8738
CRUGOS: user fork reap exit 3
CRUGOS: user cow fault tid 16 va=0x0000008000011000 (sole owner)
CRUGOS: user fork parent word 4369
CRUGOS: user fork parent write 17476
CRUGOS: user fork ok (child exit 3, memory isolated, 2 COW page(s))
CRUGOS: user fork test ok (child exit 3, memory isolated)
CRUGOS: user forkbig start
CRUGOS: user forkbig tid 16
CRUGOS: user forkbig child tid 17
CRUGOS: user cow fault tid 17 va=0x... (copy)
CRUGOS: user forkbig reap exit 7
CRUGOS: user forkbig ok (child exit 7, 300 page(s) isolated)
```

The exact tid and address values vary by run (the forkbig block's fault line is
shown with a placeholder address for that reason); boot-check only requires the
shape: a shared count of at least 2, at least two `(copy)` faults and one
`(sole owner)` fault for the fork block, and at least 300 shared pages plus one
`(copy)` fault for the forkbig block.

### What U6 does not do

- There is no copy-on-write `memcpy` avoidance beyond the first store: each
  written page is copied once, and a page never written is never copied.
- The 4096-entry table is a hard bound. A fork that needs more distinct shared
  frames is refused with `ENOMEM`; it does not fall back to eager copying.
- The 4096-page fork cap still applies, and there is no swap or page cache
  behind a share.
- A fork still inherits only the user window: no descriptors, no pending
  signals, no timers - the child starts with an empty descriptor table.

## Building a user program

A user program is a freestanding ELF64 linked by `user/user.ld`. The isolation
program uses `user/iso.ld` because it needs the extra pinned section, and the
shell uses `user/sh.ld` because it wants its data segment at a fixed address:

| Rule | Value | Why |
| --- | --- | --- |
| entry | `_start` in `user/start.S` | the kernel iretqs to the ELF entry |
| base | `0x0000008000010000` | inside the window, one page above `USER_BASE` |
| text | `PT_LOAD` R+X | executable, never writable |
| data | `PT_LOAD` R+W, `VM_NX` | writable, never executable |
| segments | page aligned | the loader maps whole pages and refuses unaligned p_vaddr |

`start.S` reads `argc`, `argv` and `envp` from the initial stack, aligns the
stack, calls `main`, and turns main's return value into `SYS_EXIT`. It cannot use
`cli` or `hlt`, which are privileged. `user/lib.h` is the whole runtime: inline
`int $0x80` wrappers (`nr` in `rax`, arguments in `rdi`, `rsi`, `rdx`, then
`r10`), plus `u_strlen`, `u_puts`, `u_putnum` and `u_exit`.

The build rule is a single Makefile target:

```sh
make user
```

`USER_PROGRAMS` is `hello badptr fault iso sh spawn child fork forkbig`. The common rule
compiles `user/*.c` with `-fcrug-kernel -ffreestanding -mcmodel=large -nostdlib
-no-pie -Wl,-T,user/user.ld` into `build/x86_64/user/<name>`; `iso` and `sh` have
explicit rules of their own because they link with `user/iso.ld` and
`user/sh.ld` (`ISO_LDFLAGS := -nostdlib -static -no-pie -Wl,--build-id=none
-Wl,-T,user/iso.ld`).

`scripts/mkdisk.sh` reads `USER_DIR` from the environment (the existing
raid/nvme/crypt call sites are unchanged) and writes each program into `/bin`
with `debugfs`, plus the shipped `/etc/sh.rc` script. A missing program is a
warning at disk-build time and a "did not start" failure at run time.

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
| `CRUGOS: user shell start` | `user.test=3` | the shell is about to run |
| `CRUGOS: sh start (argc 2)` | from `/bin/sh` | `argv` arrived on the user stack |
| `CRUGOS: sh cmd <name>` | from `/bin/sh` | one line of the script |
| `CRUGOS: sh cat <path> <n> byte(s)` | from `/bin/sh` | `SYS_READ` returned `n` bytes |
| `CRUGOS: sh write <path> <n> byte(s)` | from `/bin/sh` | the write buffer was flushed |
| `CRUGOS: sh exit <n>` | from `/bin/sh` | the script asked for exit code `n` |
| `CRUGOS: user shell finished (exit <n>)` | after join | the shell thread is gone |
| `CRUGOS: storage remounted read-write` | kernel write flush | the root was remounted for a pending write |
| `CRUGOS: storage remounted read-only` | kernel write flush | back to the read-only mount |
| `CRUGOS: user spawn test start` | `user.test=4` | the kernel is about to run `/bin/spawn` |
| `CRUGOS: user spawn start` | from `/bin/spawn` | before `SYS_SPAWN` |
| `CRUGOS: user spawn tid <n>` | from `/bin/spawn` | `SYS_SPAWN` returned the child tid |
| `CRUGOS: user child running (tid <n>)` | from `/bin/child` | a new image reached ring 3 |
| `CRUGOS: user child argc <n>` | from `/bin/child` | the argv block arrived |
| `CRUGOS: user child arg <text>` | from `/bin/child` | `argv[1]` is visible |
| `CRUGOS: user spawn child exit <n>` | from `/bin/spawn` | `SYS_WAIT` returned the code |
| `CRUGOS: user spawn exec` | from `/bin/spawn` | it is about to replace its own image |
| `CRUGOS: user exec <path> (tid <n>)` | kernel syscall path | `SYS_EXEC` replaced the image in the same thread |
| `CRUGOS: user spawn test ok (child exit 5, exec exit 5)` | end of level 4 | both the child and the exec path behaved |
| `CRUGOS: user fork test start` | `user.test=5` | the kernel is about to run `/bin/fork` |
| `CRUGOS: user fork start` | from `/bin/fork` | before `SYS_FORK` |
| `CRUGOS: user fork tid <n> -> <m> (N page(s) shared, COW)` | kernel `SYS_FORK` path | parent tid, child tid and how many frames the two spaces share |
| `CRUGOS: user cow fault tid <n> va=<va> (copy)` | first store to a shared page | the frame was copied and the store retried |
| `CRUGOS: user cow fault tid <n> va=<va> (sole owner)` | store to a page nothing else maps | the leaf just regained `WRITE` |
| `CRUGOS: user fork tid <n>` | from `/bin/fork` | the parent prints the child tid `SYS_FORK` returned |
| `CRUGOS: user fork child word <n>` | from `/bin/fork` | the child's copy holds 8738 |
| `CRUGOS: user fork reap exit <n>` | from `/bin/fork` | `SYS_WAIT` returned the child's code |
| `CRUGOS: user fork parent word <n>` | from `/bin/fork` | the parent still reads its own 4369 |
| `CRUGOS: user fork parent write <n>` | from `/bin/fork` | the sole-owner store printed 17476 |
| `CRUGOS: user fork ok (child exit 3, memory isolated, 2 COW page(s))` | from `/bin/fork` | the child's writes did not reach the parent |
| `CRUGOS: user fork test ok (child exit 3, memory isolated)` | end of level 5 | the whole two-page fork probe behaved |
| `CRUGOS: user forkbig start` | from `/bin/forkbig` | before `SYS_FORK` |
| `CRUGOS: user forkbig tid <n>` | from `/bin/forkbig` | the parent's tid before the fork |
| `CRUGOS: user forkbig child tid <n>` | from `/bin/forkbig` | the child is alive with its own copy |
| `CRUGOS: user forkbig reap exit <n>` | from `/bin/forkbig` | `SYS_WAIT` returned 7 |
| `CRUGOS: user forkbig ok (child exit 7, 300 page(s) isolated)` | end of level 6 | the sentinel survived the child's writes |
| `CRUGOS: user selftest ok (3 program(s), 1 fault(s))` | end | the U1 programs behaved |

The counting line is unchanged: it counts the sequential U1 programs, not the
isolation pair, the shell, the spawn test, the fork test or the large-fork test.
The crash report lines
(`CRUGOS: trap report ...`) are documented in [traps.md](traps.md) and appear
between `user fault writing above the window` and the
`user fault page fault at rip=...` line.

## The gate

`make test-user6` is gate 53 and the only gate that sets `EXPECT_FORKBIG=yes`;
it needs the five earlier user flags as well:

```sh
make test-user6
```

It rebuilds the ESP and the disk with all nine programs (`disk` depends on
`user`), boots with `user.test=6` on the kernel command line under a
`FORKBIG_RUN_TIMEOUT_SEC` (300) budget, then runs `scripts/boot-check.py` with
the new final argument `EXPECT_FORKBIG` (`argv[32]`; `EXPECT_USER` is
`argv[27]`, `EXPECT_USERS` is `argv[28]`, `EXPECT_USHELL` is `argv[29]`,
`EXPECT_SPAWN` is `argv[30]` and `EXPECT_FORK` is `argv[31]`). The
argument-count guard is now `if len(sys.argv) < 3 or len(sys.argv) > 33:`. The
gate then runs the plain `check-disk` pass: it does not pass
`--expect-shell-write`, so its check-disk line has no `/shout.txt` clause, and
that write proof stays with gate 50. Every other gate keeps all six flags at
`no`, so boot-check fails a plain boot that prints any user, isolation, shell,
spawn, fork or forkbig marker - the feature must be dormant unless the token is
present.

`make test-user` (gate 48), `make test-user2` (gate 49), `make test-user3`
(gate 50), `make test-user4` (gate 51) and `make test-user5` (gate 52) still run
their own levels: `user.test=1` is the three U1 programs, `user.test=2` adds the
isolation pair, `user.test=3` adds the shell, `user.test=4` adds `/bin/spawn`,
`user.test=5` adds `/bin/fork`, and `user.test=6` adds `/bin/forkbig` on top
of the two-page fork.

Captured output from the gate in order (the report's `trap rip=` line and the two
`trap regs` lines are elided - they carry the same frame the `user fault` line
repeats; the first `...` stands for the U1-U3 markers, unchanged from gate 50,
and the second for the U4 spawn/exec stage, unchanged from gate 51):

```
CRUGOS: user selftest start
...
CRUGOS: user spawn test start
...
CRUGOS: user spawn test ok (child exit 5, exec exit 5)
CRUGOS: user fork test start
CRUGOS: user fork start
CRUGOS: user fork tid 16 -> 17 (7 page(s) shared, COW)
CRUGOS: user cow fault tid 16 va=0x000000803fffffc8 (copy)
CRUGOS: user cow fault tid 17 va=0x0000008000011000 (copy)
CRUGOS: user fork tid 17
CRUGOS: user fork child word 8738
CRUGOS: user fork reap exit 3
CRUGOS: user cow fault tid 16 va=0x0000008000011000 (sole owner)
CRUGOS: user fork parent word 4369
CRUGOS: user fork parent write 17476
CRUGOS: user fork ok (child exit 3, memory isolated, 2 COW page(s))
CRUGOS: user fork test ok (child exit 3, memory isolated)
CRUGOS: user forkbig start
CRUGOS: user forkbig tid 16
CRUGOS: user forkbig child tid 17
CRUGOS: user cow fault tid 17 va=0x... (copy)
CRUGOS: user forkbig reap exit 7
CRUGOS: user forkbig ok (child exit 7, 300 page(s) isolated)
CRUGOS: user selftest ok (3 program(s), 1 fault(s))
CRUGOS: phase u6 boot ok
```

Between `user fault writing above the window` and the `user fault page fault`
line the report formatter prints the two `trap report`/`trap fault` lines quoted
in the fault section above (they are unchanged from U1: same vector, same error
bits, same `cr2`).

The boot check that accepts it:

```
ok: boot markers found, version 0.40.0 avx2 yes raid no nvme no rebuild no ahci no crypt no aes any net no tcp no dns no usb no mouse no audio no gfx no wb no menu no shell no keys no desk no icons no drawer no select no heap no user yes isolated yes ushell yes spawn yes fork yes forkbig yes storage disk QEMU exit status 33
```

The `user yes`, `isolated yes`, `ushell yes`, `spawn yes`, `fork yes` and
`forkbig yes` fields are the user fields; all others are the same as the H4
gate. The boot
timing line has the same shape (values vary by run):

```
boot   : pmm=27755 us cpu=397 us vm=663 us traps=321 us timer=65329 us heap=477 us storage=95269 us sched=373 us syscall=129 us total=189132 us
```

`check-disk` runs after the boot and reports the root filesystem intact:

```
ok: ext4 root is clean, /hello.txt and /selftest/b.txt persisted
```

That is the plain pass. Gate 53 does not pass `--expect-shell-write`, so it
never asserts `/shout.txt`; that clause in the check-disk output belongs to
gate 50 only.

Reading a failure:

- no `user selftest start` means the token did not reach the kernel (check the
  `CMDLINE_EXTRA` wiring) or the disk lacks the programs;
- `user selftest failed (/bin/... did not start)` means the loader refused the
  image - check the segment policy and the linker script;
- `(... exited X, expected Y)` names the exact mismatch: badptr returning 0
  instead of a clean `-EFAULT`, or fault not dying with -14;
- a missing `user isolation ...` line means the level did not reach
  `user_test.c`; an `iso tid ... word ... mismatch` line means two programs saw
  each other's write - the per-space mapping or the CR3 switch is broken;
- a missing `user shell start` or `sh start` line means `user.test=3` did not
  reach `user_test.c`, `/bin/sh` is missing from the disk, or the shell failed
  before its first syscall;
- a `user shell finished` exit other than 7 means the script stopped early or a
  file syscall failed - the last `sh cmd` line says where;
- a missing `user spawn ...` line means `user.test=4` did not reach
  `user_test.c`, `/bin/spawn` is missing from the disk, or the loader refused
  the image;
- a missing `user spawn tid` line means `SYS_SPAWN` returned an error - check
  the path, the argv block and the new space's page-table setup;
- a `user spawn child exit` other than 5 means `SYS_WAIT` returned the wrong
  code, or the child exited through a path other than its normal return;
- no `user exec /bin/child` line, or a tid on it that differs from the
  spawner's, means `SYS_EXEC` did not replace the image in place - the tid must
  survive the exec, and the new `vm_space` must be activated before the old one
  is destroyed;
- no `user fork test start` line means `user.test=5` did not reach
  `user_test.c`, `/bin/fork` is missing from the disk, or `SYS_FORK` failed
  before the child existed;
- a missing `user fork tid 16 -> 17` line means `SYS_FORK` returned an error:
  the parent is over the 4096-page cap, the 4096-slot reference table had no
  free slot, or a page allocation failed;
- a child that never prints `user fork child word` means the trampoline did not
  reach CPL3 - check `thread_fork_enter`, `isr_return` and the parked frame;
- a `user fork parent word` other than 4369 means the child's write reached the
  parent's frame: the clone aliased pages instead of sharing them read-only, or
  the fault path repaired the parent leaf instead of copying it;
- a missing `user fork parent write 17476` line means the sole-owner repair did
  not restore the writable bit after the child exited;
- a `user cow fault` line naming a `(copy)` where the leaf was already private,
  or a fork whose cow faults never appear, means the `VM_COW` bookkeeping is
  wrong; check that the parent leaf lost `VM_WRITE` and gained `VM_COW`;
- a missing `user forkbig ...` block means `user.test=6` did not reach
  `user_test.c`, `/bin/forkbig` is missing from the disk, or the child could not
  fork 300 pages;
- a `user forkbig` shared count below 300 means the clone refused or skipped
  pages - a 300-page fork should share every present writable leaf;
- a `user forkbig memory not isolated` line means the child's `0x5A` writes
  reached the parent's 300-page mapping;
- a `user fork reap exit` other than 3 means the child exited through another
  path or `SYS_WAIT` returned the wrong code;
- a `check-disk` complaint about `/shout.txt` belongs to gate 50: gate 53 runs
  the plain pass and expects only `/hello.txt` and `/selftest/b.txt`.
  A missing `/hello.txt` or `/selftest/b.txt`, or an unclean ext4, means the
  disk build or the umount path broke;
- a panic during the run means a fault was classified `TRAP_FATAL` - the panic
  line names the vector;
- an unexpected `CRUGOS: user ...`, `iso ...` or `sh ...` line on a non-user gate
  is itself a failure (boot-check requires the matching `EXPECT_*` flag).

## Limits

- Address-space isolation is between user programs, not between a program and
  the kernel: slot 0 is still the shared supervisor identity map in every space.
- The user window's intermediate page tables (the PML4 slot 1 PDPT, its PD and
  its PTs) are created once per space and released only at `vm_space_destroy`;
  `vm_space_unmap_user_pages` frees the leaf frame, not the tables. A fork
  builds a second set for the child's space, so every fork leaves the parent's
  tables in place and adds a new tree that is freed only when the child dies.
- Fork is copy-on-write but not lazy beyond that: the first store to a shared
  page on either side copies that page, so a program that writes its whole
  window after a fork still pays one `memcpy` per written page. A page that is
  never written is never copied.
- The reference table is a fixed 4096-slot table with no growth: a fork that
  would need more distinct shared frames is refused with `ENOMEM` (the clone
  destroys the half-built child) rather than falling back to an eager copy.
- A fork is capped at `VM_SPACE_FORK_MAX_PAGES` (4096 pages): a parent with more
  user pages is refused with an error rather than given a partial copy. There is
  no swap and no page cache behind a share.
- The forked child inherits memory only. Its descriptor table starts empty
  (`ufile_table_init`), so an open file is not shared and a buffered write is
  never flushed twice; there are no pending signals or timers to inherit either.
- `SYS_SPAWN` still starts a new image from a path, so a spawn child inherits
  neither the caller's memory nor its file descriptors, and `SYS_EXEC` replaces
  only the calling image.
- `SYS_WAIT` works only for direct children: `parent_id` must name the caller,
  there is no wait-any, no notification other than the blocking call, and no
  signals. A child that exits stays in the all-threads list as a zombie until
  its parent waits for it; there is no reaper for orphaned children.
- A tid is the scheduler's thread id, not a process id: ids are monotonic and a
  stale one never matches a recycled thread, but there is no pid namespace and
  no `getpid`.
- `argv` exists only at spawn and exec time: the kernel builds the block, the
  shell receives one script path, and nothing sets an environment variable.
- exec does not carry the old image over: the new program gets exactly the
  `argv` passed to `SYS_EXEC` and still runs in the thread's single address
  space, because the new `vm_space` replaces the old one and its mappings are
  gone.
- No user heap and no `malloc` in ring 3: the shell and the test programs use
  static buffers, so dynamic memory needs a ring-3 allocator first.
- File descriptors are per thread, not per process: `thread_destroy` calls
  `ufile_close_all`, a spawned child starts with an empty table, and there is no
  way to share an open file between threads.
- No pipes, append or seek: the five calls above are the whole interface. A read
  is a 64 KiB snapshot taken at open time, and a writable open rewrites the file
  from offset 0 on close.
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
