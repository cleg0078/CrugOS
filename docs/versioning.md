# Versioning

CrugOS uses [Semantic Versioning 2.0.0](https://semver.org/) with annotated
`vMAJOR.MINOR.PATCH` Git tags. The current release is `v0.40.0`, phase U6:
copy-on-write fork. Phase U6 turns `SYS_FORK` into a lazy copy: the child
shares the parent's frames read-only and either side takes a private copy on
its first store, so a fork of 300 pages costs a page table and a few
reference counts, and the page cap rises from 256 to
`VM_SPACE_FORK_MAX_PAGES` 4096. `include/crug/cow.h` and
`kernel/mm/cow.c` add a per-frame reference table with `COW_TABLE_SLOTS` 4096
open-addressing slots, index `(pa >> 12) * 2654435761` folded with
`x ^= x >> 16`, forward probing and tombstones; `cow_share()` records one more
owner (the first share sets two) and returns -1 when the table has no free slot,
`cow_refs()` reports the owners or 0 for an untracked frame, and
`cow_release()` returns 1 when the caller is now the last owner and must free
the frame, 0 when another space still maps it. `include/crug/vm.h` adds the
software PTE bit `VM_COW` (`1ull << 9`, part of `VM_PTE_FLAG_MASK`), makes
`vm_space_clone()` non-const and adds `vm_space_cow_fault()` (0 = nothing to
resolve, 1 = frame copied, 2 = sole-owner repair, -1 = allocation failure) and
`vm_active_space()`; `kernel/mm/vm_space.c` shares every present slot-1 leaf
through `cow_share()` and `space_map_shared()`, copying the parent's
`PRESENT`/`USER`/`NX`/`PWT`/`PCD` bits and adding `VM_COW` when the leaf is
writable or already `VM_COW`, while a writable parent leaf loses `VM_WRITE`
and gains `VM_COW`; a read-only text leaf is shared as a plain read-only
mapping - it never takes `VM_COW` and never faults, but it is still entered in
the refcount table, so the frame is freed only when the last space that maps it
is destroyed - and a full table or a mapping failure destroys the half-built
child, so the fork fails with `ENOMEM` rather than falling back to an eager
copy; `vm_space_cow_fault()` repairs a leaf in place when `cow_refs()` is at
most one and otherwise allocates a fresh frame, `memcpy`s one page, maps it
writable without `VM_COW` and releases the old frame, freeing it when that
release was the last reference, and `vm_space_unmap_pages()`/`vm_space_destroy()`
free a frame only when `cow_release()` says it is the last reference;
`kernel/mm/vm.c` tracks the active space beside the active page table,
`uaccess_pages_writable()` in `kernel/mm/uaccess.c` resolves a `VM_COW` leaf
before a kernel copy-out while `uaccess_user_read()` keeps the plain mapped
check, and `kernel/arch/x86_64/idt.c` resolves a user page fault (vector 14,
error U/S and W/R set, `trap_classify()` = `TRAP_USER`) through
`vm_space_cow_fault()` before the ordinary trap report, flushing the page and
returning to CPL3 with `CRUGOS: user cow fault tid N va=0x... (copy)` or
`(sole owner)` and falling through to the kill-only-that-thread path when the
fault is not resolvable; `SYS_FORK` (24, unchanged ABI) calls `vm_flush_all()`
after the child exists and prints
`CRUGOS: user fork tid 16 -> 17 (7 page(s) shared, COW)`; `user/fork.c` now
holds two data pages (`0x1111`, `0x3333`), the child writes `0x2222`/`0x5555` and
exits 3, the parent reaps it, must still read `0x1111` and `0x3333`, then stores
`0x4444` into the first page on the sole-owner path and prints
`CRUGOS: user fork parent write 17476`, and `user/forkbig.c` maps 300 pages of
`.bss`, plants an `0xA5` sentinel, forks, has the child write `0x5A` to the first
and last byte and exit 7, and verifies its own copy before printing
`CRUGOS: user forkbig ok (child exit 7, 300 page(s) isolated)`;
`scripts/mkdisk.sh` installs nine programs, `kernel/core/user_test.c` adds
`user.test=6` (the U1 trio, the U2 isolation pair, the U3 shell, the U4
spawn/exec probe, the U5 two-page fork and `user_forkbig()`), and
`make test-user6` is the 53rd gate -- rebuild the disk with all nine programs,
boot with the token under `FORKBIG_RUN_TIMEOUT_SEC` 300 and pass boot-check the
new final `EXPECT_FORKBIG` argument (`argv[32]`, argument guard
`if len(sys.argv) < 3 or len(sys.argv) > 33:`) alongside `EXPECT_USER`,
`EXPECT_USERS`, `EXPECT_USHELL`, `EXPECT_SPAWN` and `EXPECT_FORK`, so the boot
line reports `user yes isolated yes ushell yes spawn yes fork yes forkbig yes`,
then run the plain `check-disk` pass with no `--expect-shell-write` clause.
`make test-host` reports 7893 checks (`test_vm_space_clone` rewritten for
copy-on-write and `test_vm_space_cow` covering the table's share chains,
tombstones, unaligned fold, full-table refusal and reset) and `make size` a
277040 B (270 KiB) file and a 913790 B (892 KiB) load image (`.text` 196038 B,
`.rodata` 29568 B, `.data` 56 B, `.bss` 688128 B) under the 1024 KiB stretch
and 2048 KiB hard budgets, for 53 gates at `v0.40.0` -- on top of `v0.39.0`,
phase U5: fork, a copied address space. The U5 release copies the caller's
user window into a new address space: `include/crug/syscall.h` adds `SYS_FORK`
24 with
`SYS_MAX` 25 and the call returns the child's tid in the parent and 0 in the
child, `include/crug/vm.h` and `kernel/mm/vm_space.c` add `vm_space_clone()`
capped at `VM_SPACE_FORK_MAX_PAGES` 256 pages, which builds a child PML4 that
inherits slot 0, the shared supervisor identity map, and copies every present
slot-1 leaf into a fresh frame carrying the parent's `PRESENT`/`WRITE`/`NX`
bits and one copied page, destroying the half-built space on any failure and
refusing a parent above the cap rather than cloning part of it,
`thread_create_forked()` in `kernel/core/thread.c` keeps a copy of the
parent's trap frame in `THREAD_FLAG_FORKED`/`fork_frame` with `rax = 0` so the
child resumes immediately after the `int 0x80`, `thread_fork_enter` in
`kernel/arch/x86_64/user.S` (`cli`, `call thread_fork_resume`,
`movq %rax, %rsp`, `jmp isr_return`) parks that frame
`THREAD_FORK_FRAME_GAP` 1024 bytes below the child's kernel stack top,
restores `%ds`/`%es` to `SEG_USER_DS` and installs the child's stack through
`cpu_set_kernel_stack()` before returning through the `isr_return` label that
`kernel/arch/x86_64/isr.S` now exports just before its `popq %r15`, so a forked
child reaches CPL3 through the same interrupt epilogue as every other return,
`thread_create()` calls `ufile_table_init()` so a child starts with an empty
descriptor table (no inherited descriptor and no double flush), `user/fork.c`
adds `/bin/fork` (the child writes `0x2222` into a volatile word and exits 3,
the parent waits for the tid, requires exit 3 and must still read its own
`0x1111`, proving the pages were copied rather than aliased),
`scripts/mkdisk.sh` installs eight programs, `kernel/core/user_test.c` adds
`user.test=5` (the U1 trio, the U2 isolation pair, the U3 shell, the U4
spawn/exec probe and `user_fork()`, which joins `/bin/fork` and requires exit
3), and `make test-user5` is the 52nd gate -- rebuild the disk with all eight
programs, boot with the token under `FORK_RUN_TIMEOUT_SEC` 240 and pass
boot-check the new final `EXPECT_FORK` argument (`argv[31]`, argument guard
`tuple(range(3, 33))`) alongside `EXPECT_USER`, `EXPECT_USERS`,
`EXPECT_USHELL` and `EXPECT_SPAWN`, so the boot line reports
`user yes isolated yes ushell yes spawn yes fork yes`, then run the plain
`check-disk` pass with no `--expect-shell-write` clause. `make test-host`
reports 3723 checks and `make size` a 276616 B (270 KiB) file and an 846206 B
(826 KiB) load image (`.text` 194310 B, `.rodata` 29248 B, `.bss` 622592 B)
under the 1024 KiB stretch and 2048 KiB hard budgets, for 52 gates at
`v0.39.0` -- on top of `v0.38.0`, phase U4: spawn, exec and wait in ring 3. The
U4 release gives ring 3 a process model:
`include/crug/syscall.h` adds
`SYS_SPAWN` 21, `SYS_WAIT` 22 and `SYS_EXEC` 23 with `SYS_MAX` 24 and
`SYS_ESRCH` -3, `elf_spawn_user(path, argc, argv, parent_id, out)` in
`kernel/exec/elf_exec.c` starts a new image as a new ring-3 thread and records
the caller in the new `parent_id` field of `struct thread` while
`elf_exec_run()` stays the `argc == 0` wrapper and `elf_exec_run_args()`
passes parent id 0 for kernel-owned programs, `thread_find(id)` in
`kernel/core/thread.c` walks a global all-threads list that still holds
zombies so a wait arriving after the exit sees the code (ids are monotonic, so a
stale tid never matches a recycled thread) and `SYS_WAIT` only accepts a target
whose `parent_id` is the caller's tid and that is not the caller, reaping it
with `thread_join_code()`, `elf_exec_in_place()` replaces the calling image
by loading into a fresh `vm_space`, activating it before destroying the old one
because CR3 still points at the old tables, and then rewriting the saved trap
frame (`rip`, `rsp`, `rax`) so the `iretq` lands in the new image while the
thread keeps its tid (serial `CRUGOS: user exec /bin/child (tid 14)`) and a CPL0
caller gets `SYS_EINVAL`, `syscall_argv()` copies arguments from CPL3 memory
through `uaccess_check` and `uaccess_user_read`/`uaccess_user_str` into
`g_argv_buf` (the identity-map path with `harden_range_ok` and `memcpy` for
CPL0 callers) and ends in `exec_args_ok(argc, str_bytes)`, a pure predicate in
`kernel/exec/argv.c` enforcing `EXEC_ARGS_MAX` 16 and `EXEC_ARGS_STR_MAX`
2048 bytes before any space is allocated, `user/spawn.c` and `user/child.c`
add `/bin/spawn` and `/bin/child` (spawn starts the child with
`from-spawn`, prints the tid, waits for exit 5, then execs it with
`from-exec`; the child prints its tid, `argc 2` and the argument; the program
loop in `scripts/mkdisk.sh` now installs seven programs),
`kernel/core/user_test.c` adds `user.test=4` (the U1 trio, the U2 pair, the
shell and `user_spawn()`, which joins `/bin/spawn` and requires exit 5), and
`make test-user4` is the 51st gate -- rebuild the disk with all seven programs,
boot with the token under `SPAWN_RUN_TIMEOUT_SEC` 240 and pass boot-check the
new final `EXPECT_SPAWN` argument (`argv[30]`, argument guard
`tuple(range(3, 32))`) alongside `EXPECT_USER`, `EXPECT_USERS` and
`EXPECT_USHELL`, so the boot line reports
`user yes isolated yes ushell yes spawn yes`, then run the plain `check-disk`
pass with no `--expect-shell-write` clause. `make test-host` reports 3670
checks and `make size` a 276408 B (269 KiB) file and an 844414 B (824 KiB) load
image (`.text` 192902 B, `.rodata` 28864 B, `.bss` 622592 B) under the
1024 KiB stretch and 2048 KiB hard budgets, for 51 gates at `v0.38.0` -- on top
of `v0.37.0`, phase U3: a ring-3 shell with file syscalls. The U3 release gives
ring 3 the mounted ext4 root: `include/crug/syscall.h` adds
`SYS_OPEN` 17, `SYS_READ` 18, `SYS_CLOSE` 19 and `SYS_LISTDIR` 20 with
`SYS_MAX` 21 and `SYS_ENOENT` -2, `SYS_WRITE` 16 keeps the console for
`fd` 1 and 2 and now writes the open file for `fd` 3 and up, and the open
flags are `SYS_O_RDONLY` 0, `SYS_O_WRONLY` 1, `SYS_O_CREAT` 2 and
`SYS_O_TRUNC` 4. `include/crug/ufile.h` and `kernel/fs/ufile.c` add a
per-thread descriptor table of `UFILE_FD_MAX` 8 slots starting at
`UFILE_FD_BASE` 3 with `UFILE_MAX_BYTES` 64 KiB read snapshots and write
buffers and `UFILE_PATH_MAX` 64, closed wholesale by `ufile_close_all()`
from `thread_destroy()`; a writable close flushes through create, a
whole-buffer write, or truncate to zero, then `fsync`, and brackets the
filesystem with optional `begin_write`/`end_write` hooks that the ext4
backend implements as `storage_remount_rw()`/`storage_remount_ro()` because
the root boots read-only (the log shows `CRUGOS: storage remounted read-write`
then `read-only` around the write). `include/crug/exec.h` and
`kernel/exec/argv.c` add `exec_args_build()`, which writes the strings,
`envp[]`, `argv[]` and the `argc` word into a 4 KiB
`EXEC_ARGS_BLOCK_MAX` area at the top of the user stack (16-byte aligned,
`EXEC_ARGS_MAX` 16 arguments, `EXEC_ARGS_STR_MAX` 2048 bytes of strings),
and `elf_exec_run_args(path, argc, argv, out)` starts the thread with `rsp`
at the `argc` word while `elf_exec_run()` stays the `argc == 0` wrapper;
`user/start.S` reads `argc`/`argv`/`envp` into `rdi`/`rsi`/`rdx`
before aligning the stack. `user/sh.c` and `user/sh.ld` add `/bin/sh` at
text `0x8000010000` and data `0x8000020000` with the commands `ver`,
`echo TEXT`, `cat FILE`, `ls PATH`, `write FILE TEXT` and `exit N`,
reading `argv[1]` or `/etc/sh.rc` into static buffers because there is no
ring-3 heap; `scripts/mkdisk.sh` writes the shipped `/etc/sh.rc` whose
`write /shout.txt hello-from-user-shell` leaves 22 bytes on the disk.
`kernel/core/user_test.c` adds `user.test=3` (the U1 trio, the U2 isolation
pair and then the shell, expecting exit 7), and `make test-user3` is the 50th
gate -- rebuild the disk with all five programs, boot with the token under
`SHELL_RUN_TIMEOUT_SEC` 240 and pass boot-check the new final
`EXPECT_USHELL` argument (`argv[29]`, argument guard
`tuple(range(3, 31))`) alongside `EXPECT_USER` (`argv[27]`) and
`EXPECT_USERS` (`argv[28]`), so the boot line reports
`user yes isolated yes ushell yes`, then `check-disk --expect-shell-write`
asserts `/shout.txt` holds `hello-from-user-shell` plus a newline after a
clean umount and `e2fsck`. `make test-host` reports 3663 checks and
`make size` a 267976 B (261 KiB) file and an 842046 B (822 KiB) load image
(`.text` 190790 B, `.rodata` 28608 B, `.bss` 622592 B) under the 1024 KiB
stretch and 2048 KiB hard budgets, for 50 gates at `v0.37.0` -- on top of
`v0.36.0`, phase U2: per-process address space. The U2 release turns U1's
single shared address space into one page-table root per ring-3 thread.
`include/crug/vm.h` adds
`struct vm_space` (`u64 *pml4`, `u64 phys`, `u32 user_pages`, `u32 pad`) with
`vm_space_set_kernel_root()`/`vm_space_create()`/`vm_space_destroy()`/
`vm_space_map_user_pages()`/`vm_space_unmap_user_pages()`/`vm_space_translate()`/
`vm_space_activate()` and `vm_kernel_root_phys()`, implemented in a separate
host-testable `kernel/mm/vm_space.c` that needs only `pmm_alloc_page()`/
`pmm_free_page()` and the identity translations, while the CR3 write stays in
`kernel/mm/vm.c` where `vm_space_activate()` is the only writer and skips an
unchanged root. Every space copies slot 0 from the kernel root -- the shared
supervisor identity map over kernel image, stacks and MMIO -- and owns slot 1,
the 512 GiB `USER_BASE` `0x0000008000000000`..1 TiB `USER_TOP`
`0x0000010000000000` window: mapping upgrades every intermediate entry to
`PRESENT|WRITE|USER`, clears `VM_GLOBAL` and forces `VM_USER` on the leaf,
keeps the caller's W^X flags (`VM_WRITE|VM_NX` on data pages), validates page
alignment, the window and an absent leaf, and rolls back exactly the pages a
failed call had mapped; `vm_space_destroy()` frees every leaf frame, PT, PD,
PDPT and the root page. `kernel/mm/vm.c` keeps the active root in a static
`g_root` that `pte_for()` and `vm_translate()` follow, so uaccess walks the
running space. `struct thread` carries `vm_space *space` instead of the U1
`thread_user_map[8]` array, `schedule()` installs the kernel stack top and
then calls `vm_space_activate(next->space)`, `thread_destroy()` destroys the
space, and `thread_create_user(name, entry, user_rsp, space, sched_class)` no
longer returns `-EBUSY`, so the one-program rule is gone.
`kernel/exec/elf_exec.c` owns one space per exec and hands it to the thread,
and `vm_init` now publishes the root immediately after allocating it because
the section mapper walks the root (the old order page-faulted before the IDT
existed). `user/iso.c` and `user/iso.ld` pin a writable page at
`0x0000008000030000`; under `user.test=2` two instances write
`0x15500000 | (tid << 8)`, yield 300 times and re-check their own word while
`kernel/core/user_test.c` proves the same virtual address maps to different
frames (`CRUGOS: user isolation frames 0x28000 vs 0x38000 at 0x8000030000`) and
joins both. `make test-user2` is the 49th gate -- rebuild the disk with all
four programs, boot with `user.test=2` under `USER2_RUN_TIMEOUT_SEC` 240 and
pass boot-check the new final `EXPECT_USERS` argument (`argv[28]`, argument
guard `tuple(range(3, 30))`) alongside `EXPECT_USER`, so the boot line reports
`user yes isolated yes` (`make test-user` stays gate 48 on `user.test=1`).
`tests/host/test_vm_space.c` covers slot-0 inheritance, the window validation
table, raw leaf flags, distinct frames for the same VA, partial rollback, full
reclaim and an allocation-failure path that asks for the whole 512 GiB window,
and `host_pmm_free_pages_count()` in `test_heap.c` makes a leak visible.
`make test-host` reports 3554 checks and `make size` a 263056 B (256 KiB) file
and an 832574 B (813 KiB) load image for 49 gates at `v0.36.0` -- on top of
`v0.35.0`, phase U1: ring 3 and the first user program. The U1 release grows
the GDT from five to seven
entries -- kernel code/data `0x08`/`0x10`, the 64-bit TSS at `0x18` and
ring-3 code/data `0x2B`/`0x33`, named in `include/crug/segment.h` -- and makes
`rsp0` per thread: `cpu_set_kernel_stack()` is its only writer and
`schedule()` installs the incoming thread's kernel stack top, so a trap from
CPL3 lands on a valid stack. `include/crug/vm.h` fixes the user window at
`USER_BASE` `0x0000008000000000` (512 GiB, PML4 slot 1) through `USER_TOP`
`0x0000010000000000` (1 TiB), and `kernel/mm/vm.c` adds
`vm_user_map_page`/`vm_user_map_pages`/`vm_user_unmap_pages`, which create
every intermediate table with `VM_USER`, strip `VM_GLOBAL` and take W^X from
the caller. `kernel/exec/elf_exec.c` (`include/crug/exec.h`) reads
`/bin/<name>` with `ext4_read_file` into a `ELF_MAX_BYTES` 512 KiB buffer,
parses it with the same `boot/efi/elf.c` the UEFI loader uses, reserves each
`PT_LOAD` read+execute or `VM_WRITE|VM_NX`, copies the bytes through
`pmm_phys_to_virt` so SMAP cannot trip the loader, ignores empty segments,
maps `USER_STACK_PAGES` 4 pages just below `USER_STACK_TOP`
`0x0000008040000000` with an unmapped page under them, and validates the
segments, entry and stack with `harden_user_segment_ok`/
`harden_user_entry_ok`. `kernel/mm/uaccess.c` is the only path to user
memory: `uaccess_check` is pure window arithmetic (`harden_user_range_ok`), a
page-table walk reports `UACCESS_EFAULT` (-14) for an unmapped page instead of
taking a kernel `#PF`, and `stac`/`clac` bracket only the copy when CPUID
exposes SMAP; `uaccess_user_read`/`uaccess_user_write`/`uaccess_user_str` are
the three primitives. `kernel/core/syscall.c` serves both rings --
`from_user()` tests `frame->cs & 3`, CPL0 callers keep the identity-map path
so the in-kernel selftests are unchanged, CPL3 callers go through bounce
buffers -- and adds `SYS_WRITE` 16 (fd 1/2, at most 4096 bytes in 256-byte
chunks) with `SYS_MAX` 17 and `SYS_EBUSY` -16, while `SYS_EXIT` from CPL3
ends only the calling thread. `kernel/arch/x86_64/user.S` and
`kernel/core/thread.c` give a `THREAD_FLAG_USER` thread `user_rip`/`user_rsp`
and up to `THREAD_USER_MAP_MAX` 8 mapped regions, refuse a second live user
thread with `-EBUSY`, unmap the recorded pages in `thread_destroy()`, and
build the `iretq` frame (`SS 0x33`, user `RSP`, `RFLAGS 0x202`, `CS 0x2B`,
entry `RIP`). `kernel/arch/x86_64/idt.c` now charges a CPL3 fault to the
program: the H3 report is followed by `CRUGOS: user fault <name> at
rip=... err=... tid N` and `CRUGOS: user fault killed tid N code -<vector>`,
then `thread_kill_current()` ends that thread, while user `#DF`/`#NMI`/`#MC`
and every CPL0 fault still panic (the old `panic("user-mode <name>")` path is
gone). `user/` holds the three freestanding programs `hello.c`, `badptr.c`
and `fault.c`, the `user.ld` script (base `0x8000010000`, separate R+X text
and R+W data `PT_LOAD`s) and `user/lib.h` (inline `int $0x80` wrappers, `nr`
in `rax`, arguments in `rdi`/`rsi`/`rdx`/`r10`); `make user` builds them and
`scripts/mkdisk.sh` writes them into `/bin` when `USER_DIR` is set.
`kernel/core/user_test.c` parses `user.test=1` and runs the three programs in
sequence (exit 0, exit 0, exit -14), and `make test-user` is the 48th gate: it
rebuilds the disk with the programs, boots with the token under
`USER_RUN_TIMEOUT_SEC` 240 and passes boot-check the new final `EXPECT_USER`
argument, so every other gate must not print a user marker. `make test-host`
reports 3470 checks and `make size` a 262760 B (256 KiB) file and an 831006 B
(811 KiB) load image for 48 gates at `v0.35.0` -- on top of `v0.34.0`, phase
H4: host mount-path fuzzer. The kernel image, the `BootInfo` v1 ABI and every
boot path are unchanged; the release adds `tests/host/fuzz_mount.c` (about 890
lines), a `FUZZ_SOURCES`/`fuzz` target in `tests/host/Makefile` and
`make test-fuzz`. The harness links the real `kernel/fs/ext4.c`,
`kernel/fs/gpt.c`, `kernel/fs/journal.c`, `kernel/block/block.c`,
`kernel/block/crypt_meta.c`, `kernel/mm/heap.c`, the `kernel/lib/*` helpers
and the `kernel/crypto/*` primitives against host stubs for `kprintf()`
(silent unless `--verbose-kernel`), `panic()` (exit 97) and a `pmm_*`
allocator over a 32 MiB static arena, and serves a `scripts/mkdisk.sh` image
through a bounds-checked in-memory block device. It locates the `crugroot`
partition with the real `gpt_parse()` (callers test `< 0`, because the
function returns the partition count) and weights six mutation regions --
superblock 22, group descriptors 14, inode table 16, root directory block 16, a
64 KiB metadata window 17 and the whole partition 15. Each case forks a child
on a copy-on-write image, applies one to four of eight mutations (one-byte XOR,
`0xFF` fill, zero fill, `u16`/`u32`/`u64` write, a 16-byte splice, a
16-byte random fill) from edge-weighted values seeded by `--seed`, walks
`gpt_parse()` -> `ext4_mount()` (25% read-write) -> lookup/list/read -> the
create/write/truncate/fsync/mkdir/rename/unlink write path -> `ext4_umount()`
and encodes the depth reached in its exit code (0 rejected, 1 mounted, 2 walked
a path, 3 wrote); a signal, the panic code 97 or an exit above 3 prints the
plan and dumps the mutant under `build/x86_64/fuzz/`, `--selftest` forks a
null dereference to prove the detector, `control_mount()` requires the
pristine image to mount and read `/hello.txt`, and a second leg mutates the
256-byte `struct crug_crypt_header` and calls
`crypt_meta_validate()`/`crypt_meta_scratch_bytes()` without unwrapping a
keyslot. `make test-fuzz` is the 47th gate -- rebuild `disk.img` and
`crypt.img`, run the self-test and then `FUZZ_CASES` 20000 ext4 cases plus
`FUZZ_CRYPT_CASES` 2000 header cases under `FUZZ_SEED` 1337 and
`FUZZ_TIMEOUT_SEC` 10 in about 23 seconds with no QEMU boot and therefore no
kernel version line, reporting about 10580 cases mounted, 10535 that walked a
path, 2594 that wrote and 0 crashes, with seeds 1337, 7, 424242 and 999983 all
clean; `make test-host` still reports 3400 checks and `make size` the same
253144 B (247 KiB) file and 821986 B (802 KiB) load image for 47 gates at
`v0.34.0` -- on top of `v0.33.0`, phase H3: trap policy and crash reports.
Every non-`#BP` exception goes through one
bounded formatter
(`kernel/lib/trapfmt.c`: no `kprintf`, no serial, no framebuffer) that writes
a `CRUGOS: trap report <name> vector=<dec> error=0x<hex> ist=<dec> from=<kernel|user>`
head, the saved `rip`/`cs`/`rflags`/`rsp`/`ss` line, a `#PF`-only
`present`/`write`/`user`/`reserved`/`instruction`/`cr2` line and two
register lines, lowercase and unpadded in at most 1024 bytes.
`trap_classify()` keeps `#BP` handled in both rings and `#OF` handled only
for a user frame, treats the user-killable set {0 divide, 1 debug, 6 invalid
opcode, 11 segment not present, 12 stack-segment, 13 general protection, 14 page
fault, 16 x87, 17 alignment, 19 SIMD, 21 control protection} as `TRAP_USER`
when `cs & 3 == 3` and fatal from ring 0, and every other vector (`#NMI`,
`#DF`, `#MC`, 5, 7, 9, 10, 15, 20, 22..31 and >= 32) as fatal outright;
`TRAP_USER` still panics (`user-mode <name>`) because there is no ring 3 yet.
`__df_stack` grows to 16 KiB and `__nmi_stack`/`__mc_stack`/`__db_stack` are
new (8 KiB each, +32 KiB of `.bss`), loaded into TSS `ist1`..`ist4` while
`rsp0` keeps pointing at the boot stack. `trap.test=<recipe>` (de, ud, gp,
pf, df, nmi, mc, db) injects one fault as the last action before
`arch_exit_qemu(0x10)`, and `make test-trap` is the 46th gate:
`scripts/run-trap.py` boots the control plus eight recipes from per-case disks
under `build/x86_64/trap/` with `TRAP_TIMEOUT_SEC` 60 and requires exit 35 and
the report and panic markers from every hostile case, while the control exits 33;
`tests/host/test_trap.c` brings `make test-host` to 3400 checks for 46 gates at
`v0.33.0` -- on top of `v0.32.0`, phase H2:
the hardened kernel heap. Every slab page and every large-block header starts
with a `u64` cookie keyed by its page address (`SLAB_COOKIE_SALT`
`0x534c4142ull` / `LARGE_COOKIE_SALT` `0x4c415247ull`), `struct slab_header`
keeps its 64-byte footprint but is validated field by field (class, object
counts, used bitmap, free-list entries, cycle cap), and large blocks form a
doubly linked list with interior-pointer detection. `heap_check()` walks every
slab class and the large list and returns one of nine `HEAP_CHECK_*` codes
(`heap_check_name()`), cross-checks
`g_slab_pages`/`g_large_pages`/`g_large_bytes` and masks interrupts.
`heap_stress(ops)` and `heap_guard()` prove the checker from the boot line
(`heap.stress=` / `heap.guard=`), `heap_selftest()` prints
`CRUGOS: heap check ok (6 slab(s), 8 object(s), 1 large block(s))`, and
`include/crug/cpu.h` gains a `CRUG_HOST_TEST` branch so
`kernel/mm/heap.c` runs under `tests/host`. `make test-heap` is the 45th
gate: it boots with `heap.stress=$(HEAP_STRESS_OPS) heap.guard=1`
(`HEAP_STRESS_OPS` 4000, `HEAP_RUN_TIMEOUT_SEC` 240), requires the stress,
guard and check markers and a clean `e2fsck`, and `tests/host/test_heap.c`
brings `make test-host` to 3235 checks for 45 gates at `v0.32.0` -- on top
of `v0.31.0`, phase H1: hardening pass 1, the first pass that treats the boot
path as hostile input.
`include/crug/harden.h` and `kernel/lib/harden.c` add seven pure validators --
`harden_range_ok()`, `harden_payload_ok()`, `harden_extent_header_ok()`,
`harden_ext4_geometry_ok()`, `harden_nvme_doorbell_ok()`, `harden_kdf_cost_ok()`
and `harden_volume_ok()` -- each host-tested in `tests/host/test_harden.c`. A
`SYS_PORT_SEND`/`SYS_PORT_RECV` buffer outside the 4 GiB identity map now
returns `SYS_EFAULT` (-14) instead of writing through a bad pointer, and
`msgport_send()` refuses a NULL payload with a non-zero length. An ext4 mount
refuses a crafted `s_blocks_per_group`/`s_inodes_per_group` (`CRUGOS: ext4
mount rejected (geometry)`, before the superblock checksum) and a root inline
extent that does not fit the 60-byte `i_block` area (`CRUGOS: ext4 mount
rejected (root extent)`, after the inode checksum). The NVMe probe keeps every
doorbell inside BAR0 (`nvme   : doorbell stride outside BAR0`), and
`crypt_unlock()` enforces an Argon2id policy of 1024..65536 KiB, `t` 2..8,
lanes 1..16 and `m >= 8 * lanes` plus a `volume_sectors` bound against the
parent partition, both refusals wiping the 16 KiB header copy. `secure_zero()`
(volatile stores) backs every kernel wipe, and the loader zeroes its
`cmdline.txt` pool copy so `crypt.pass=` does not survive in USABLE memory.
`scripts/run-hardening.py` behind `make test-hardening` is the 44th gate: it
patches a pristine image per case (geometry-bpg, geometry-ipg, root-extent,
kdf-high, kdf-low, volume-huge), boots it and requires the rejection marker and
exit 35 with no panic, while the control boots to exit 33. `make test-host`
reports 3200 checks, for 44 gates at `v0.31.0` -- on top of `v0.30.0`, phase 6H:
click-selectable rows in the Workbench `System:` drawer and a draggable
scrollbar, built on a generic Intuition body-input hook that lets a window own
the presses inside its body. `include/crug/intuition.h` adds
`INTUITION_BODY_PRESS` (0), `INTUITION_BODY_MOVE` (1) and
`INTUITION_BODY_RELEASE` (2) plus
`int intuition_set_body_input(int id, void (*fn)(int id, u32 kind, i32 x, i32 y, void *ctx), void *ctx)`,
which receives body-relative coordinates, clears on `NULL` and returns -1 for an
unknown id; `kernel/gfx/intuition.c` sets an internal body window on a press
inside a body with a handler, forwards `PRESS`, repaints and returns before any
title-bar drag, resize or gadget press, forwards every motion while the button
is held and forwards `RELEASE` before the icon, resize, gadget and drag
branches, and the new `body_rect()` helper shares its rectangle with
`win_draw_body()`. `kernel/gfx/workbench.c` adds `wb.dsel=1` (parsed before
`wb.shell=`): the (560,60,360x260) drawer's 356x236 body over 29 entries shows
11 rows on a 20-pixel pitch (max top row 18), a click at body `y` selects index
`top + (y - 6) / 20` and paints an amber `INTUITION_MENU_HIGHLIGHT_RGB`
(`0x00FFB000`) band with window-background text, and the 4-pixel `WB_TRACK_W`
track at body-local `x = body width - 8`, `y + 6`, `h - 12` carries a
`max(12, track * rows / entries)` thumb whose position follows the first
visible row: a track press jumps the view and grabs the thumb at the press
point, dragging follows the pointer and the release prints the final top row.
Markers `CRUGOS: wb select mode (click a row, drag the scrollbar)` (after the
desktop-ready line), `wb drawer selected '<name>' (index N of M)`,
`wb drawer scrollbar grabbed`, `wb drawer scrollbar to (top N of M)` and
`wb select selftest ok (2 select(s), 1 scrollbar drag(s))` (or
`wb select timeout (N/M)`).
`make test-workbench-select` clicks row 2, drags the thumb so the top row
becomes 18 and clicks the new row 0, then requires exit status 33,
`EXPECT_SELECT=yes` and a clean `e2fsck`; `tests/host/test_intuition.c`
gained `test_intuition_body_input` and `make test-host` reports 3104 checks,
for 43 gates at `v0.30.0` -- on top of `v0.29.0`, phase 6G:
the Workbench `System:` drawer as a scrolling viewport with arrow keys, a
refresh key, a scrollbar and a reflow on resize, a boot token that fills the
root and read-write remount helpers for the write window.
`kernel/gfx/workbench.c` turns the drawer body into a viewport over the root
listing: `wb_drawer_rows()` returns 0 for a body up to 22 pixels and
`(h - 22) / 20 + 1` above it, so the 360x260 drawer (236-pixel body) shows 11
rows and the 460x360 resize (336-pixel body) shows 16, and
`wb_drawer_scroll()` clamps the top row to `[0, count - rows]`;
`wb_drawer_refresh()` re-reads `ext4_list()` and clamps again; the painter
draws a 4-pixel track at `x + w - 8` with a thumb of
`max(12, track_h * rows / entries)` that follows the top entry, and prints
`CRUGOS: wb drawer mode (arrow keys scroll, r refreshes)`,
`CRUGOS: wb drawer ready (N entries, R row(s))`,
`CRUGOS: wb drawer scrolled (top N of M)` and
`CRUGOS: wb drawer refreshed (N entries)`. `wb.dr=1` gives the open drawer
the arrow keys and `r`/`R`, `wb.fill=N` (0..24) creates `/f00`..`/f(N-1)`
once before the listing loads, and because the ext4 root is mounted read-only
at boot `kernel/fs/storage.c` adds `storage_remount_rw()` /
`storage_remount_ro()` (declared in `include/crug/storage.h`) that umount and
remount around the brief write window, used by the fill and by the shell's
`write` command, which had been failing silently against the read-only mount.
`make test-workbench-drawer` opens the filled drawer, scrolls it three times,
resizes it to 460x360, refreshes it and checks exit status 33 and a clean
`e2fsck` -- on top of `v0.28.0`, phase 6F:
desktop icons that a press grabs and a pointer drag moves, a send-to-front
gadget in every title bar and keyboard focus for the icon table.
`include/crug/intuition.h` + `kernel/gfx/intuition.c`
add a 4-pixel `INTUITION_ICON_DRAG_MIN` dead zone before a grabbed tile moves,
`INTUITION_EVENT_ICON_MOVE` (type 9) with the
object index and its new origin, `INTUITION_EVENT_FRONT` (type 10),
`intuition_icon_moves()`, `intuition_front_count()` and
`intuition_icon_key(u8)`: a press on a tile selects and grabs it, motion moves
it to the cursor minus the grab offset clamped to `[0, screen width - 64]` x
`[0, screen height - 48]`, and the release queues `ICON_MOVE` (the amber frame
stays selected) and prints `CRUGOS: wb icon '<label>' dropped at (x, y)`; a
12x12 `INTUITION_FRONT_SIZE` box at `close_box_x - 32` raises its window, queues
`FRONT` and prints `CRUGOS: wb window N sent to front` (a no-op on the
frontmost window); and `kernel/gfx/workbench.c` reads `wb.ikeys=1`, where
Tab/Right/Down and Left/Up wrap the selection, Enter selects and then activates
it and Escape clears it, printing `CRUGOS: wb icons ready (N object(s),
keyboard focus)`, `CRUGOS: wb icon '<label>' focused` and
`CRUGOS: wb icons selftest ok (N move(s), M front(s))`. `make
test-workbench-icons` drives the drag, the front gadget and the keyboard
through QMP screendumps -- on top of `v0.27.0`, phase 6E: desktop objects that
a press selects and a double click opens, and a depth gadget that sends a
window to the back of the stacking order. `include/crug/intuition.h` +
`kernel/gfx/intuition.c` add a table of `INTUITION_MAX_ICONS` (8) objects: a
`INTUITION_ICON_WIDTH` x `INTUITION_ICON_HEIGHT` (64x48) tile with one label
line `INTUITION_ICON_LABEL_GAP` (4) pixels under it, drawn on the background
under every window and clamped so all its pixels stay on screen, in the kinds
`INTUITION_ICON_VOLUME` / `_DRAWER` / `_FILE` with `INTUITION_ICON_RGB`
`0x00c0c0c0`, `_INNER_RGB` `0x00081018`, `_TEXT_RGB` `0x00e0e0e0` and
`_SELECTED_BORDER_RGB` `0x00ffb000`; `intuition_add_icon()`,
`intuition_icon_rect()`, `intuition_icon_selected()`,
`intuition_icon_count()` and `intuition_icon_clicks()` expose the table, a
press selects a tile (amber frame and label), counts one action and queues
`INTUITION_EVENT_ICON` (type 7) with the index and tile origin, and a 12x12
`INTUITION_DEPTH_SIZE` box four pixels left of every close box sends its
window to the bottom of the stacking order, activates the new top window, queues
`INTUITION_EVENT_DEPTH` (type 8) then `ACTIVATE`, counts one action and prints
`CRUGOS: wb window N sent to back`; `intuition_set_paint()` /
`intuition_redraw_window()` paint a body from a callback. `kernel/gfx/workbench.c`
reads `wb.desk=1`, places a `System:` volume object at (16,240), prints
`CRUGOS: wb icon ready ('System:', volume)`, opens a 360x260 `System:` drawer
on a double click within `WB_DOUBLE_CLICK_NS` (600000000 ns) whose painter
lists at most 10 mounted-root names and prints
`CRUGOS: wb drawer 'System:' opened (N entries)` plus
`CRUGOS: wb desk selftest ok (N drawer(s))`, `make test-workbench-desk`
verifies the idle, selected, drawer and depth pixels through QMP screendumps,
and `make test-host` reports 3075 checks in this tree (2957 when phase 6E
landed) -- on top of `v0.26.0`, phase 6D:
window resizing from a 12x12 grip and keyboard navigation of the desktop.
`include/crug/intuition.h` + `kernel/gfx/intuition.c` give every window a
12x12 `INTUITION_RESIZE_SIZE` grip in its inner bottom-right corner at
`(x + w - INTUITION_BORDER - 12 - 2, y + h - INTUITION_BORDER - 12 - 2)`;
dragging it resizes live, floors the size at 120x80 (`INTUITION_MIN_WIDTH` /
`INTUITION_MIN_HEIGHT`) and refuses a motion past the screen, and the release
prints `CRUGOS: wb window %d resized to %ux%u`, counts one action,
increments `intuition_resize_count()` and queues `INTUITION_EVENT_RESIZE`
(type 6), whose `struct intuition_event` gained the new `u32 w` / `u32 h`
fields; `intuition_key_event(u8)` gives Tab a focus cycle through the stacking
order, moves the active window by `INTUITION_KEY_STEP` (8) pixels with each
arrow key, and opens, highlights, selects and dismisses the active window's
first menu popup with Enter/Down/Up/Enter/Escape. `include/crug/input.h`
gains `INPUT_KEY_UP` `0x90`, `INPUT_KEY_DOWN` `0x91`, `INPUT_KEY_LEFT`
`0x92` and `INPUT_KEY_RIGHT` `0x93`: the PS/2 set-1 decoder maps E0 make
codes `0x48`/`0x50`/`0x4B`/`0x4D` to them and ignores E0 break codes and
E0 modifier releases, `drivers/usb/usb_hid.c` maps HID usage page 0x07 usages
`0x52`, `0x51`, `0x4F` and `0x50` (Right `0x4F` before Left `0x50`),
`kernel/gfx/workbench.c` reads `wb.keys=1` (shell mode wins), counts resizes
through `wb.acts=` and prints `CRUGOS: wb keys ready (window 2 active)` plus
the resize, focus, key-move and menu markers, `make test-workbench-keys`
drives the grip drag and the keyboard navigation through
`scripts/run-workbench-keys.py`, and `make test-host` reports 2862 checks --
on top of `v0.25.0`, phase 6C: a live shell in the Workbench's second window.
`include/crug/textgrid.h` +
`kernel/lib/textgrid.c` add a scrollback of 96 lines of 38 columns (37 usable)
with a wrap, a backspace, tail-preserving overwrites, oldest-line eviction and a
clipped inverse-video cursor cell; `include/crug/shell.h` +
`kernel/shell/shell.c` add a 79-character line editor with `Ctrl-U`, `Ctrl-C`
and an eight-entry `Ctrl-P`/`Ctrl-N` history, a tokenizer that folds extra words
into the last argument and the command table `help`, `ver`, `mem`, `uptime`,
`ps`, `ls`, `cat`, `echo`, `write`, `sync`, `cls` over a five-call
`struct shell_io`, which `kernel/shell/shell_ops.c` binds to the mounted ext4
root, the heap counters and the scheduler; `intuition_set_paint()` /
`intuition_redraw_window()` let a window body be painted by a callback, so the
grid redraws on every keystroke; `workbench.c` reads `wb.shell=1` / `wb.sh=`,
routes every keystroke to the shell, prints `sh run` / `sh out` lines and
`CRUGOS: wb shell ready (window 2, 37x8 text)`, and `make test-workbench-shell`
types four commands through a hotplugged `usb-kbd` and verifies them against
two `screendump`s and the boot markers. `make test-host` reports 2792 checks --
on top of `v0.24.0`, phase 6B: menu bars with popups, body gadgets, a close
gadget and a typed event queue on the phase 6A desktop
(`intuition_set_menus()` / `intuition_add_gadget()` / `intuition_event_poll()`,
two menus of six items, four gadgets per window, a 150-pixel popup with 18-pixel
rows, a 12x12 close gadget, `ACTIVATE` / `DRAG` / `MENU` / `GADGET` / `CLOSE` in
a 16-entry FIFO that drops the newest, press precedence `popup row > menu bar >
close gadget > body gadget > title drag`, `wb.menu=` / `wb.acts=`,
`make test-workbench-menu` driving six QMP clicks and checking the highlighted
row, the desktop background after a close and the surviving white frame) -- on
top of `v0.23.1`, a packaging
release on top of phase 6A: `make iso` assembles a hybrid UEFI ISO that boots
from a USB stick and from a CD-ROM (`scripts/mkiso.sh` driving `xorriso`, a
deterministic FAT16 ESP image written by `scripts/mkfat.py` because this host
has no mtools, an isolated build tree whose `crugos/cmdline.txt` is rewritten
from `ISO_CMDLINE`), the kernel gained the `storage=none` token so an image with
no block device still passes the boot contract (`CRUGOS: storage skipped
(storage=none)`, soft-skipped storage self-tests, still fail-closed without the
token), `scripts/check-iso.py` re-parses the El Torito catalog, FAT16 BPB and
protective MBR and compares an `xorriso -osirrox` extraction against the boot
image, `make test-iso` boots the artifact as USB mass storage and as a CD-ROM
behind `boot-check.py --storage=none`, and `make usb` writes real media behind
`USB_DEV`/`USB_CONFIRM` guards -- on top of phase 6A: the
first Workbench slice -- `intuition.device` keeps four window slots in a
stacking order, hit-tests title bars top-down, raises and drags them with a grab
offset, clamps to the screen and repaints the whole desktop, `workbench.c` takes
the framebuffer console away when `wb.enable=` is supplied and opens two demo
windows, `wb.wait=` and `wb.drag=` bound the boot check, `make test-workbench`
hotplugs a USB mouse over QMP and verifies the active title bar in QEMU
screendumps, and the framebuffer gained `fb_fill_rect`/`fb_frame_rect`/
`fb_draw_text` plus the `fb_cursor_begin`/`fb_cursor_end` save-under pair -- on
top of the phase 5D pointer-driven cursor on the GOP framebuffer, where
`gfx.device` drains the phase 5B
`input.device` pointer state and moves a 12x19 sprite that the framebuffer
console saves, restores and recomposites around every text write, `gfx.wait=` and
`gfx.moves=` bound the boot check, `make test-gfx` hotplugs a USB mouse over QMP
and verifies the amber sprite centroid in QEMU screendumps against the guest's
own coordinates, and the `fb_pixel_color` BGRX/RGBX packing bug that had been
invisible under a greyscale console was found and fixed -- on top of the phase
5C USB Audio Class path with an `audio.device` PCM ring and a per-channel mixer,
where the xHCI driver gained a small isochronous API whose Normal TRBs are scheduled
as soon as possible and whose endpoint intervals are encoded in microframes,
`drivers/usb/usb_audio.c` parses UAC1 and UAC2 descriptors, selects alternate
setting 1 and keeps 32 packets in flight with success-only accounting,
`kernel/audio/mixer.c` maps 1/256 dB volume onto a 31-entry Q15 table, and a
hermetic `-audiodev none` gate streams half a second of a tone -- on top of the
phase 5B USB HID boot mouse with its four-device xHCI table, the phase 5A polled
xHCI host controller and USB HID boot keyboard with PCI BAR mapping below
`0xffc00000`, HCRST, command, event, EP0 and interrupt rings with Link TRB wrap,
slot enumeration through `GET_DESCRIPTOR`, `SET_CONFIGURATION` and
`CONFIGURE_ENDPOINT`, boot-report decoding into `input.device`, and port
hotplug with reset and unbind -- on top of the phase 4D caching DNS stub
resolver, the phase 4C TCP transport, the phase 4B polled
virtio-net driver, the phase 4A PS/2 keyboard input, the phase 3B-10
AES-256-XTS `crypt.device` with Argon2id keyslots, a checksummed crash-consistent filesystem over virtio-blk, NVMe, AHCI
and RAID1/RAID10 roots, and loader/token passphrase handling with redaction and
key zeroization).

## Source of truth

The repository file [`VERSION`](VERSION) is the only place a release number is
edited. The build reads it and passes `-DCRUG_VERSION_STR="<version>"` to the
kernel and the UEFI loader; `include/crug/version.h` supplies `0.0.0-dev` when
the compiler is invoked directly. The kernel prints the value on boot as
`CRUGOS: version <version>`, which the `make test` gate compares against
`VERSION`.

```
make version     # print the version the tree would build
```

## What the numbers mean

| Component | Bump when |
| --- | --- |
| MAJOR | a stable ABI breaks: the `boot_info` layout, the syscall dispatcher, the on-disk/handle interfaces, or the package format |
| MINOR | a phase or feature lands without breaking those ABIs |
| PATCH | bug fixes, documentation, build and test changes |

Pre-release suffixes (`0.2.0-rc1`) are allowed for release candidates. Once
`v1.0.0` exists, the kernel rejects a `boot_info` whose major version it does
not understand; before that, loader and kernel are always built and shipped
as a pair.

`BOOTINFO_VERSION` (currently 1) is versioned separately from the product: it
must be bumped whenever `struct boot_info` or a nested structure changes
layout or meaning, independently of whether the release is major, minor or
patch.

## Branches and tags

- `main` always builds and passes `make test`.
- Work happens on `feature/*` branches and reaches `main` by merge.
- A release is an annotated tag on `main`; tags are pushed explicitly.

## Release checklist

```
vi VERSION                       # pick the new SemVer
make distclean && make test      # clean build, boot gate, guest version check
make test-raid test-raid-degraded # RAID1 healthy and degraded gates
make test-raid10 test-raid10-degraded  # RAID10 healthy and degraded gates
make test-raid-repair test-raid10-repair  # cold-spare rebuild gates
make test-nvme                   # NVMe-only root boot and disk check
make test-ahci                   # SATA-only root boot and disk check
make test-journal-csum test-journal-replay  # JBD2 crash/replay gates
make test-crypt test-crypt-recovery test-crypt-wrongkey  # encrypted root
make test-crypt-tamper test-crypt-ni  # MAC tamper and AES-NI gates
make test-crypt-prompt test-crypt-prompt-wrong test-crypt-prompt-timeout  # console unlock
make test-crypt-keyboard test-crypt-keyboard-wrong  # PS/2 keyboard unlock
make test-net                    # virtio-net DHCP/ARP/ICMP boot gate
make test-tcp                    # TCP client/server HTTP gate
make test-net-dns                # DNS resolution over a host DNS server
make test-usb-hotplug            # hotplug a USB keyboard and type at it
make test-usb-mouse              # hotplug a USB mouse and move the pointer
make test-audio                  # stream PCM to a USB Audio Class device
make test-gfx                    # move a hotplugged USB mouse and verify the cursor
make test-workbench              # drag a window on the software-rendered desktop
make test-workbench-menu         # open menus, select items and click gadgets
make test-workbench-shell        # type commands into the Workbench shell window
make test-workbench-keys         # resize a window by its grip and drive it from the keyboard
make test-workbench-desk         # select desktop objects and send a window to the back
make test-workbench-icons        # drag an icon, raise a window by its front gadget and drive the icons
make test-workbench-drawer       # scroll, refresh and resize the System: drawer
make test-workbench-select       # click drawer rows and drag its scrollbar
make test-hardening              # craft hostile superblock/inode/crypt images and boot them
make test-heap                   # corrupt the kernel heap and prove the checker catches it
make test-trap                   # inject eight crafted faults and check report/IST/panic
make test-fuzz                   # mutate a real disk image and fuzz the mount path
make test-user                   # boot with user.test=1 and run three ring-3 programs
make test-user2                  # boot with user.test=2 and run the isolation pair
make test-user3                  # boot with user.test=3 and run the scripted shell
make test-user4                  # spawn and exec ring-3 programs and wait for their exit codes
make test-user5                  # fork a ring-3 program and prove the child gets its own pages
make test-user6                  # fork copy-on-write and prove 300 shared pages stay isolated
make test-iso                    # boot the hybrid ISO from USB and from CD-ROM
make size check-efi test-host repro
make check-disk                  # e2fsck + persisted files on the test disk
git add -A && git commit -m "release: vX.Y.Z"
git tag -a vX.Y.Z -m "CrugOS vX.Y.Z"
git push origin main vX.Y.Z
```

## Released versions

| Version | Date | Contents |
| --- | --- | --- |
| `v0.1.0` | 2026-10-27 | phase 0: UEFI loader, ELF handoff, BootInfo, serial + framebuffer console, boot report, reproducible build and boot gates |
| `v0.2.0` | 2026-10-27 | phase 1: bitmap physical allocator, identity page tables with W^X, GDT/IDT and exceptions, LAPIC timer and TSC calibration, AVX2 baseline; BootInfo stays v1 |
| `v0.3.0` | 2026-10-27 | phase 2: kernel heap, threads and scheduling classes, handles, MsgPort IPC, single `int 0x80` syscall dispatcher; BootInfo stays v1 |
| `v0.4.0` | 2026-10-27 | phase 3A: block layer, legacy virtio-blk, GPT, read-only ext4, generated test disk; BootInfo stays v1 and the kernel ABI is unchanged |
| `v0.5.0` | 2026-10-27 | phase 3B-1: writable ext4 (create/mkdir/write/truncate/unlink/rmdir/rename/fsync), virtio flush, boot write self-test and `e2fsck` disk gate; BootInfo stays v1 and the kernel ABI is unchanged |
| `v0.6.0` | 2026-10-27 | phase 3B-2: JBD2 journal replay, ordered metadata journaling, dirty-journal mount recovery and a replay self-test; BootInfo stays v1 and the kernel ABI is unchanged |
| `v0.7.0` | 2026-10-27 | phase 3B-3: multiple virtio-blk devices, checksummed RAID1 metadata, `md0` assembly, healthy/degraded root boot and host RAID gates; BootInfo stays v1 and the kernel ABI is unchanged |
| `v0.8.0` | 2026-10-27 | phase 3B-4: polled NVMe driver (64-bit BAR mapping, admin/I/O queues, namespace 1, PRP I/O, flush), NVMe-only root boot and `make test-nvme`; BootInfo stays v1 and the kernel ABI is unchanged |
| `v0.9.0` | 2026-10-27 | phase 3B-5: RAID10 metadata (level/chunk), mirror-pair mapping, four-member healthy and two-member degraded `md0` boots, host image builder/checker gates; BootInfo stays v1 and the kernel ABI is unchanged |
| `v0.10.0` | 2026-10-27 | phase 3B-6: cold-spare RAID rebuild at boot (blank member detection, lowest missing slot, 64-sector copy from the mirror, kernel-written metadata), `raid-blank.sh` and healthy/degraded repair gates; BootInfo stays v1 and the kernel ABI is unchanged |
| `v0.11.0` | 2026-10-27 | phase 3B-7: polled AHCI/SATA driver (class 01/06/01, port bring-up, 48-bit LBA IDENTIFY, DMA EXT commands, 8 KiB bounce), SATA-only root boot and `make test-ahci`; BootInfo stays v1 and the kernel ABI is unchanged |
| `v0.12.0` | 2026-10-27 | phase 3B-8: ext4 `metadata_csum` (superblock, group descriptors, bitmaps, inodes, directory/extent tails), JBD2 checksum v3 journals (tags, superblock, descriptor tail, commit block), multi-descriptor transactions, boot-time journal replay from a crash image, and `make test-journal-csum` / `make test-journal-replay`; BootInfo stays v1 and the kernel ABI is unchanged |
| `v0.13.0` | 2026-10-27 | phase 3B-9: `crypt.device` (GPT type `8f1c7d2e-...`, 16 KiB header, eight keyslots, AES-256-XTS data area with data-relative tweaks), Argon2id (RFC 9106) key derivation, 64-byte master key wrapped per slot with a keyed BLAKE2b-256 MAC and a recovery passphrase, AES-NI/software dispatch pinned to IEEE 1619 vector 10, loader `\crugos\cmdline.txt` with `crypt.pass=` plus redaction and key zeroization, `mkcrypt` host tool, `mkdisk.sh crypt` mode, and five encrypted-root gates; BootInfo stays v1 and the kernel ABI is unchanged |
| `v0.14.0` | 2026-10-27 | phase 3B-10: masked serial-console passphrase prompt when the ESP carries no `crypt.pass=`, shared line editor (`kernel/lib/lineedit.c`) with CR/LF, backspace, Ctrl-C and overflow handling, three attempts bounded by `crypt.prompt_timeout=`, fail-closed `crypt locked` markers, `SERIAL_TCP` serial capture and the `run-crypt-prompt.py` driver behind three new gates (correct, wrong, timeout); BootInfo stays v1 and the kernel ABI is unchanged |
| `v0.15.0` | 2026-10-27 | phase 4A: `input.device` (set-1 scancode decoder, Shift/Ctrl/Alt/Caps state, US layout, control-code mapping, 64-byte queue), polled i8042 keyboard driver, `console_try_getc()` merging serial and keyboard input, `CRUGOS: input ready (serial + keyboard)` marker and console prompt wording, QMP `send-key` driver (`run-crypt-keyboard.py`) behind two new keyboard gates and 903 host checks; BootInfo stays v1 and the kernel ABI is unchanged |
| `v0.40.0` | 2026-10-29 | phase U6: copy-on-write fork (`include/crug/cow.h`, `kernel/mm/cow.c`, `include/crug/vm.h`, `kernel/mm/vm_space.c`, `kernel/mm/vm.c`, `kernel/mm/uaccess.c`, `kernel/arch/x86_64/idt.c`, `kernel/core/syscall.c`, `kernel/core/user_test.c`, `user/fork.c`, `user/forkbig.c`, `scripts/mkdisk.sh`, `scripts/boot-check.py`, `tests/host/Makefile`, `tests/host/test_vm_space.c`) makes `SYS_FORK` share the parent's frames read-only through a 4096-slot open-addressing reference table (`COW_TABLE_SLOTS`, index `(pa >> 12) * 2654435761` folded with `x ^= x >> 16`, forward probing, tombstones; `cow_share()` first share sets two and returns -1 when full, `cow_refs()` 0 untracked, `cow_release()` 1 when the caller is last), adds the `VM_COW` software PTE bit `1ull << 9` to `VM_PTE_FLAG_MASK` and raises `VM_SPACE_FORK_MAX_PAGES` 256 -> 4096 so `vm_space_clone()` shares every present leaf through `cow_share()` and `space_map_shared()` (child leaf gains `VM_COW`, parent leaf loses `VM_WRITE`, a read-only text leaf is shared plain read-only but still tracked so the frame is freed only by the last space, and a full table or mapping failure destroys the half-built child so fork fails with ENOMEM), `vm_space_cow_fault()` returns 1 copy / 2 sole-owner repair / 0 nothing / -1 allocation failure and frees the old frame when `cow_release()` says last, `vm_active_space()` plus `g_active_space` in `kernel/mm/vm.c` locate the faulting space, `uaccess_pages_writable()` resolves a `VM_COW` leaf before a kernel copy-out, `idt.c` resolves a user page fault (vector 14, error U/S and W/R, `TRAP_USER`) before the trap report and prints `user cow fault tid N va=0x... (copy)` or `(sole owner)`, `SYS_FORK` calls `vm_flush_all()` and prints `user fork tid 16 -> 17 (7 page(s) shared, COW)`, `user/fork.c` probes two pages (child 0x2222/0x5555 exits 3, parent keeps 0x1111/0x3333, then a sole-owner store prints `user fork parent write 17476`), `user/forkbig.c` forks 300 pages (sentinel 0xA5, child 0x5A exits 7, `user forkbig ok (child exit 7, 300 page(s) isolated)`), `scripts/mkdisk.sh` installs nine programs, `user.test=6` adds `user_forkbig()`, `make test-user6` is gate 53 (rebuild disk, `FORKBIG_RUN_TIMEOUT_SEC` 300, `EXPECT_FORKBIG` `argv[32]` with `len(sys.argv)` in 3..33, boot line `fork yes forkbig yes`, plain check-disk, `make test-user5` stays green with `forkbig no`), `make test-host` reports 7893 checks, `make size` 277040 B (270 KiB) file / 913790 B (892 KiB) load (`.text` 196038 B, `.rodata` 29568 B, `.data` 56 B, `.bss` 688128 B) under the 1024/2048 KiB budgets, docs/userspace.md plus README, roadmap, versioning, build, boot, traps, hardening, iso, workbench, fuzzing and heap, BootInfo stays v1 and the kernel ABI is unchanged |
| `v0.39.0` | 2026-10-29 | phase U5: fork, a copied address space (`include/crug/syscall.h`, `include/crug/vm.h`, `kernel/mm/vm_space.c`, `kernel/core/thread.c`, `kernel/arch/x86_64/user.S`, `kernel/arch/x86_64/isr.S`, `kernel/exec/elf_exec.c`, `kernel/core/syscall.c`, `kernel/core/user_test.c`, `user/fork.c`, `user/user.ld`, `user/start.S`, `scripts/mkdisk.sh`, `scripts/boot-check.py`, `Makefile`: `SYS_FORK` 24 with `SYS_MAX` 25, returning the child's tid in the parent and 0 in the child so the child resumes at the instruction after `int 0x80`; `struct vm_space` cloning through `vm_space_clone()` capped at `VM_SPACE_FORK_MAX_PAGES` 256 pages, a child PML4 inheriting slot 0 (the shared supervisor identity map) and every present slot-1 leaf copied into a fresh frame with the parent's `PRESENT`/`WRITE`/`NX` bits and a page copy, any failure destroying the half-built space and a parent above the cap refused rather than partly cloned; `thread_create_forked()` keeping a copy of the parent's trap frame in `THREAD_FLAG_FORKED`/`fork_frame` with `rax` 0; `thread_fork_enter` in `kernel/arch/x86_64/user.S` (`cli`, `call thread_fork_resume`, `movq %rax, %rsp`, `jmp isr_return`) parking that frame `THREAD_FORK_FRAME_GAP` 1024 bytes below the child's kernel stack top, restoring `%ds`/`%es` to `SEG_USER_DS` and installing the child's stack through `cpu_set_kernel_stack()`, then returning through the `isr_return` label `kernel/arch/x86_64/isr.S` exports before its `popq %r15` so the child reaches CPL3 through the normal interrupt epilogue; `thread_create()` calling `ufile_table_init()` so a forked child starts with an empty descriptor table and a buffered write is never flushed twice; `user/fork.c` adding `/bin/fork` (the child writes `0x2222` into a volatile word and exits 3, the parent waits, requires exit 3 and must still read its own `0x1111`, proving the pages were copied and not aliased); `scripts/mkdisk.sh` installing eight programs; `user.test=5` running the U1 trio, the U2 isolation pair, the U3 shell, the U4 spawn/exec probe and `user_fork()`, which joins `/bin/fork` and requires exit 3; `make test-user5` as the 52nd gate -- rebuild the disk with all eight programs, boot with the token under `FORK_RUN_TIMEOUT_SEC` 240, parse the new `EXPECT_FORK` argument (`argv[31]` with argument guard `tuple(range(3, 33))`) alongside `EXPECT_USER`, `EXPECT_USERS`, `EXPECT_USHELL` and `EXPECT_SPAWN` so the boot line reports `user yes isolated yes ushell yes spawn yes fork yes`, and run the plain `check-disk` pass; `make test-host` reporting 3723 checks, a 276616 B (270 KiB) file and an 846206 B (826 KiB) load image (`.text` 194310 B, `.rodata` 29248 B, `.data` 56 B, `.bss` 622592 B) under the 1024 KiB stretch and 2048 KiB hard budgets; `docs/userspace.md` (fork, the clone cap, the trampoline and the empty descriptor table) plus README, roadmap, versioning, build, boot, traps, hardening, iso, workbench, fuzzing and heap; BootInfo stays v1 and the kernel ABI is unchanged |
| `v0.38.0` | 2026-10-28 | phase U4: spawn, exec and wait in ring 3 (`include/crug/syscall.h`, `include/crug/exec.h`, `include/crug/thread.h`, `kernel/exec/elf_exec.c`, `kernel/exec/argv.c`, `kernel/core/syscall.c`, `kernel/core/thread.c`, `kernel/core/user_test.c`, `user/spawn.c`, `user/child.c`, `scripts/mkdisk.sh`, `scripts/boot-check.py`, `tests/host/test_argv.c`, `Makefile`: `SYS_SPAWN` 21, `SYS_WAIT` 22, `SYS_EXEC` 23 with `SYS_MAX` 24 and `SYS_ESRCH` -3, `elf_spawn_user(path, argc, argv, parent_id, out)` starting a new image as a new ring-3 thread and recording the caller in the new `parent_id` field of `struct thread` while `elf_exec_run()` stays the `argc == 0` wrapper and `elf_exec_run_args()` passes parent id 0 for kernel-owned programs, a scheduler all-threads list that keeps zombies with `thread_find(id)` finding them and monotonic ids so a stale tid never matches a recycled thread, `SYS_WAIT(tid)` blocking until the child exits and returning its code only when `parent_id` is the caller's tid and the target is not the caller (otherwise `SYS_ESRCH`) and reaping through `thread_join_code()`, `elf_exec_in_place()` loading a fresh `vm_space`, pointing the thread's `space`/`user_rip`/`user_rsp` at it, activating the new space before destroying the old one because CR3 still points at the old tables and rewriting the saved trap frame (`rip`, `rsp`, `rax` = 0) so the `iretq` lands in the new image with the tid preserved (serial `CRUGOS: user exec /bin/child (tid 14)`) and `SYS_EINVAL` for a CPL0 caller, `syscall_argv()` copying CPL3 arguments through `uaccess_check` and `uaccess_user_read`/`uaccess_user_str` into `g_argv_buf[EXEC_ARGS_STR_MAX]` with the identity-map `harden_range_ok` plus `memcpy` path for CPL0 and ending in `exec_args_ok(argc, str_bytes)`, a pure `kernel/exec/argv.c` predicate enforcing `EXEC_ARGS_MAX` 16 arguments and `EXEC_ARGS_STR_MAX` 2048 string bytes before an address space is allocated, `user/spawn.c`/`user/child.c` adding `/bin/spawn` (spawns `/bin/child` with `from-spawn`, prints the tid, waits for exit 5, then execs the child with `from-exec` and exits 5) and `/bin/child` (prints its tid, `argc 2` and the argument, exits 5) with `scripts/mkdisk.sh` installing seven programs, `user.test=4` running the U1 trio, the U2 pair, the shell and `user_spawn()` (which joins `/bin/spawn` and requires exit 5), `make test-user4` as the 51st gate -- rebuild the disk with all seven programs, boot under `SPAWN_RUN_TIMEOUT_SEC` 240 and pass boot-check the new final `EXPECT_SPAWN` argument (`argv[30]`, argument guard `tuple(range(3, 32))`) alongside `EXPECT_USER` (`argv[27]`), `EXPECT_USERS` (`argv[28]`) and `EXPECT_USHELL` (`argv[29]`) so the boot line reports `user yes isolated yes ushell yes spawn yes`, then the plain `check-disk` pass with no `--expect-shell-write` clause, `make test-host` at 3670 checks with the new `exec_args_ok` cases in `tests/host/test_argv.c`, and `make size` reporting a 276408 B (269 KiB) file and an 844414 B (824 KiB) load image with `.text` 192902 B, `.rodata` 28864 B and `.bss` 622592 B under the 1024 KiB stretch and 2048 KiB hard budgets; `docs/userspace.md` (spawn, wait, in-place exec and the argv boundary) plus README, roadmap, versioning, build, boot, traps, hardening, iso, workbench, fuzzing and heap; BootInfo stays v1 and the kernel ABI is unchanged |
| `v0.37.0` | 2026-10-28 | phase U3: a ring-3 shell with file syscalls (`include/crug/syscall.h`, `include/crug/ufile.h`, `include/crug/exec.h`, `kernel/fs/ufile.c`, `kernel/exec/argv.c`, `kernel/exec/elf_exec.c`, `kernel/core/syscall.c`, `kernel/core/user_test.c`, `kernel/core/thread.c`, `user/sh.c`, `user/sh.ld`, `user/start.S`, `scripts/mkdisk.sh`, `scripts/boot-check.py`, `scripts/check-disk.py`, `Makefile`: `SYS_OPEN` 17, `SYS_READ` 18, `SYS_CLOSE` 19, `SYS_LISTDIR` 20, `SYS_MAX` 21 and `SYS_ENOENT` -2 with `SYS_WRITE` 16 writing an open file for `fd` 3 and up, the flags `SYS_O_RDONLY` 0, `SYS_O_WRONLY` 1, `SYS_O_CREAT` 2 and `SYS_O_TRUNC` 4, a per-thread `ufile` table of `UFILE_FD_MAX` 8 slots from `UFILE_FD_BASE` 3 with `UFILE_MAX_BYTES` 64 KiB read snapshots and write buffers and `UFILE_PATH_MAX` 64 and `ufile_close_all()` from `thread_destroy()`, a writable close flushing through create, a whole-buffer write or truncate plus fsync and bracketing the filesystem with `begin_write`/`end_write` implemented as `storage_remount_rw()`/`storage_remount_ro()` because the root boots read-only (serial `CRUGOS: storage remounted read-write` then `read-only`), `kernel/exec/argv.c` building the `argc`/`argv`/`envp` block in a 4 KiB `EXEC_ARGS_BLOCK_MAX` area at the top of the user stack with `EXEC_ARGS_STR_MAX` 2048 bytes and `EXEC_ARGS_MAX` 16 arguments at 16-byte alignment, `elf_exec_run_args(path, argc, argv, out)` starting the thread with `rsp` at the `argc` word while `elf_exec_run()` stays the zero-argument wrapper and `user/start.S` loading `rdi`/`rsi`/`rdx`, `user/sh.c`/`user/sh.ld` adding `/bin/sh` at text `0x8000010000` and data `0x8000020000` with `ver`, `echo TEXT`, `cat FILE`, `ls PATH`, `write FILE TEXT` and `exit N`, `argv[1]` or `/etc/sh.rc` as the script and static buffers instead of a ring-3 heap, `scripts/mkdisk.sh` writing the shipped `/etc/sh.rc` whose `write /shout.txt hello-from-user-shell` leaves 22 bytes, `user.test=3` running the U1 trio, the U2 isolation pair and the shell with expected exit 7, `make test-user3` as the 50th gate -- rebuild the disk with all five programs, boot under `SHELL_RUN_TIMEOUT_SEC` 240 and pass boot-check the new final `EXPECT_USHELL` argument (`argv[29]`, argument guard `tuple(range(3, 31))`) alongside `EXPECT_USER` (`argv[27]`) and `EXPECT_USERS` (`argv[28]`) so the boot line reports `user yes isolated yes ushell yes`, then `check-disk --expect-shell-write` asserting `/shout.txt` equals `hello-from-user-shell` plus a newline after a clean umount and `e2fsck`, `make test-host` at 3663 checks, and `make size` reporting a 267976 B (261 KiB) file and an 842046 B (822 KiB) load image with `.text` 190790 B, `.rodata` 28608 B and `.bss` 622592 B under the 1024 KiB stretch and 2048 KiB hard budgets; `docs/userspace.md` (reworked for U3) plus README, roadmap, versioning, build, boot and hardening; BootInfo stays v1 and the kernel ABI is unchanged |
| `v0.36.0` | 2026-10-28 | phase U2: per-process address space (`include/crug/vm.h`, `include/crug/thread.h`, `include/crug/exec.h`, `include/crug/uaccess.h`, `kernel/mm/vm_space.c`, `kernel/mm/vm.c`, `kernel/mm/uaccess.c`, `kernel/exec/elf_exec.c`, `kernel/core/thread.c`, `kernel/core/user_test.c`, `kernel/core/main.c`, `kernel/arch/x86_64/user.S`, `kernel/arch/x86_64/cpu.c`, `kernel/arch/x86_64/idt.c`, `user/iso.c`, `user/iso.ld`, `user/user.ld`, `scripts/mkdisk.sh`, `scripts/boot-check.py`, `tests/host/test_vm_space.c`, `tests/host/test_heap.c`, `Makefile`: `struct vm_space` (`pml4`, `phys`, `user_pages`, `pad`) with `vm_space_set_kernel_root()`/`vm_space_create()`/`vm_space_destroy()`/`vm_space_map_user_pages()`/`vm_space_unmap_user_pages()`/`vm_space_translate()`/`vm_space_activate()`/`vm_kernel_root_phys()`, `kernel/mm/vm_space.c` as a separate host-testable unit needing only `pmm_alloc_page()`/`pmm_free_page()` and the identity translations and never CR3 or the LAPIC, slot 0 of every space copied from the kernel root so the shared supervisor identity map over the kernel image, stacks and MMIO is present everywhere with slot 1 owning the 512 GiB `USER_BASE` `0x0000008000000000`..1 TiB `USER_TOP` `0x0000010000000000` window, mapping upgrading every intermediate entry to `PRESENT OR WRITE OR USER`, clearing `VM_GLOBAL` and forcing `VM_USER` on the leaf, keeping W^X from the caller (`VM_WRITE` plus `VM_NX` on data pages) and validating page alignment, the window and an absent leaf before touching the tables, a mid-range failure rolling back exactly the pages that call mapped, `vm_space_destroy()` freeing every leaf frame, PT, PD, PDPT and the root page, `kernel/mm/vm.c` keeping the active root in a static `g_root` that `pte_for()`/`vm_translate()` follow with `vm_space_activate()` as the only CR3 writer and no write for an unchanged root, `struct thread` carrying `vm_space *space` instead of the U1 `thread_user_map[8]` array, `schedule()` installing the kernel stack top then calling `vm_space_activate(next->space)`, `thread_destroy()` destroying the space, `thread_create_user(name, entry, user_rsp, space, sched_class)` dropping the `-EBUSY` one-user-thread rule, `kernel/exec/elf_exec.c` owning one space per exec with the segment reservations and the four-page guarded stack in it before handing it to the thread, `vm_init` publishing the root immediately after allocating it because the section mapper walks the root (the old order page-faulted before the IDT existed), `user/iso.c`/`user/iso.ld` pinning a writable page at `0x0000008000030000` where two `user.test=2` instances write `0x15500000` combined with the tid shifted left 8 bits, yield 300 times and re-check their own word while `kernel/core/user_test.c` proves the same virtual address maps to different frames (`CRUGOS: user isolation frames 0x28000 vs 0x38000 at 0x8000030000`) and joins both, `make test-user2` as the 49th gate -- rebuild the disk with all four programs, boot with the token under `USER2_RUN_TIMEOUT_SEC` 240 and pass boot-check the new final `EXPECT_USERS` argument (`argv[28]`, argument guard `tuple(range(3, 30))`) alongside `EXPECT_USER` (`argv[27]`) so the boot line reports `user yes isolated yes`, with `make test-user` still gate 48 on `user.test=1` -- `tests/host/test_vm_space.c` covering slot-0 inheritance, the window validation table, raw leaf flags, distinct frames for the same VA, partial rollback, full reclaim and an allocation-failure path that asks for the whole 512 GiB window, `host_pmm_free_pages_count()` in `test_heap.c` exposing leaks, `make test-host` at 3554 checks, and `make size` reporting a 263056 B (256 KiB) file and an 832574 B (813 KiB) load image under the 1024 KiB stretch and 2048 KiB hard budgets; `docs/userspace.md` (reworked for U2) plus README, roadmap, versioning, build, boot, traps, hardening and iso; BootInfo stays v1 and the kernel ABI is unchanged |
| `v0.35.0` | 2026-10-28 | phase U1: ring 3 and the first user program (`include/crug/segment.h`, `include/crug/exec.h`, `include/crug/uaccess.h`, `include/crug/vm.h`, `include/crug/thread.h`, `include/crug/harden.h`, `kernel/arch/x86_64/cpu.c`, `kernel/arch/x86_64/user.S`, `kernel/arch/x86_64/idt.c`, `kernel/mm/vm.c`, `kernel/mm/uaccess.c`, `kernel/exec/elf_exec.c`, `kernel/core/syscall.c`, `kernel/core/thread.c`, `kernel/core/user_test.c`, `kernel/core/main.c`, `user/*`, `scripts/mkdisk.sh`, `scripts/boot-check.py`, `tests/host/test_uaccess.c`, `tests/host/test_exec.c`, `Makefile`: `SEG_KERNEL_CS` 0x08 / `SEG_KERNEL_DS` 0x10 / `SEG_TSS` 0x18 / `SEG_USER_CS` 0x2B / `SEG_USER_DS` 0x33, `GDT_ENTRIES` 7, a 64-bit TSS over the phase H3 IST1..IST4 stacks, `cpu_set_kernel_stack()` as the only writer of `rsp0` with `schedule()` installing the incoming thread's kernel stack top (the boot stack top for kernel threads), the fixed user window `USER_BASE` `0x0000008000000000`..`USER_TOP` `0x0000010000000000` (PML4 slot 1, above the supervisor-only identity map), `vm_user_map_page()`/`vm_user_map_pages()`/`vm_user_unmap_pages()` creating every intermediate PDPTE/PDE with `VM_USER`, stripping `VM_GLOBAL` and taking W^X from the caller (`VM_NX` on data pages), `ELF_MAX_BYTES` 512 KiB, `USER_STACK_PAGES` 4 and `USER_STACK_TOP` `0x0000008040000000`, `elf_exec_run()` reusing `boot/efi/elf.c`, reserving executable segments without `VM_WRITE`/`VM_NX` and data segments `VM_WRITE` and `VM_NX`, copying through `pmm_phys_to_virt()`, ignoring empty `PT_LOAD`s and validating segments, entry and stack with `harden_user_segment_ok()`/`harden_user_entry_ok()`, `uaccess_check()`/`uaccess_user_read()`/`uaccess_user_write()`/`uaccess_user_str()` with pure `harden_user_range_ok()` arithmetic, a mapped-page walk returning `UACCESS_EFAULT` (-14) and `stac`/`clac` around the copy only when CPUID exposes SMAP, `from_user()` testing `frame->cs & 3` so a CPL0 selftest keeps the identity-map path and a CPL3 caller uses bounce buffers, `SYS_WRITE` 16 (fd 1/2, at most 4096 bytes in 256-byte chunks), `SYS_MAX` 17, `SYS_EBUSY` -16 and a CPL3 `SYS_EXIT` that ends only the calling thread, `THREAD_FLAG_USER` threads carrying `user_rip`/`user_rsp` and up to `THREAD_USER_MAP_MAX` 8 mapped regions, `thread_create_user()` returning `-EBUSY` for a second live user thread, `thread_destroy()` unmapping the recorded pages, `thread_user_start` building the `iretq` frame (`SS` 0x33, user `RSP`, `RFLAGS` 0x202, `CS` 0x2B, entry `RIP`), a CPL3 fault printing the H3 report plus `CRUGOS: user fault <name> at rip=... err=... tid N` and `CRUGOS: user fault killed tid N code -<vector>` then killing only that thread via `thread_kill_current()` while user `#DF`/`#NMI`/`#MC` and every CPL0 fault still panic, the freestanding programs `user/hello.c`, `user/badptr.c` and `user/fault.c` linked by `user/user.ld` (base `0x8000010000`, R+X text and R+W data `PT_LOAD`s) against the inline `int $0x80` wrappers in `user/lib.h` (`nr` in `rax`, arguments in `rdi`/`rsi`/`rdx`/`r10`), `make user` building them into `build/x86_64/user/` and `scripts/mkdisk.sh` writing them into `/bin` via `USER_DIR`, `user.test=1` running the three programs in sequence with exit codes 0, 0 and -14, and `make test-user` as the 48th gate -- rebuild the disk with the programs, boot with the token under `USER_RUN_TIMEOUT_SEC` 240 and pass boot-check the new final `EXPECT_USER` argument (`argv[27]`) that fails any other gate printing a user marker -- with `make test-host` at 3470 checks from `tests/host/test_uaccess.c` and `tests/host/test_exec.c` and `make size` reporting a 262760 B (256 KiB) file and an 831006 B (811 KiB) load image; `docs/userspace.md` plus README, roadmap, versioning, build, boot, traps, hardening, iso, fuzzing and workbench; BootInfo stays v1 and the existing kernel ABI entries are unchanged, with `SYS_WRITE` 16 added to the syscall table |
| `v0.34.0` | 2026-10-28 | phase H4: host mount-path fuzzer (`tests/host/fuzz_mount.c`, about 890 lines, plus `tests/host/Makefile`'s `FUZZ_SOURCES` and `fuzz` target and `make test-fuzz`: the harness links the real `kernel/fs/ext4.c`, `kernel/fs/gpt.c`, `kernel/fs/journal.c`, `kernel/block/block.c`, `kernel/block/crypt_meta.c`, `kernel/mm/heap.c`, `kernel/lib/*` and `kernel/crypto/*` against host stubs for `kprintf()` (silent unless `--verbose-kernel`), `panic()` (exit 97) and a `pmm_*` allocator over a 32 MiB static arena, serves a `scripts/mkdisk.sh` image through a bounds-checked in-memory block device, finds `crugroot` with the real `gpt_parse()` (callers test `< 0`: it returns the partition count) and weights six mutation regions -- superblock 22, group descriptors 14, inode table 16, root directory block 16, a 64 KiB metadata window 17 and the whole partition 15; each case forks a child on a copy-on-write image, applies one to four of eight mutations (one-byte XOR, `0xFF` fill, zero fill, `u16`/`u32`/`u64` write, a 16-byte splice, a 16-byte random fill) from edge-weighted values seeded by `--seed`, then walks `gpt_parse()` -> `ext4_mount()` (25% `EXT4_MOUNT_RW`) -> `ext4_lookup()`/`ext4_list()`/`ext4_read_file()` -> the create/write/truncate/fsync/mkdir/rename/unlink write path -> `ext4_umount()` and encodes the depth reached in its exit code (0 rejected, 1 mounted, 2 walked a path, 3 wrote), while a signal, the `PANIC_EXIT` 97 or an exit above 3 prints the plan, dumps `fuzz-ext4-<N>.img` (or the 256-byte `fuzz-crypt-<N>.img`) under `--crash-dir` and fails the case, `--selftest` forks a null dereference to prove the detector, `control_mount()` requires the pristine image to mount and read `/hello.txt`, and the crypt leg mutates the 256-byte `struct crug_crypt_header` and calls `crypt_meta_validate()`/`crypt_meta_scratch_bytes()` without unwrapping a keyslot; `make test-fuzz` is the 47th gate -- rebuild `disk.img` and `crypt.img`, `make -C tests/host fuzz`, the self-test, then `FUZZ_CASES` 20000 ext4 cases plus `FUZZ_CRYPT_CASES` 2000 header cases with `FUZZ_SEED` 1337 and `FUZZ_TIMEOUT_SEC` 10 in about 23 seconds with no QEMU boot and no kernel version line, reporting about 10580 cases mounted, 10535 that walked a path, 2594 that wrote and 0 crash(es), with seeds 1337, 7, 424242 and 999983 all clean; `make test-host` still reports 3400 checks, the kernel is still a 253144 B (247 KiB) file and an 821986 B (802 KiB) load image, and `docs/fuzzing.md` documents the harness, its limits and the `gpt_parse()` bring-up bug (testing for 0 instead of `< 0`); BootInfo stays v1 and the kernel ABI is unchanged |
| `v0.33.0` | 2026-10-28 | phase H3: trap policy and crash reports (`include/crug/trap.h`, `kernel/lib/trapfmt.c`, `kernel/arch/x86_64/idt.c`, `kernel/arch/x86_64/linker.ld`, `kernel/arch/x86_64/cpu.c`, `kernel/core/trap_test.c`: `TRAP_IST_DF` 1 / `TRAP_IST_NMI` 2 / `TRAP_IST_MC` 3 / `TRAP_IST_DB` 4, `enum trap_class { TRAP_HANDLED = 0, TRAP_USER = 1, TRAP_FATAL = 2 }`, `struct trap_report_info { u64 vector; u64 error; u32 ist; u64 cr2; int has_cr2; const char *name; }`, `trap_classify()` (vector, cs), `trap_ist_for()`, `trap_format(frame, info, out, cap)`, `trap_cmdline()`, `trap_test_run()`; the pure formatter writes a `CRUGOS: trap report <name> vector=<dec> error=0x<hex> ist=<dec> from=<kernel|user>` head, the saved `rip`/`cs`/`rflags`/`rsp`/`ss` line, a `#PF`-only `present`/`write`/`user`/`reserved`/`instruction`/`cr2` line (error bits 0 present, 1 write, 2 user, 3 RSVD, 4 instruction fetch) and two register lines, all lowercase and unpadded in at most 1024 bytes; `page_fault_panic()` is gone and every vector except the timer (32), the syscall gate (128) and `#BP` (still counted in `g_int3_seen`) builds a report, `console_write()`s it and classifies it -- `#BP` `TRAP_HANDLED` in both rings, `#OF` handled only for a user frame, the user-killable set {0, 1, 6, 11, 12, 13, 14, 16, 17, 19, 21} `TRAP_USER` when `cs & 3 == 3` and `TRAP_FATAL` from ring 0, everything else (`#NMI`, `#DF`, `#MC`, 5, 7, 9, 10, 15, 20, 22..31 and >= 32) fatal in both rings, a `TRAP_USER` fault panicking as `user-mode <name>` until ring 3 exists; `__df_stack` grown to 16 KiB and `__nmi_stack`/`__mc_stack`/`__db_stack` added (8 KiB each, +32 KiB of `.bss` after the boot stack) and loaded into TSS `ist1`..`ist4` while `rsp0` keeps pointing at the boot stack; `trap.test=<recipe>` (`de`, `ud`, `gp`, `pf`, `df`, `nmi`, `mc`, `db`, at most 16 characters, an unknown value printing `CRUGOS: trap test unknown <value>`) injecting exactly one fault as the last action before `arch_exit_qemu(0x10)` after `CRUGOS: phase h3 boot ok`; `make test-trap` the 46th gate (`scripts/run-trap.py`: nine boots, control plus eight recipes, from per-case disk copies under `build/x86_64/trap/` with `CMDLINE_EXTRA='trap.test=<recipe>'`, `QEMU_DISPLAY=none`, `NET_MODE=none` and `TRAP_TIMEOUT_SEC` 60, asserting the recipe marker, the report vector/ist/from=kernel fragments, `CRUGOS PANIC: <name>` and `CRUGOS: panic` with exit 35 while the control exits 33 with the phase marker and no panic), `tests/host/test_trap.c` (classification matrix, IST routing, the page-fault fault line, the no-fault-line path, a user frame and the truncation bound), 3400 host checks, kernel 253144 B (247 KiB) / load image 821986 B (802 KiB) and docs/traps.md; BootInfo stays v1 and the kernel ABI is unchanged |
| `v0.32.0` | 2026-10-28 | phase H2: hardened kernel heap (`kernel/mm/heap.c` + `include/crug/heap.h`: a `u64` cookie at the start of every slab page and every large-block header, keyed by the page address (`HEAP_COOKIE_KEY` `0x435255474f5348ull` XOR the page XOR `SLAB_COOKIE_SALT` `0x534c4142ull` for slabs or `LARGE_COOKIE_SALT` `0x4c415247ull` for large blocks); `struct slab_header` stays 64 bytes (`u64 cookie; u16 class_idx, objects, free_count, in_use; u64 used[4]; void *free_list; struct slab_header *next;`) and `slab_validate()` checks the cookie, class, object counts, `free_count + in_use == objects`, the bitmap past `objects`, every free-list entry inside the page and unallocated, the listed count against `free_count` and a cycle cap of `objects`, panicking `heap: corrupt slab header` or `heap: corrupt slab free list`; `struct large_header` (`u64 cookie, size, pages; void *next, *prev; u64 reserved[3];`) with a doubly linked list, `prev` checks and `large_contains()` turning a `free()` of an interior large pointer into `heap: free of interior large pointer`; `heap_check(u32 *slabs, u32 *objects, u32 *large)` with `HEAP_CHECK_OK` 0 / `SLAB_COOKIE` 1 / `SLAB_CLASS` 2 / `SLAB_COUNTS` 3 / `SLAB_BITMAP` 4 / `SLAB_FREE_LIST` 5 / `LARGE_COOKIE` 6 / `LARGE_FIELDS` 7 / `PAGE_TOTALS` 8, `heap_check_name()` (`ok`, `slab cookie`, ...), a `g_slab_pages`/`g_large_pages`/`g_large_bytes` cross-check and `cpu_irq_save()`/`cpu_irq_restore()` around the walks; `heap_stress(u32 ops)` (55% alloc / 45% free, `HEAP_STRESS_LIVE` 64, 1..8000 B, tag verification, a check every 64 operations, a final exact `heap_used_bytes()` accounting check, `HEAP_STRESS_MAX` 200000) and `heap_guard()` (a bad free-list link, a zeroed slab cookie and a zeroed large-block cookie, proving codes 5, 1 and 6 then repairing them); `heap_cmdline()` parsing `heap.stress=<1..200000>` and `heap.guard=1` right after `heap_init()` and `heap_selftest()` printing `CRUGOS: heap check ok (6 slab(s), 8 object(s), 1 large block(s))` plus the freed-16-byte reuse check; `include/crug/cpu.h` under `CRUG_HOST_TEST` (`cpu_irq_save()` returns `(1ull << 9)`, restore/disable are no-ops) so `kernel/mm/heap.c` compiles into `tests/host`, where the new `tests/host/test_heap.c` supplies a fake 2048-page 8 MiB `.bss` arena with its own bitmap, mirrors `host_slab`/`host_large` to corrupt real metadata and covers every code, `heap_stress(2000)`, `heap_guard()` and every `heap_check_name()` string (the `pmm_alloc_pages`/`pmm_free_pages` and `kzalloc` stubs moved out of `test_argon2.c`/`test_handle.c`); `make test-heap` as the 45th gate with `EXPECT_HEAP`/`HEAP_STRESS_OPS` 4000/`HEAP_RUN_TIMEOUT_SEC` 240, booting `heap.stress=$(HEAP_STRESS_OPS) heap.guard=1` and passing `EXPECT_HEAP` in the 20th boot-check positional slot before `scripts/check-disk.py`; 3235 host checks, `kernel` 244264 B (238 KiB) / load image 783106 B (764 KiB) and docs/heap.md; BootInfo stays v1 and the kernel ABI is unchanged |
| `v0.31.0` | 2026-10-28 | phase H1: hardening pass 1 (`include/crug/harden.h` + `kernel/lib/harden.c`: seven pure validators `harden_range_ok()` (a buffer inside the 4 GiB identity map, no wrapping addition), `harden_payload_ok()` (a NULL payload with a non-zero length), `harden_extent_header_ok()` (ext4 magic `0xF30A`, entries, depth at most 5 and the real node size, 60 bytes for the inline `i_block` root and `block_size - 12 - csum` for a leaf), `harden_ext4_geometry_ok()` (power-of-two block size of at least 1024, non-zero counts and `s_blocks_per_group`/`s_inodes_per_group` at most `block_size * 8`), `harden_nvme_doorbell_ok()` (stride a power of two of at least 4 and `0x1000 + (2*qid + cq) * stride + 4` at most `bar_size`), `harden_kdf_cost_ok()` (lanes 1..16, `m >= 8 * lanes`) and `harden_volume_ok()` (`offset <= parent`, `volume <= parent - offset`), host-tested in `tests/host/test_harden.c`; `kernel/core/syscall.c`: `SYS_PORT_SEND` validates the `rdx`/`r10` buffer and `SYS_PORT_RECV` validates that buffer plus a non-zero `rsi` as an 8-byte writable range against `HARDEN_IDENTITY_LIMIT`, returning `SYS_EFAULT` (`-14`, `include/crug/syscall.h`), `msgport_send()` rejects `data == 0` with `len > 0` and the self-test prints `CRUGOS: syscall selftest failed (pointer gate)`; `kernel/fs/ext4.c`: geometry checked before the `metadata_csum` superblock checksum (`CRUGOS: ext4 mount rejected (geometry)`) and the root inline extent after `read_inode()` (`CRUGOS: ext4 mount rejected (root extent)`), `extent_map()`/`extent_add()` validating every header against the bytes the node owns and `free_block()` re-checking `g >= groups || bit >= blocks_per_group`, both rejections failing `storage_init()` (`CRUGOS: no root filesystem`, exit 35); `drivers/block/nvme.c`: a `CAP.DSTRD` that puts a doorbell past `bar_size` is refused at probe (`nvme   : doorbell stride outside BAR0`) instead of writing up to ~192 KiB past BAR0; `kernel/block/crypt.c`: unlock-time KDF policy 1024..65536 KiB / `t` 2..8 / lanes 1..16 / `m >= 8 * lanes` under the unchanged format ceiling (1 GiB / `t` 64, `tools/mkcrypt` unchanged) and a volume bound against the parent partition, both refusals wiping the 16 KiB header copy (`CRUGOS: crypt unlock failed (kdf policy)` / `CRUGOS: crypt unlock failed (volume exceeds partition)`); `kernel/lib/string.c`: `secure_zero()` volatile wipe used by the crypt header/bounce/scratch/master paths and `storage_wipe()`, and `boot/efi/main.c` zeroing the loader's `cmdline.txt` pool copy so `crypt.pass=` does not survive in USABLE memory; `scripts/run-hardening.py` behind `make test-hardening` (44th gate, seven cases: an unpatched control plus six crafted images, `HARDEN_TIMEOUT_SEC` 120, exit 35 with no panic and a control boot at exit 33), 3200 host checks, `kernel` 239840 B (234 KiB) / load image 778018 B (759 KiB) and docs/hardening.md; BootInfo stays v1 and the kernel ABI is unchanged |
| `v0.30.0` | 2026-10-28 | phase 6H: drawer row selection and a draggable scrollbar (`include/crug/intuition.h`: `INTUITION_BODY_PRESS` 0 / `INTUITION_BODY_MOVE` 1 / `INTUITION_BODY_RELEASE` 2 and `int intuition_set_body_input(int id, void (*fn)(int id, u32 kind, i32 x, i32 y, void *ctx), void *ctx)` receiving body-relative coordinates, `NULL` clearing the handler and an unknown id returning -1; `kernel/gfx/intuition.c`: an internal body window captured on a press inside a body with a handler, `PRESS` forwarded with a repaint and a return before any title-bar drag, resize or gadget press, every motion while held forwarded as `MOVE`, `RELEASE` forwarded with a repaint and a return before the icon, resize, gadget and drag branches, and the new `body_rect()` helper sharing its `x + BORDER`, `y + BORDER + TITLE_HEIGHT`, `w - 2 * BORDER` by `h - 2 * BORDER - TITLE_HEIGHT` rectangle with `win_draw_body()`; `kernel/gfx/workbench.c`: `wb.dsel=1` (parsed before `wb.shell=`) giving the (560,60,360x260) drawer's 356x236 body a handler, 29 entries over 11 visible rows on a 20-pixel pitch with max top row 18, a click at body `y` selecting index `top + (y - 6) / 20` and painting an amber `INTUITION_MENU_HIGHLIGHT_RGB` `0x00FFB000` band with window-background text (`CRUGOS: wb drawer selected '<name>' (index N of M)`), a 4-pixel `WB_TRACK_W` track at body-local `x = body width - 8`, `y + 6`, `h - 12` with a `max(12, track * rows / entries)` thumb proportional to the first visible row whose track press jumps the view and grabs the thumb, whose drag follows the pointer and whose release prints `CRUGOS: wb drawer scrollbar to (top N of M)`, the `CRUGOS: wb select mode (click a row, drag the scrollbar)` marker after the desktop-ready line, the `wb drawer scrollbar grabbed` marker and the `wb.acts=` select self-test printing `CRUGOS: wb select selftest ok (2 select(s), 1 scrollbar drag(s))` or `wb select timeout (N/M)`, on top of the phase 6G `wb.fill=N` root fill; `scripts/run-workbench-select.py` behind `make test-workbench-select` (gate 43: `wb.enable=1 wb.wait=30 wb.desk=1 wb.dr=1 wb.dsel=1 wb.fill=24 wb.acts=3`, row 0 dark `(8,16,24)`, a (600,136) click selecting row 2 with an amber `(255,176,0)` band at (890,136) and index 2 of 29, a (911,100) thumb press dragged to (911,280) reporting top 18 of 29 and clearing row 2's band, a (600,96) click selecting index 18, exit status 33, `EXPECT_SELECT`/`WB_SELECT_WAIT`/`WB_SELECT_ACTS`/`WB_SELECT_FILL`/`WB_SELECT_RUN_TIMEOUT_SEC` make options, the 20th `EXPECT_` boot-check positional and a clean `e2fsck`); `tests/host/test_intuition.c` `test_intuition_body_input` group, 3104 host checks and `CRUGOS: phase 6h boot ok`; BootInfo stays v1 and the kernel ABI is unchanged |
| `v0.29.0` | 2026-10-28 | phase 6G: drawer scrolling, refresh and reflow (`include/crug/workbench.h` + `kernel/gfx/workbench.c`: `WB_DRAWER_ROW_H` 20, `WB_DRAWER_MAX` 40 and `WB_DRAWER_NAME_MAX` 32, `wb_drawer_rows(h)` returning 0 for a body up to 22 pixels and `(h - 22) / 20 + 1` above it (360x260 -> 236-pixel body -> 11 rows, 460x360 -> 336 -> 16 rows, so a resize reflows the list), `wb_drawer_scroll(delta)` moving the top row by one and clamping it to `[0, count - rows]` (a press at either end returns 0 and counts nothing), `wb_drawer_refresh()` re-reading `ext4_list()` and clamping again, a 4-pixel scrollbar track at `x + w - 8` from `y + 6`, `h - 12` tall in `0x00404040` with a thumb of `max(12, track_h * rows / entries)` in the menu background colour that follows the top entry and is painted only while `entries > rows`, `CRUGOS: wb drawer mode (arrow keys scroll, r refreshes)` / `wb drawer ready (N entries, R row(s))` / `wb drawer scrolled (top N of M)` / `wb drawer refreshed (N entries)` markers and the `wb.acts=` drawer self-test with `wb drawer selftest ok (N scroll(s), M refresh(es))`; `wb.dr=1` giving the open drawer Up/Down and `r`/`R` (the icon table keeps the keys while the drawer is closed), `wb.fill=N` (0..24) creating `/f00`..`/f(N-1)` once before the listing loads with `CRUGOS: wb fill N file(s)`; `kernel/fs/storage.c` + `include/crug/storage.h`: `storage_remount_rw()`/`storage_remount_ro()` umounting and remounting the read-only boot root around a short write window (`CRUGOS: storage remounted read-write`/`remounted read-only`), used by the drawer fill and by `kernel/shell/shell_ops.c` `ops_write`, whose `write` command had been failing silently against the read-only mount, with `ops_sync` returning 0 while the volume is not mounted read-write; `scripts/run-workbench-drawer.py` behind `make test-workbench-drawer` (`wb.enable=1 wb.wait=30 wb.desk=1 wb.dr=1 wb.fill=24 wb.acts=4`, a double click opening 29 entries and 11 rows, three Down presses each with a scrolled marker and more than 200 changed pixels in the body, the grip dragged from (904,304) to (1004,404) resizing to 460x360 with desktop `(16,32,48)` at (830,400) becoming window `(8,16,24)` and at least 200 body-text pixels, `r` refreshing, exit status 33 and a clean `e2fsck`, plus the `EXPECT_DRAWER`/`WB_DRAWER_WAIT`/`WB_DRAWER_ACTS`/`WB_DRAWER_FILL`/`WB_DRAWER_RUN_TIMEOUT_SEC` make options and the `EXPECT_DRAWER` boot-check positional); `tests/host/test_intuition.c` `test_intuition_paint`, 3075 host checks and docs/workbench.md; BootInfo stays v1 and the kernel ABI is unchanged |
| `v0.28.0` | 2026-10-28 | phase 6F: icon dragging, send-to-front and keyboard icons (`include/crug/intuition.h` + `kernel/gfx/intuition.c`: `INTUITION_ICON_DRAG_MIN` 4, `INTUITION_EVENT_ICON_MOVE` (type 9) carrying the object index and its new origin, `INTUITION_EVENT_FRONT` (type 10), `intuition_icon_moves()`/`intuition_front_count()`/`intuition_icon_key(u8)`, a press selecting and grabbing a tile after a 4-pixel INTUITION_ICON_DRAG_MIN dead zone, motion clamped to `[0, screen width - 64]` x `[0, screen height - 48]`, the release queueing `ICON_MOVE` and printing `CRUGOS: wb icon '<label>' dropped at (x, y)` with the amber frame kept, a 12x12 `INTUITION_FRONT_SIZE` box at `close_box_x - 32` raising its window and queueing `FRONT` (`CRUGOS: wb window N sent to front`, a no-op on the frontmost window), five-frame window repaints; `kernel/gfx/workbench.c`: `wb.ikeys=1` parsed before `wb.desk=` and ignored while `wb.shell=1` owns the keyboard, Tab/Right/Down and Left/Up wrapping the icon selection, Enter selecting then activating (the `System:` drawer) and Escape clearing it, `CRUGOS: wb icon '<label>' focused`/`wb action N icon I (keyboard)`/`wb icon activated (keyboard)`/`wb icons ready (N object(s), keyboard focus)`/`wb icons selftest ok (N move(s), M front(s))` markers and window 1 body `phase 6f`; `scripts/run-workbench-icons.py` behind `make test-workbench-icons` (QMP hotplug of `usb-mouse` + `usb-kbd`, the keyboard activation, a (16,240) to (168,376) tile drag with screendump checks of the amber frame, the window body and the desktop, the depth box sending window 1 back and the front gadget raising it, exit status 33, `EXPECT_ICONS`/`WB_ICONS_WAIT`/`WB_ICONS_ACTS`/`WB_ICONS_RUN_TIMEOUT_SEC` make options and the `EXPECT_ICONS` boot-check positional); `tests/host/test_intuition.c` icon-drag, front and icon-key groups, a 75-frame window repaint assertion, 3054 host checks and docs/workbench.md; BootInfo stays v1 and the kernel ABI is unchanged |
| `v0.27.0` | 2026-10-28 | phase 6E: desktop objects and window depth (`include/crug/intuition.h` + `kernel/gfx/intuition.c`: `INTUITION_MAX_ICONS` 8, a 64x48 `INTUITION_ICON_WIDTH`/`INTUITION_ICON_HEIGHT` tile with one `INTUITION_ICON_LABEL_GAP` 4-pixel label line drawn on the background under every window and clamped so all its pixels stay on screen, kinds `INTUITION_ICON_VOLUME`/`_DRAWER`/`_FILE`, palette `INTUITION_ICON_RGB` `0x00c0c0c0` / `_INNER_RGB` `0x00081018` / `_TEXT_RGB` `0x00e0e0e0` / `_SELECTED_BORDER_RGB` `0x00ffb000`, `intuition_add_icon()`/`_icon_rect()`/`_icon_selected()`/`_icon_count()`/`_icon_clicks()`, a press selecting a tile and queueing `INTUITION_EVENT_ICON` (type 7) with the index and tile origin, a 12x12 `INTUITION_DEPTH_SIZE` box four pixels left of the close box sending a window to the bottom of the stacking order and queueing `INTUITION_EVENT_DEPTH` (type 8) then `ACTIVATE` with `intuition_depth_count()` (backmost no-op), body painters through `intuition_set_paint()`/`intuition_redraw_window()`; `kernel/gfx/workbench.c` + `include/crug/workbench.h`: `wb.desk=1` creating a `System:` volume object at (16,240) with `CRUGOS: wb icon ready ('System:', volume)`, a double click within `WB_DOUBLE_CLICK_NS` 600000000 ns opening a 360x260 `System:` drawer whose painter lists at most 10 mounted ext4 root names (`CRUGOS: wb drawer 'System:' opened (N entries)`, `no volume` without a root), `CRUGOS: wb icon '<label>' selected`/`wb window N sent to back`/`wb action N icon|depth`/`wb desk selftest ok (N drawer(s))` markers and window 1 body `phase 6e`; `scripts/run-workbench-desk.py` behind `make test-workbench-desk` with screendump checks of the idle, selected, drawer and depth pixels, exit status 33, `EXPECT_DESK`/`WB_DESK_WAIT`/`WB_DESK_ACTS`/`WB_DESK_RUN_TIMEOUT_SEC` make options and the `EXPECT_DESK` boot-check positional; `tests/host/test_intuition.c` desktop-object and depth-gadget groups, 2957 host checks and docs/workbench.md; BootInfo stays v1 and the kernel ABI is unchanged |
| `v0.26.0` | 2026-10-28 | phase 6D: window resizing and keyboard navigation (`include/crug/input.h`: `INPUT_KEY_UP` `0x90` / `INPUT_KEY_DOWN` `0x91` / `INPUT_KEY_LEFT` `0x92` / `INPUT_KEY_RIGHT` `0x93`; PS/2 set-1 E0 make codes `0x48`/`0x50`/`0x4B`/`0x4D` and USB HID usage page 0x07 usages `0x52`/`0x51`/`0x4F`/`0x50` (Right `0x4F` before Left `0x50`) into `input.device`, E0 break codes and E0 modifier releases ignored; `include/crug/intuition.h` + `kernel/gfx/intuition.c`: `INTUITION_RESIZE_SIZE` 12 grip in the inner bottom-right corner at `(x + w - INTUITION_BORDER - 12 - 2, y + h - INTUITION_BORDER - 12 - 2)`, live sizing from the size at press plus the pointer delta, `INTUITION_MIN_WIDTH`/`INTUITION_MIN_HEIGHT` 120x80 floor, a motion past the screen refused rather than clamped, release marker `CRUGOS: wb window %d resized to %ux%u` + `intuition_resize_count()` + one action + `INTUITION_EVENT_RESIZE` (type 6) carrying `u32 w`/`u32 h` in `struct intuition_event`; `intuition_key_event(u8)`: Tab cycles focus through the stacking order (raise, activate, queue `ACTIVATE`), `INTUITION_KEY_STEP` 8 arrow moves clamped to the screen, Enter opens the first screen-top menu as a popup with item 0 highlighted, Down/Up move the highlight, Enter selects through the normal `MENU` path and Escape closes the popup; `kernel/gfx/workbench.c` + `include/crug/workbench.h`: `wb.keys=1` keys mode (`wb.shell=1` wins), `CRUGOS: wb keys ready (window 2 active)`, `wb.acts=` counts resizes, drain marker `CRUGOS: wb action %u resize window %d (%ux%u)`, window 1 body `phase 6d`; `scripts/run-workbench-keys.py` behind `make test-workbench-keys` (grip drag (790,490)->(730,430) shrinking window 2 from 320x200 to 260x140 with a screendump check of the freed corner at (798,498), then Tab/Right/Right/Enter/Down/Enter on a hotplugged `usb-kbd`, exit status 33, `EXPECT_KEYS`/`WB_KEYS_WAIT`/`WB_KEYS_ACTS`/`WB_KEYS_RUN_TIMEOUT_SEC` make options), `tests/host/test_input.c` + `tests/host/test_usb.c` arrow checks and `tests/host/test_intuition.c` resize-grip and keyboard/menu-popup groups (2862 host checks) and docs; BootInfo stays v1 and the kernel ABI is unchanged |
| `v0.25.0` | 2026-10-28 | phase 6C: the Workbench shell (`include/crug/textgrid.h` + `kernel/lib/textgrid.c`: `TG_LINES` 96-line ring of `TG_COLS` 38-column NUL-terminated lines (37 usable), `\n`/`\r`/backspace/`.` control mapping, tail-preserving overwrite, oldest-line eviction on overflow, `tg_draw()` painting `min((h-6)/20, 8)` lines at a 20-pixel pitch with a clipped inverse-video cursor cell; `include/crug/shell.h` + `kernel/shell/shell.c`: 79-character line editor, `Ctrl-U`/`Ctrl-C`/`Ctrl-P`/`Ctrl-N` with an eight-entry history that restores the draft, a tokenizer folding extra words into the last argument, the command table `help`/`ver`/`mem`/`uptime`/`ps`/`ls`/`cat`/`echo`/`write`/`sync`/`cls` over a five-call `struct shell_io`; `kernel/shell/shell_ops.c` binding `list`/`read`/`write`/`sync`/`info` to `ext4_list`/`ext4_read_file`/`ext4_lookup`+`ext4_create`+`ext4_write_file`/`ext4_fsync` on `storage_root()`, the heap counters, `sched_current()` and `sched_switches()`; `intuition_set_paint()`/`intuition_redraw_window()` body painters; `kernel/gfx/workbench.c` + `include/crug/workbench.h`: `wb.shell=1`/`wb.sh=` tokens, grid banner, keyboard routing through `input_try_getc()`, `CRUGOS: wb shell ready (window 2, 37x8 text)`, `CRUGOS: sh run '<line>'`/`CRUGOS: sh out <text>` markers, `sh command timeout`, `sh selftest ok`; `scripts/run-workbench-shell.py` typing four commands through a hotplugged `usb-kbd` with screendump body checks behind `make test-workbench-shell`, `EXPECT_SHELL`/`WB_SHELL_WAIT`/`WB_SHELL_CMDS`/`WB_SHELL_RUN_TIMEOUT_SEC` make options, `tests/host/test_textgrid.c` + `tests/host/test_shell.c` (2792 host checks) and docs/workbench.md; BootInfo stays v1 and the kernel ABI is unchanged |
| `v0.24.0` | 2026-10-28 | phase 6B: menus, gadgets and the Intuition event queue (`include/crug/intuition.h` + `kernel/gfx/intuition.c`: `intuition_set_menus()` with two menus of six items, an 18 px menu bar at `x = 4`, 150-px popups at `(strip_x, 18)` with 18-px rows, hover highlight and selection, `intuition_add_gadget()` body-relative gadgets clamped to the body with a 16x16 minimum, a 12x12 close gadget that closes on press, press precedence popup row > menu bar > close gadget > body gadget > title drag, `ACTIVATE`/`DRAG`/`MENU`/`GADGET`/`CLOSE` events in a 16-entry FIFO dropping the newest with `intuition_event_dropped()`, `intuition_event_poll()`/`menu_open()`/`menu_hover()`/`menu_opens()`/`gadget_clicks()`/`close_count()`/`action_count()`/`event_count()`, an untitled menu entry ends the table instead of crashing the bar; `kernel/gfx/workbench.c` + `include/crug/workbench.h`: `wb.menu=` / `wb.acts=` tokens, two menu tables and a `Run` gadget, queue drain into `wb action N menu|gadget|close ...`, `wb_selftest` waits for drags and actions, `wb.wait=0` disables both; `scripts/run-workbench-menu.py` driving six QMP clicks with screendump checks of the highlighted popup row, the desktop background after a close and the white frame of the surviving window, `EXPECT_MENU`/`WB_MENU_WAIT`/`WB_MENU_ACTS`/`WB_MENU_RUN_TIMEOUT_SEC` make options, `tests/host/test_intuition.c` menu/gadget/close/event-queue groups (2639 host checks) and docs/workbench.md; BootInfo stays v1 and the kernel ABI is unchanged |
| `v0.23.1` | 2026-10-28 | packaging: bootable hybrid UEFI ISO (`scripts/mkiso.sh` + `scripts/mkfat.py` + `xorriso`: El Torito no-emulation entry, deterministic FAT16 ESP image with 8.3 enforcement and `--list`/`--read`, `ISO_CMDLINE` rewritten into the staged ESP, protective MBR with a type-`0xEF` entry, `ISO_IMAGE`/`ISO_STAGE`/`ISO_FAT_IMG`/`ISO_SIZE_MB`/`ISO_VOLID`/`ISO_CMDLINE`/`ISO_DATE`/`ISO_RUN_TIMEOUT_SEC`/`EXPECT_ISO`/`ISO_IMG`/`USB_IMG`/`USB_STICK`/`USB_DEV`/`USB_CONFIRM` make options), kernel `storage=none` command-line token (`CRUGOS: storage skipped (storage=none)`, storage self-tests soft-skipped, still fail-closed without the token), `scripts/check-iso.py` (ISO 9660 PVD, El Torito boot record and catalog checksum, FAT16 BPB, protective MBR, `xorriso -osirrox` extraction compared with the catalog range, `fsck.vfat -n`, file set and command line), `make test-iso` booting the ISO as `usb-storage` on `qemu-xhci` and as a CD-ROM with `boot-check.py --storage=none`, `make usb` with `USB_DEV`/`USB_CONFIRM` guards, optional `xorriso`/`fsck.vfat` preflight notes and docs/iso.md; BootInfo stays v1 and the kernel ABI is unchanged |
| `v0.23.0` | 2026-10-28 | phase 6A: Workbench desktop foundation (`kernel/gfx/intuition.c` + `include/crug/intuition.h`: four window slots with a stacking order, top-down title-bar hit tests, raise/activate, grab-offset dragging with screen clamping, close gadget, 47-char body lines, full-desktop repaint, gate-mode `wb window N opened` / `active` / `drag start` / `drag to` / `moved to` markers; `kernel/gfx/workbench.c` + `include/crug/workbench.h`: `wb.enable=` / `wb.wait=` / `wb.drag=` tokens, text console released through `fb_set_text_enabled(0)`, two demo windows, `usb_poll` + `gfx_pointer_poll` as the console poll hook, silent `wb_selftest` unless enabled, 1.5 s capture linger; `gfx.device` `gfx_set_pointer_hook()` so the desktop sees the same deltas as the sprite; `kernel/arch/x86_64/fb.c` + `include/crug/fb.h`: `fb_fill_rect`, `fb_frame_rect`, `fb_draw_text` and the `fb_cursor_begin`/`fb_cursor_end` save-under bracket; `scripts/run-workbench.py` driving QMP hotplug, a title-bar drag and screendump title-bar colour checks behind `make test-workbench`, `EXPECT_WB`/`WB_WAIT`/`WB_DRAG`/`WB_RUN_TIMEOUT_SEC`/`WB_QEMU_EXTRA` make options, `tests/host/test_intuition.c` with a fake framebuffer, docs/workbench.md and 2487 host checks; BootInfo stays v1 and the kernel ABI is unchanged |
| `v0.22.0` | 2026-10-28 | phase 5D: pointer-driven framebuffer cursor (`kernel/arch/x86_64/fb.c`: 12x19 sprite with saved-under pixels, `fb_putc`/`fb_clear`/`fb_scroll` restore-composite guards, `fb_dimensions`/`fb_cursor_show_at`/`fb_cursor_hide`/`fb_cursor_visible`/`fb_cursor_x`/`fb_cursor_y`, `CRUG_PIXEL_BGRX8888` packing fix), `gfx.device` (`kernel/gfx/gfx.c`, `include/crug/gfx.h`: pointer drain with `GFX_CURSOR_STEP` and edge clamping, button tracking, `gfx.wait=`/`gfx.moves=` tokens, gate-mode markers, 1.5 s capture linger, silent when no gate asked), `input_poll()` in `input.device`, `scripts/run-gfx-cursor.py` driving a QMP `device_add` of `usb-mouse` plus `screendump` PPM amber-centroid comparison behind `make test-gfx`, `EXPECT_GFX`/`GFX_WAIT`/`GFX_MOVES`/`GFX_RUN_TIMEOUT_SEC`/`GFX_QEMU_EXTRA` make options, a `CRUG_HOST_TEST` idle stub so the host suite can drive the self-test clock, docs/gfx.md and 2426 host checks; BootInfo stays v1 and the kernel ABI is unchanged |
| `v0.21.0` | 2026-10-28 | phase 5C: USB Audio Class playback and capture (`drivers/usb/usb_audio.c`: UAC1/UAC2 descriptor parse for the feature unit, streaming interface, format type I and endpoint, alternate setting 1, sampling-frequency `SET_CUR`, hardware volume and mute with the spec layout then the swapped `wIndex` layout QEMU expects, 32 isochronous packets of 48 frames with success-only in-flight accounting, capture mirror and `CRUGOS: usb audio attached`/`audio capture unavailable` markers), the xHCI isochronous API (`usb_control`, `usb_isoc_open`/`submit`/`buf`/`done`/`errors`/`close`, `TRB_SIA`, microframe interval encoding for full/low speed), `audio.device` (`kernel/audio/audio.c`: 16384-frame 16-bit stereo ring, zero-fill read, playback/capture counters, 360-entry sine `audio_tone`), the per-channel mixer (`kernel/audio/mixer.c`: 1/256 dB from -30 dB to 0 dB through a 31-entry Q15 table, mute wins), `audio.rate=`/`audio.volume=`/`audio.seconds=` tokens, `scripts`-free `make test-audio` with an `-audiodev none` Hermetic USB audio function, `EXPECT_AUDIO`/`AUDIO_RUN_TIMEOUT_SEC`/`AUDIO_QEMU_EXTRA` make options and 2372 host checks; BootInfo stays v1 and the kernel ABI is unchanged |
| `v0.20.0` | 2026-10-28 | phase 5B: USB HID boot mice (`drivers/usb/xhci.c` device table of up to four boot keyboards/mice, each with a device context page, interrupt ring, 64-byte report buffer and DCI, with unbind when the port loses `CCS`; `drivers/usb/usb_hid.c` protocol 2 descriptor match, `usb_hid_mouse_decode()` with signed deltas, wheel, masked buttons and a first-report button baseline), `input.device` pointer state (`input_pointer_event`/`input_pointer_read`/`input_pointer_count`/`input_pointer_reset`), `usb.mouse=`/`usb.moves=` command-line tokens, `usb_selftest()` keyboard and mouse windows with `CRUGOS: usb mouse ready`/`usb mouse move`/`usb mouse button` markers, `scripts/run-usb-mouse.py` driving a QMP `device_add` of `usb-mouse` plus broadcast `input-send-event` motion and a button press behind `make test-usb-mouse`, `EXPECT_MOUSE`/`MOUSE_MOVES`/`MOUSE_WAIT`/`MOUSE_RUN_TIMEOUT_SEC` make options and 1168 host checks; BootInfo stays v1 and the kernel ABI is unchanged |
| `v0.19.0` | 2026-10-28 | phase 5A: polled xHCI host controller (`drivers/usb/xhci.c`: PCI class `0C/03/30`, 64-bit BAR sizing with a window assigned below `0xffc00000` when firmware leaves it zero, HCRST/`CNR` wait, one 4 KiB command/event/EP0/HID ring each with a Link TRB wrap and No-Op padding, event ring segment table, device context array plus input context, `CRCR` readback), slot enumeration (`ENABLE_SLOT`, `ADDRESS_DEVICE` at the default packet size, `GET_DESCRIPTOR` 8/18/9/full, `EVALUATE_CONTEXT` for the real `bMaxPacketSize0`, `SET_CONFIGURATION`, `CONFIGURE_ENDPOINT` and one armed Normal TRB per report), USB HID boot decoding (`drivers/usb/usb_hid.c`: class 3/subclass 1/protocol 1 descriptor walk, modifier bits, letters with Shift XOR Caps, digits, punctuation, Enter/Backspace/Tab/Escape/Space, the same Ctrl folding as the PS/2 decoder, held-key suppression) feeding `input_char()` and running `usb_poll()` as the queue's poll hook, `PORTSC` hotplug with connect reset and unbind, `usb.wait=`/`usb.keys=` tokens, `scripts/run-usb-hotplug.py` driving a QMP `device_add` of `usb-kbd` into `qemu-xhci` with `usb=off,i8042=off` behind `make test-usb-hotplug`, `EXPECT_USB` boot-check markers, the `MACHINE_EXTRA` make option and 1117 host checks; BootInfo stays v1 and the kernel ABI is unchanged |
| `v0.18.0` | 2026-10-28 | phase 4D: caching DNS stub resolver (`kernel/net/dns.c`: UDP query to `net.dns=` or the DHCP server address, TCP retry with the two-byte length prefix on truncation or timeout, strictly-backwards compression-pointer walking with 16-hop and 128-byte caps, up to four CNAME aliases, eight-entry TTL cache keyed by the lowercased name), `dns_build_query`/`dns_parse_response`/`dns_decode_name`/`dns_skip_name` wire helpers, `net.dns=`/`net.dns-tcp=`/`net.name=` command-line tokens, `SYS_RESOLVE` (15), `dns_selftest()` with `CRUGOS: dns ...` markers, `scripts/run-net-dns.py` hermetic DNS-over-TCP responder behind `make test-net-dns`, `EXPECT_DNS` boot-check markers and 1042 host checks; BootInfo stays v1 and the kernel ABI is unchanged |
| `v0.17.0` | 2026-10-27 | phase 4C: TCP transport (`kernel/net/tcp.c`: four sockets, 1400-byte segments, 4 KiB receive rings, connect with bounded SYN retries, listen/accept, send with bounded retransmission, in-order receive, close), `tcp_selftest()` client/listener HTTP exchange selected by `net.tcp=`/`net.serve=`, six socket syscalls (`SYS_SOCKET_CONNECT/SEND/RECV/CLOSE/LISTEN/ACCEPT`, typed handles), `scripts/run-net-tcp.py` with a slirp responder and a QEMU host forward behind `make test-tcp`, `EXPECT_TCP` boot-check markers and 943 host checks; BootInfo stays v1 and the kernel ABI is unchanged |
| `v0.16.0` | 2026-10-27 | phase 4B: net.device registry and net_config, legacy virtio-net PCI driver (VIRTIO_NET_F_MAC only, 32 pre-posted 2 KiB RX buffers on queue 0, one 2 KiB TX bounce buffer on queue 1 with a 500 ms completion wait, up to four polled eth0..eth3), Ethernet framing with 60-byte padding and a MAC/broadcast filter, ARP cache with request/reply and resolution retries, IPv4 header checksum validation, ICMP echo, UDP with a transmit pseudo-header checksum, DHCP client (DISCOVER/OFFER/REQUEST/ACK, three attempts, options 53/55/50/54, mask/router/DNS), net_selftest() boot gate, QEMU slirp wiring (NET_MODE=user) behind make test-net and 938 host checks; BootInfo stays v1 and the kernel ABI is unchanged |
