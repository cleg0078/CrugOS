# Boot flow and the boot contract

## Chain

```
OVMF (UEFI firmware)
  -> EFI/BOOT/BOOTX64.EFI          CrugOS loader
       - serial + ConOut console
       - read \crugos\kernel.elf from the boot device
       - read \crugos\cmdline.txt (optional kernel command line, <= 511 B)
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
       - kernel heap, input.device + polled PS/2 keyboard, net.device +
         polled virtio-net (Ethernet/ARP/IPv4/ICMP/UDP + DHCP client) and the
         TCP transport with its six socket syscalls
       - virtio-blk/NVMe/AHCI + GPT + AES-256-XTS crypt unlock + RAID
         assembly (cold-spare rebuild) + writable ext4, JBD2 replay, self-test
       - scheduler, handles and MsgPort IPC
       - syscall dispatcher behind int 0x80
       - self-tests, boot report, deterministic exit
```

## BootInfo

`include/crug/bootinfo.h` is the single contract between loader and kernel.
The structure is versioned (`magic = 0x43524731`, `version = 1`) and the kernel
refuses to boot if either does not match or if required fields are invalid.
Phases 1 through 6A did not change the layout; phase 3B-9 is the first
kernel to consume the `command_line` field, phase 3B-10 falls back to an
interactive serial passphrase prompt when no token is present, phases 4C and 4D
read the non-secret `net.tcp=` / `net.serve=` / `net.dns=` /
`net.dns-tcp=` / `net.name=` tokens from the same field, phase 5A adds
`usb.wait=` / `usb.keys=`, phase 5B adds `usb.mouse=` / `usb.moves=`, phase
5C adds `audio.rate=` / `audio.volume=` / `audio.seconds=`, phase 5D adds
`gfx.wait=` / `gfx.moves=`, phase 6A adds `wb.enable=` / `wb.wait=` /
`wb.drag=` / `wb.hold=`, phase 6B adds `wb.menu=` / `wb.acts=`, phase 6C
adds `wb.shell=1` / `wb.sh=` for the shell window, phase 6D adds
`wb.keys=1` for keyboard navigation of the desktop, phase 6E adds
`wb.desk=1` for desktop objects and the depth gadget, phase 6F adds
`wb.ikeys=1` for keyboard focus of the icon table, phase 6G adds `wb.dr=1`
for drawer scrolling and refresh plus `wb.fill=<n>` to fill the root first, and
phase 6H adds `wb.dsel=1` for click-selected drawer rows and a draggable
scrollbar, phase 6I adds `wb.screens=1` for a retained `Tools` screen with its
window, gadget and self-test, phase 6J adds `wb.cx=1` for the Commodities
Exchange (four built-in hotkey utilities and a manager window), phase 7A adds
`wb.app=1` for the Applications icon and the bundle window behind it, and
phase 7B adds `wb.pkg=1` for the Packages window plus `pkg.test=1` and
`pkg.test=2` for the package and capability self-tests, phase 7C makes that
window a toggle whose body press installs while the package is absent and
removes while it is present and grows `pkg.test=1` into install, update and
removal checks, and phase H2 adds
`heap.stress=<1..200000>` and `heap.guard=1`,
parsed by `heap_cmdline()` right after `heap_init()`, and phase H3 adds
`trap.test=<recipe>` (`de`, `ud`, `gp`, `pf`, `df`, `nmi`, `mc`, `db`) for the
fault injector, parsed by `trap_cmdline()` right after `heap_cmdline()`, and
phase U1 adds `user.test=`, parsed by `user_test_cmdline()` right after
`trap_cmdline()`: `user.test=1` runs three ring-3 programs from `/bin` after the
kernel self-test, phase U2's `user.test=2` also runs the two-instance
isolation pair, phase U3's `user.test=3` adds the scripted ring-3 shell
(`/bin/sh` reading `/etc/sh.rc`), phase U4's `user.test=4` adds
`/bin/spawn`, which spawns `/bin/child`, waits for its exit code and then
execs the same child in place, phase U5's `user.test=5` adds `/bin/fork`, which
copies the calling program into a second address space so the child can write
its own pages without the parent seeing the write, and phase U6's
`user.test=6` adds `/bin/forkbig`, which forks 300 shared pages and proves the
child's writes never reach the parent's copy, and phase 7A's `user.test=7` adds
`app_list()`, which runs `/bin/sh /etc/app.rc` so the shell lists, inspects and
launches a bundle and exits 9. `pkg.test=1` is independent of the user level:
it probes and installs a signed package and refuses one signed by an untrusted
key, and `pkg.test=2` launches the installed bundle and proves its capability
mask, both parsed by `pkg_test_cmdline()` right after `user_test_cmdline()`.
Without the token the user level is 0 and the self-test is silent. A boot
medium that ships no block device (the hybrid ISO) passes `storage=none`, which
is the one token that changes the storage contract instead of adding work to
it. The loader caps the command line at 511 bytes and the kernel wipes the
page after it copies the `crypt.pass=` value.

| Field | Meaning |
| --- | --- |
| `kernel_base`, `kernel_size` | physical extent of the loaded kernel image |
| `memory_map`, `memory_map_count` | normalized `memory_region[]` |
| `fb` | GOP framebuffer base/size/resolution/stride/pixel format |
| `command_line`, `command_line_len` | optional NUL-terminated command line from `\crugos\cmdline.txt`; `crypt.pass=<pass>` is consumed and the page wiped; without it the kernel prompts on the serial console (up to three masked attempts) |
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
7. `heap_init`: slab caches and large-allocation headers; `heap_cmdline()`
   then reads `heap.stress=` and `heap.guard=`, `trap_cmdline()` reads
   `trap.test=` and `user_test_cmdline()` reads `user.test=`. `CRUGOS: heap ready`.
8. `input_init` and `keyboard_init`: reset `input.device`, then program the
   i8042 controller (translation on, IRQ1/IRQ12 masked) and enable scanning.
   The boot prints `CRUGOS: input ready (serial + keyboard)`, or
   `(serial only)` when no i8042 keyboard answered. `usb_init` then brings up the
   first xHCI controller (PCI class 0C/03/30) and `usb_cmdline` reads
   `usb.wait=<seconds>`, `usb.keys=<n>`, `usb.mouse=<seconds>` and
   `usb.moves=<n>`: the boot prints `CRUGOS: usb controller absent` when the
   machine has no xHCI function, otherwise `CRUGOS: usb controller ready (xhci,
   <n> port(s))` and, once devices are enumerated, `CRUGOS: usb keyboard ready
   (endpoint 0x81, 8 byte reports)` and/or `CRUGOS: usb mouse ready (endpoint
   0x81, 4 byte reports)`. Up to four boot keyboards and mice are tracked.
   `audio_cmdline` then reads `audio.rate=`, `audio.volume=` and
   `audio.seconds=`, and `audio_init` resets the mixer and the PCM ring:
   `CRUGOS: audio device absent` when no USB audio function was enumerated, or
   `CRUGOS: audio device ready (usb uac1|uac2, 2 ch, 16-bit, 48000 Hz)` after
   the driver attached to interface class 0x01. `gfx_init` then requires a GOP
   framebuffer and centres the pointer sprite, and `gfx_cmdline` reads
   `gfx.wait=<seconds>` and `gfx.moves=<n>`: the boot prints
   `CRUGOS: gfx framebuffer absent` or
   `CRUGOS: gfx ready (1024x768, cursor at 512,384)`. `wb_cmdline` then reads
   `wb.enable=`, `wb.wait=<seconds>`, `wb.drag=<n>`, `wb.menu=<0|1>`,
   `wb.acts=<n>`, `wb.hold=<seconds>`, `wb.keys=<0|1>`, `wb.desk=<0|1>`,
   `wb.ikeys=<0|1>` (parsed before `wb.desk=`, and still ignored while
   `wb.shell=1` owns the keyboard) and `wb.dr=<0|1>` / `wb.fill=<0..24>` (the
   drawer's keys and a one-shot fill of `/f00`..`/f(N-1)` before it opens) and
   `wb.dsel=<0|1>` (parsed before `wb.shell=`: it gives the drawer body the
   click-and-drag handler, and the drawer must be opened for it to matter) and
   `wb.screens=<0|1>` (a second `Tools` screen with a window, a gadget that
   moves it to screen 0, and the screens self-test) and `wb.cx=<0|1>` (a
   Commodities Exchange registry; only 0 disables it, and it needs
   `wb.enable=1`), `wb.app=<0|1>` (the Applications icon and its bundle
   window, which the app self-test drives; it also needs `wb.enable=1`) and
   `wb.pkg=<0|1>` (the Packages window, which the package self-test presses;
   it also needs `wb.enable=1`), and
   `wb_init` builds the desktop only when
   the token was supplied: it takes
   the framebuffer away from the text console and prints `CRUGOS: wb desktop
   ready (1024x768, 2 window(s))`, followed by `CRUGOS: wb keys ready (window 2
   active)` when `wb.keys=1` asked for keyboard navigation and by
   `CRUGOS: wb icon ready ('System:', volume)` when `wb.desk=1` asked for the
   desktop object and by `CRUGOS: wb icons ready (N object(s), keyboard focus)`
   when `wb.ikeys=1` asked for the icon table's keyboard focus, and by
   `CRUGOS: wb drawer mode (arrow keys scroll, r refreshes)` when `wb.dr=1`
   gave the drawer the keys, and by `CRUGOS: wb select mode (click a row, drag
   the scrollbar)` when `wb.dsel=1` installed the body-input handler after the
   desktop-ready line, and by `CRUGOS: wb screens ready (N screen(s), front 0)`
   when `wb.screens=1` added the retained Tools screen with its window, menus
   and Send to Workbench gadget, by `CRUGOS: wb cx ready (N commodity(ies))` when `wb.cx=1` registered the four
   built-in commodities, and by `CRUGOS: wb app icon ready (Applications:)` and
   `CRUGOS: wb app ready (N application(s))` when `wb.app=1` added the
   Applications icon and its window, and by `CRUGOS: wb pkg ready (window N, M
   package(s))` when `wb.pkg=1` opened the Packages window before `pkg_init`
   filled the registry, and `CRUGOS: wb pkg removed <id> (N package(s))` when
   the toggle took one away; a screen switch prints `CRUGOS: wb screen N
   front (K window(s))` and a move between screens `CRUGOS: wb window N moved
   to screen M`. Without the token the framebuffer console keeps
   its text mode and no Workbench line appears.
9. `net_init`: reset `net.device` and `tcp_init`, then probe legacy virtio-net
   PCI functions (up to four `eth0`..`eth3`) and register them, bind the DHCP
   client to UDP port 68 and read the `net.tcp=<port>` / `net.serve=<seconds>`
   command-line tokens through `tcp_cmdline`; `dns_init` empties the resolver
   cache and binds UDP port 53000, and `dns_cmdline` reads `net.dns=<ip>[:port]`,
   `net.dns-tcp=1` and `net.name=<host>`. The boot prints
   `CRUGOS: net ready (N device(s))`. The network self-test runs later, with the
   other self-tests.
10. `storage_init`: `block_init`, the legacy virtio-blk devices, then the
   NVMe driver (PCI class 01/08/02, admin/I/O queues, namespace 1) and the AHCI
   driver (PCI class 01/06/01, port bring-up, IDENTIFY, `sd` names), and the GPT
   of every block device. `storage_parse_cmdline` first extracts `crypt.pass=`
   from the loader command line and wipes that page, and recognises
   `storage=none`, which marks a boot medium without a block device: every
   transport is still probed and prints its `ready` line, then `storage_init`
   prints `CRUGOS: storage skipped (storage=none)` and succeeds without a root,
   and `storage_selftest` becomes a soft skip. A machine with no block device
   and no such token still fails closed with `CRUGOS: no root filesystem`.
   Without the token, a partition named `crugroot` is mounted directly;
   otherwise a partition named `crugcrypt` (or
   carrying the CrugOS encrypted-root type GUID) is unlocked: the kernel reads
   the 16 KiB header and keyslots, derives 96 bytes with Argon2id, verifies the
   slot MAC, unwraps the 64-byte master key and registers `crypt0`. When the
   loader command line carried no `crypt.pass=` token, the kernel prints
   `CRUGOS: crypt passphrase required`, prompts `passphrase: ` with a masked
   echo (`*` per accepted byte) and reads up to three lines through
   `kernel/lib/lineedit.c`; each attempt is bounded by
   `crypt.prompt_timeout=<seconds>` (1..3600, default 60 per attempt). A
   rejected line prints `CRUGOS: passphrase rejected (N attempt(s) left)`, and
   three rejections print `CRUGOS: crypt locked (too many attempts)`; an empty
   line, Ctrl-C, overflow or timeout prints
   `CRUGOS: crypt locked (no passphrase)`. Every line is zeroed after the
   attempt. Failing to unlock fails the boot (there is no fallback past a
   present encrypted root).
   Otherwise `fd00` partitions are assembled into RAID1 or RAID10 `md0` and
   mounted. A member whose metadata sectors are blank is a cold spare: the
   kernel assigns it the lowest missing slot, copies the missing data from its
   mirror and writes fresh metadata before mounting. A journaled filesystem is
   opened through JBD2; a dirty journal that refuses a read-only mount is
   replayed by a short read-write mount, which prints
   `CRUGOS: journal replayed <n> block(s) at mount`, and the read-only mount is
   then retried.
   `CRUGOS: virtio ready`, `CRUGOS: nvme ready`, `CRUGOS: ahci ready`,
   `CRUGOS: block ready`,
   `CRUGOS: gpt ready`, `CRUGOS: crypt passphrase required` and the
   interactive `passphrase:` prompt when the console is used (serial line or
   PS/2 keyboard), `CRUGOS: raid
   ready`,
   `CRUGOS: raid rebuilt (<n> member(s))` when a spare was rebuilt, the
   `raid   : level ...` geometry line and
   `CRUGOS: raid healthy`/`raid degraded` when RAID is used, then
   `CRUGOS: root device <partition>` for the selected root partition, then
   `CRUGOS: ext4 ready`, `CRUGOS: journal ready`, `CRUGOS: storage ready`.
   `app_init` then lists `/Applications`, validates each `manifest.toml`, checks
   that the payload program exists and prints `CRUGOS: app registry ready (N
   app(s), M rejected)` plus one `CRUGOS: app rejected <dir> (<reason>)` line per
   refused bundle. A disk without bundles (every gate but
   `make test-app-bundles`) prints `CRUGOS: app registry ready (0 app(s), 0
   rejected)`.
   `perms_init` then reads `/System/perms.conf` and prints
   `CRUGOS: perms policy ready (default <caps>, N rule(s))`; a boot without
   storage or without that file keeps the privileged `CAP_ALL` default.
   `pkg_init` then reads the `/System/pkgdb` records and prints
   `CRUGOS: package registry ready (N package(s))`.
11. `sched_init`: per-class run queues, boot thread id 0 and the idle thread.
   `CRUGOS: sched ready`.
12. `process_init`: the kernel process and its handle table.
    `CRUGOS: syscall ready`.
13. `sti`, then `selftest_run`: allocator, mappings, W^X audit, `int3`, timer,
    heap, block/GPT/ext4/storage (including the read-write round trip, checksum
    verification and a journal replay test), handles, scheduler, message ports
    and syscalls, then the network self-test (DHCP lease, ARP, ICMP echo) when a
    NIC is present, followed by the TCP checks (listen on 8080, HTTP client leg
    against `net.tcp=`, the same exchange through the socket syscalls, and one
    served request bounded by `net.serve=`) when that token was supplied, then
    the DNS checks (resolve `net.name=`, a cached second lookup and the
    `SYS_RESOLVE` path) when a name was supplied; then the USB check, which
    waits for a keyboard only when `usb.wait=` or `usb.keys=` asked for it and
    for a mouse only when `usb.mouse=` or `usb.moves=` did; then the audio
    check, which streams a 440 Hz tone for `audio.seconds=` (0.5 s by default)
    through the mixer when a USB audio function is present and is silent
    otherwise; then the graphics check, which shows the cursor and waits for
    `gfx.moves=` pointer reports within `gfx.wait=` seconds when those tokens
    were supplied and is silent otherwise; then the Workbench check, which shows
    the desktop and waits for `wb.drag=` title-bar drags and, with `wb.menu=1`,
    `wb.keys=1`, `wb.desk=1` or `wb.ikeys=1`, for `wb.acts=` menu, gadget, close,
    resize, icon, icon-move, depth and front actions within `wb.wait=` seconds
    when `wb.enable=` enabled it; with `wb.shell=1` it also waits for `wb.sh=`
    typed shell commands, with `wb.desk=1` it opens the `System:` drawer and
    prints `CRUGOS: wb desk selftest ok (N drawer(s))`, with `wb.ikeys=1` it
    prints `CRUGOS: wb icons selftest ok (N move(s), M front(s))`, and with
    `wb.dr=1` it waits for `wb.acts=` drawer scrolls and refreshes and prints
    `CRUGOS: wb drawer selftest ok (N scroll(s), M refresh(es))`, or
    `CRUGOS: wb drawer timeout (N/M)` when the wait expires first; with
    `wb.dsel=1` it waits for two row clicks and one scrollbar drag and prints
    `CRUGOS: wb select selftest ok (N select(s), M scrollbar drag(s))`, or
    `CRUGOS: wb select timeout (N/M)`. With `wb.screens=1` it switches the
    strip twice and moves the `Tools` window once, printing
    `CRUGOS: wb screens selftest ok (N switch(es), M move(s))`; with `wb.cx=1`
    it waits for two row toggles and two hotkey fires and prints
    `CRUGOS: wb cx selftest ok (N toggle(s), M fire(s))`; with `wb.app=1` it
    opens the Applications window, clicks the first row and launches it, printing
    `CRUGOS: wb app selftest ok (N click(s), M launch(es))` once the desktop has
    reaped the program; with `wb.pkg=1` it presses the Packages window body three times
    (`wb.acts=3`), installing the demo package, removing it and installing it
    again, and prints
    `CRUGOS: wb pkg selftest ok (N install(s), M remove(s), K package(s))`. The
    whole
    check is silent unless `wb.enable=` enabled it. `heap_selftest()` prints
    `CRUGOS: heap check ok (S slab(s), O object(s), L large block(s))` and,
    when `heap.stress=` or `heap.guard=` was supplied, runs those passes too.
    Right after `CRUGOS: selftest ok` the user self-test runs when `user.test=`
    was supplied. `user.test=1` loads `/bin/hello`, `/bin/badptr` and
    `/bin/fault` one at a time, joins each thread and checks the exit codes 0,
    0 and -14. `user.test=2` runs the same three and then `/bin/iso` twice in
    two address spaces at once, checking the isolation lines below as well as
    the same exit codes. `user.test=3` runs levels 1 and 2 and then `/bin/sh`
    with the script `/etc/sh.rc`, expecting exit 7. `user.test=4` runs level 3
    and then `user_spawn()`, which runs `/bin/spawn`, joins it and requires exit
    5; the spawn program itself spawns `/bin/child`, waits for exit 5 and then
    execs the same binary, so the level proves spawn, wait and in-place exec at
    once. `user.test=5` runs level 4 and then `user_fork()`, which runs
    `/bin/fork`, joins it and requires exit 0; the fork program copies itself
    with `SYS_FORK`, the child overwrites a word the parent had set to `0x1111`
    and exits 3, and the parent must still read its own `0x1111` after the wait.
    `user.test=6` runs level 5 and then `user_forkbig()`, which runs
    `/bin/forkbig`, joins it and requires exit 0; the program maps 300 pages,
    forks them copy-on-write, has the child write both ends and exit 7, and then
    checks that its own sentinel and trailing byte survived. `user.test=7` runs
    level 6 and then `user_app()`, which runs `/bin/sh /etc/app.rc`; the script
    performs `app list`, `app info orbit` and `app run orbit` and then exits 9,
    and the kernel requires that code. A mismatch exits 35
    before the report. Without the token the user level is 0 and nothing is
    printed. `pkg.test=1` then runs `pkg_selftest()`: it probes the signed and
    the untrusted package, installs the first, updates it to 2.0.0, refuses the
    downgrade, a second install of the same version and the untrusted package,
    removes the package, refuses a second removal, reinstalls it, re-runs
    `app_init` and prints
    `CRUGOS: pkg selftest ok (2 installed, 1 updated, 1 removed)`. `pkg.test=2` installs
    the bundle if it is missing, checks the manifest mask against the policy,
    launches it and requires exit 5 with `fs.read` only, printing
    `CRUGOS: caps selftest ok (exit 5, caps 0x1)`. A failed package or
    capability check exits 35 before the report.
14. The user self-test of step 13, when it ran, has already joined its threads;
    then `report_boot`, `storage_lock` (wipes the passphrase, the volume keys
    and the bounce buffer), `CRUGOS: phase 7c boot ok`, then
    `wb_hold` (only when
    `wb.hold=` was given: the guest stays alive with the desktop on screen until
    the deadline), `trap_test_run()` (only when `trap.test=` was given, so the
    fault lands after the phase marker) and `arch_exit_qemu(0x10)`.

Storage is still brought up with interrupts disabled: the virtio driver polls
its used ring, the NVMe driver polls completion queues and the AHCI driver polls
port registers between `heap_init` and `sched_init`.

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
2048 bytes. Each slab page starts with a 64-byte header (a page cookie, class,
object and used counts, a validated 256-bit used map and a free list); objects
follow it. Allocations larger than 2048 bytes, up to the 1 MiB cap, take
contiguous PMM pages with a 64-byte large-block header carrying a page cookie,
size, page count and list links. Phase H2 validates every header and free-list
entry before use and can walk the heap read-only; see [heap.md](heap.md). Slab
pages are not returned to the PMM when they become empty (a phase 2
simplification); large allocations are returned immediately. All heap paths run
with interrupts disabled.

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

The boot disks are legacy PCI virtio-blk devices (`vda`..`vdh`), NVMe
namespaces (`nvme0n1`..) or AHCI/SATA drives (`sda`..`sdh`). `storage_init()` parses every disk's GPT and prefers
a partition named
`crugroot`, mounted directly as ext4. If there is none, it looks for an
encrypted root (name `crugcrypt` or the CrugOS encrypted-root type GUID),
unlocks it with the `crypt.pass=` value from the loader command line and mounts
ext4 over `crypt0`. If there is neither, it collects the
partitions whose type is Linux RAID (`fd00`), validates their 512-byte metadata
records and assembles `md0`, then mounts that. The metadata level selects the
layout: level 1 mirrors every sector across all members, while level 10
stripes 64 KiB chunks across mirror pairs and maps each range with
`(lba / chunk) % (member_count / 2)`. A missing member is allowed unless a
RAID10 mirror pair loses both members. A member with blank metadata is treated
as a cold spare and rebuilt synchronously from its mirror (64-sector chunks,
new checksummed metadata, flush) before the array is used; `CRUGOS: raid
rebuilt (N member(s))` reports it. Degraded arrays print
`CRUGOS: raid degraded (N/M members)`, and a healthy array prints
`CRUGOS: raid healthy (N/M members)` after a geometry line such as
`raid   : level 10, 4 member(s), 2 pair(s), chunk 128 sectors`. The encrypted path prints
`CRUGOS: crypt ready (<n> sectors, keyslot <i>, Argon2id m=<m> KiB t=<t> p=<p>)`,
`CRUGOS: crypt aes: software`/`aes-ni` and `CRUGOS: root device crypt0` instead
of the RAID lines. The root mount then reads
`/hello.txt` into a static buffer and the self-test re-checks the mount, the
directory listing and the file contents. See [raid.md](raid.md) and
[nvme.md](nvme.md).

The one exception is the `storage=none` token: it declares that the boot medium
carries no root at all, which is the case for the hybrid image built by
`make iso`. Probes run as usual, `CRUGOS: storage skipped (storage=none)` is
printed instead of a root selection, and the writable, journal and crash
self-tests are skipped rather than failed. See [iso.md](iso.md).

The same self-test then remounts the root device read-write and exercises the
write path: it removes stale `/selftest` entries, creates `/selftest` and
`a.txt`, writes and `fsync`s a 5000-byte pattern, renames the file to `b.txt`,
truncates it to 1000 bytes, creates and unlinks `tmp.txt`, unmounts cleanly
(marking the filesystem clean and flushing the virtio cache), and finally
remounts read-only to verify that `/selftest/b.txt` still holds the first 1000
bytes of the pattern.

A journal check follows: the test writes `/selftest/j.bin`, queues a dirty
block through `journal_test_dirty()`, unmounts *without* clearing the journal,
remounts read-write (which replays it through `journal_open()`), and reads the
physical block back. Because the test disk is formatted with `metadata_csum`
and a checksum v3 journal, the same round trip also proves the checksum path:
the host gate extracts the partition and requires `e2fsck -fn` to pass, which
verifies every checksum the kernel wrote.

On the crash gate image (`/simulate-crash` present) the self-test switches to a
two-boot scenario instead: the first boot journals 100 data blocks in one
multi-descriptor transaction (two descriptors at 1 KiB), marks the superblock
unclean, commits and exits without checkpointing, printing
`CRUGOS: dirty journal committed (no checkpoint)`. The next boot replays it at
mount, verifies the file contents and prints
`CRUGOS: journal replay verified (<n> block(s) recovered)`. The complete
stack, supported ext4 subset, write path, checksum layout, journal
format/recovery and known limits are documented in [storage.md](storage.md) and
[checksums.md](checksums.md).

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
CRUGOS: input ready (serial + keyboard)   # or: (serial only)
CRUGOS: usb controller absent             # or: usb controller ready (xhci, 8 port(s))
CRUGOS: audio device absent               # or: audio device ready (usb uac1, 2 ch, 16-bit, 48000 Hz)
CRUGOS: gfx ready (1024x768, cursor at 512,384)
CRUGOS: wb desktop ready (1024x768, 2 window(s))  # only with wb.enable=1
CRUGOS: wb keys ready (window 2 active)    # only with wb.keys=1
CRUGOS: wb icon ready ('System:', volume)  # only with wb.desk=1
CRUGOS: wb icons ready (1 object(s), keyboard focus)  # only with wb.ikeys=1
CRUGOS: wb drawer mode (arrow keys scroll, r refreshes)  # only with wb.dr=1
CRUGOS: wb select mode (click a row, drag the scrollbar)   # only with wb.dsel=1
CRUGOS: wb screens ready (2 screen(s), front 0)  # only with wb.screens=1
CRUGOS: wb cx ready (4 commodity(ies))     # only with wb.cx=1
CRUGOS: usb keyboard ready (endpoint 0x81, 8 byte reports)  # USB keyboard attached
CRUGOS: usb key 'a'                       # one per decoded key while usb.wait= is set
CRUGOS: usb mouse ready (endpoint 0x81, 4 byte reports)     # USB mouse attached
CRUGOS: usb mouse move dx=3 dy=0          # one per report while usb.mouse= is set
CRUGOS: usb mouse button 0x01
CRUGOS: net ready (1 device(s))           # 0 when no NIC is present
CRUGOS: virtio ready
CRUGOS: nvme ready
CRUGOS: ahci ready
CRUGOS: block ready
CRUGOS: gpt ready
CRUGOS: crypt ready (24544 sectors, keyslot 0, Argon2id m=4096 KiB t=3 p=1)
CRUGOS: crypt aes: software        # or: aes-ni
CRUGOS: crypt passphrase required   # no crypt.pass= token: console unlock
CRUGOS: type the passphrase on the console (serial or keyboard) (3 attempt(s))
passphrase: ***********             # one * per accepted byte, never echoed
CRUGOS: passphrase rejected (2 attempt(s) left)
CRUGOS: crypt locked (too many attempts)
CRUGOS: crypt locked (no passphrase)  # empty, cancelled or timed out
CRUGOS: crypt unlock failed         # rejected token passphrase: boot fails closed
CRUGOS: root device crypt0
CRUGOS: raid ready
CRUGOS: raid rebuilt (1 member(s))  # only when a blank spare was repaired
raid   : level 10, 4 member(s), 2 pair(s), chunk 128 sectors
CRUGOS: raid healthy            # or: CRUGOS: raid degraded
CRUGOS: journal replayed (100 block(s) at mount)  # only after a crash
CRUGOS: ext4 ready
CRUGOS: journal ready
CRUGOS: storage ready
CRUGOS: app rejected Broken.app (no-version)  # one line per refused bundle
CRUGOS: app registry ready (2 app(s), 1 rejected)  # 0/0 on a disk with no bundles
CRUGOS: perms policy ready (default fs.read,app.launch, 1 rule(s))  # only with a policy file
CRUGOS: package registry ready (0 package(s))  # 0 on a disk with no packages
CRUGOS: sched ready
CRUGOS: syscall ready
CRUGOS: heap check ok (6 slab(s), 8 object(s), 1 large block(s))
CRUGOS: writable storage ok
CRUGOS: journal selftest ok
CRUGOS: dhcp lease 10.0.2.15/255.255.255.0 gw 10.0.2.2 dns 10.0.2.3
CRUGOS: arp resolved 10.0.2.2 52:55:0a:00:02:02
CRUGOS: icmp echo reply from 10.0.2.2 seq=1 ttl=255
CRUGOS: tcp listen ready (port 8080)      # only with net.tcp= on the command line
CRUGOS: tcp connect ok (10.0.2.2:52847)
CRUGOS: tcp http ok (53 bytes)
CRUGOS: tcp socket syscall ok
CRUGOS: tcp http served (72 bytes)
CRUGOS: dns server 10.0.2.2:53 (udp)      # only with net.name= on the command line
CRUGOS: dns resolved crugos.test -> 10.0.2.99 via tcp (ttl 60)
CRUGOS: dns cache hit crugos.test -> 10.0.2.99
CRUGOS: dns socket syscall ok
CRUGOS: dns selftest ok
CRUGOS: net selftest ok
CRUGOS: audio stream started (48000 Hz, 2 ch)   # only with a USB audio function
CRUGOS: audio streamed 24144 frame(s) in 498 ms
CRUGOS: audio capture unavailable (no input endpoint)
CRUGOS: audio selftest ok
CRUGOS: gfx cursor shown at (512, 384)     # only with gfx.wait=/gfx.moves=
CRUGOS: gfx cursor at (572, 424)
CRUGOS: gfx selftest ok (3 move(s), buttons 0x00)
CRUGOS: wb window 2 drag start (512, 312)   # only with wb.enable=1
CRUGOS: wb window 2 drag to (570, 350)
CRUGOS: wb window 2 moved to (570, 350)
CRUGOS: wb window 2 resize start (790, 490) # only with a press on the grip
CRUGOS: wb window 2 resized to 260x140
CRUGOS: wb action 1 resize window 2 (260x140)
CRUGOS: wb window 1 focused (Tab)           # only with wb.keys=1
CRUGOS: wb window 1 key move to (96, 90)
CRUGOS: wb menu 'Workbench' open (window 1)  # Enter with wb.keys=1
CRUGOS: wb menu 'Workbench' item 1 highlighted ('Info')
CRUGOS: wb menu 'Shell' open (window 2)     # only with wb.menu=1
CRUGOS: wb menu 'Shell' selected ('New')
CRUGOS: wb menu closed (escape)
CRUGOS: wb action 1 menu window 2 item 0
CRUGOS: wb shell ready (window 2, 37x8 text)  # only with wb.shell=1
CRUGOS: wb icon 'System:' selected          # only with wb.desk=1
CRUGOS: storage remounted read-write        # only with wb.fill=
CRUGOS: wb fill 24 file(s)
CRUGOS: storage remounted read-only
CRUGOS: wb drawer 'System:' opened (29 entries)
CRUGOS: wb drawer ready (29 entries, 11 row(s))
CRUGOS: wb drawer scrolled (top 1 of 29)
CRUGOS: wb drawer refreshed (29 entries)
CRUGOS: wb drawer selected '<name>' (index 2 of 29)  # only with wb.dsel=1
CRUGOS: wb drawer scrollbar grabbed
CRUGOS: wb drawer scrollbar to (top 18 of 29)
CRUGOS: wb screen 1 front (1 window(s))     # only with wb.screens=1
CRUGOS: wb action 1 screen 1 front
CRUGOS: wb window 3 moved to screen 0
CRUGOS: wb action 3 screen-move window 3 to 0
CRUGOS: wb screen 0 front (3 window(s))
CRUGOS: wb action 4 screen 0 front
CRUGOS: wb window 3 sent to back
CRUGOS: wb action 2 icon 0
CRUGOS: wb action 3 depth window 3
CRUGOS: wb desk selftest ok (1 drawer(s))
CRUGOS: wb icon 'System:' focused            # only with wb.ikeys=1
CRUGOS: wb action 4 icon 0 (keyboard)
CRUGOS: wb icon activated (keyboard)
CRUGOS: wb icon 'System:' drag to (168, 376)
CRUGOS: wb icon 'System:' dropped at (168, 376)
CRUGOS: wb action 5 icon-move 0
CRUGOS: wb window 1 sent to front
CRUGOS: wb action 6 front window 1
CRUGOS: wb icons selftest ok (1 move(s), 1 front(s))
CRUGOS: sh run 'ver'
CRUGOS: sh out CrugOS 0.45.0 (phase 7c), x86_64
CRUGOS: wb selftest ok (1 drag(s), 1 action(s))
CRUGOS: wb desk selftest ok (1 drawer(s))
CRUGOS: wb drawer selftest ok (3 scroll(s), 1 refresh(es))
CRUGOS: wb select selftest ok (2 select(s), 1 scrollbar drag(s))
CRUGOS: wb screens selftest ok (2 switch(es), 1 move(s))  # only with wb.screens=1
CRUGOS: wb action 1 commodity Mute off      # only with wb.cx=1
CRUGOS: wb commodity Mute blocked (hotkey off)
CRUGOS: wb action 2 commodity Mute on
CRUGOS: wb action 3 commodity Mute fired (muted)
CRUGOS: wb action 4 commodity Clock fired (uptime 1 s)
CRUGOS: wb cx selftest ok (2 toggle(s), 2 fire(s))
CRUGOS: wb app icon ready (Applications:)     # only with wb.app=1
CRUGOS: wb app ready (2 application(s))       # only with wb.app=1
CRUGOS: wb app selected hello (index 0 of 2)  # one click selects a row
CRUGOS: wb app launch hello tid 11            # a second click launches it
CRUGOS: wb app reaped tid 11 exit 0
CRUGOS: wb app selftest ok (1 click(s), 1 launch(es))
CRUGOS: wb pkg ready (window 3, 0 package(s))  # only with wb.pkg=1
CRUGOS: wb pkg installed /System/packages/captest.crpkg (1 package(s))  # only with wb.pkg=1
CRUGOS: wb pkg removed captest (0 package(s))           # a second body press takes it away
CRUGOS: wb pkg installed /System/packages/captest.crpkg (1 package(s))  # the third press installs again
CRUGOS: wb pkg selftest ok (2 install(s), 1 remove(s), 1 package(s))
CRUGOS: sh selftest ok (1 command(s))
CRUGOS: selftest ok
CRUGOS: user selftest start                  # only with user.test=
CRUGOS: user hello from ring 3
CRUGOS: user program /bin/hello finished (exit 0)
CRUGOS: user badptr blocked (EFAULT)
CRUGOS: user program /bin/badptr finished (exit 0)
CRUGOS: user fault writing above the window
CRUGOS: trap report page fault vector=14 error=0x6 ist=0 from=user
CRUGOS: trap fault present=no write=yes user=yes reserved=no instruction=no cr2=0x10000000000
CRUGOS: user fault page fault at rip=0x0000008000010064 err=0x0000000000000006 tid 10
CRUGOS: user fault killed tid 10 code -14
CRUGOS: user program /bin/fault finished (exit -14)
CRUGOS: user isolation start
CRUGOS: user isolation frames 0x28000 vs 0x38000 at 0x8000030000
CRUGOS: iso tid 11 word 0x15500b00 ok
CRUGOS: iso tid 12 word 0x15500c00 ok
CRUGOS: user isolation ok (2 program(s), 0 mismatch(es))
CRUGOS: user shell start
CRUGOS: sh start (argc 2)
CRUGOS: sh cmd ver
CRUGOS: sh cmd echo
CRUGOS: sh cmd cat
CRUGOS: sh cat /hello.txt 16 byte(s)
CRUGOS: sh cmd ls
CRUGOS: sh cmd write
CRUGOS: storage remounted read-write
CRUGOS: storage remounted read-only
CRUGOS: sh write /shout.txt 22 byte(s)
CRUGOS: sh cmd cat
CRUGOS: sh cat /shout.txt 22 byte(s)
CRUGOS: sh cmd exit
CRUGOS: sh exit 7
CRUGOS: user shell finished (exit 7)
CRUGOS: user selftest ok (3 program(s), 1 fault(s))
boot   : pmm=28575 us cpu=414 us vm=617 us traps=317 us timer=65299 us heap=494 us storage=95001 us sched=5614 us syscall=90 us total=196426 us
CRUGOS: phase 7c boot ok
```

With `trap.test=<recipe>` the same boot ends differently: the injector runs after
`CRUGOS: phase 7c boot ok`, injects exactly one fault and the guest exits 35
instead of 33. A `trap.test=de` boot ends:

```
CRUGOS: trap test de
CRUGOS: trap report divide error vector=0 error=0x0 ist=0 from=kernel
CRUGOS: trap rip=0x10509b cs=0x8 rflags=0x10246 rsp=0x1c0f70 ss=0x10
CRUGOS: trap regs rax=0x64 rbx=0x1de47000 rcx=0x1 rdx=0x0 rsi=0x12f693 rdi=0x136732 rbp=0x1c0f80
CRUGOS: trap regs r8=0x0 r9=0x1 r10=0x98 r11=0x1312e0 r12=0x1de47000 r13=0x1cb000 r14=0x1de47000 r15=0x1de49000
*** CRUGOS PANIC: divide error ***
CRUGOS: panic
```

`trap.test=pf` adds a fault line between the saved-frame line and the
register dump:

```
CRUGOS: trap fault present=no write=yes user=no reserved=no instruction=no cr2=0xffff800000000000
```

A `trap.test=df`, `nmi`, `mc` or `db` boot reports `ist=1`, `ist=2`, `ist=3` and
`ist=4`. The formatter, the classification matrix and the eight recipes are in
[traps.md](traps.md); `make test-trap` is the gate. Phase H4's
`make test-fuzz` adds no line here: it mutates disk images on the host in a
forked child and never boots QEMU, so it produces no serial output at all (see
[fuzzing.md](fuzzing.md)).

With `user.test=1` the kernel runs three ring-3 programs after the kernel
self-test, so the user markers appear after `CRUGOS: selftest ok`, before the
`boot   :` report line and before the phase marker; `user.test=2` runs those
three plus the two-instance isolation pair, whose five lines sit between the
fault program and the summary, and `user.test=3` then runs `/bin/sh` on
`/etc/sh.rc`, whose shell lines sit between the isolation summary and the
`user selftest ok` line; `user.test=4` continues after the shell with
`/bin/spawn`, whose lines are shown below, `user.test=5` then runs
`/bin/fork`, whose lines follow the spawn stage, and `user.test=6` adds the
large-fork probe `/bin/forkbig`. A `make test-user3` boot ends:

```
CRUGOS: user selftest start
CRUGOS: user hello from ring 3
CRUGOS: user program /bin/hello finished (exit 0)
CRUGOS: user badptr blocked (EFAULT)
CRUGOS: user program /bin/badptr finished (exit 0)
CRUGOS: user fault writing above the window
CRUGOS: trap report page fault vector=14 error=0x6 ist=0 from=user
CRUGOS: trap fault present=no write=yes user=yes reserved=no instruction=no cr2=0x10000000000
CRUGOS: user fault page fault at rip=0x0000008000010064 err=0x0000000000000006 tid 10
CRUGOS: user fault killed tid 10 code -14
CRUGOS: user program /bin/fault finished (exit -14)
CRUGOS: user isolation start
CRUGOS: user isolation frames 0x28000 vs 0x38000 at 0x8000030000
CRUGOS: iso tid 11 word 0x15500b00 ok
CRUGOS: iso tid 12 word 0x15500c00 ok
CRUGOS: user isolation ok (2 program(s), 0 mismatch(es))
CRUGOS: user shell start
CRUGOS: sh start (argc 2)
CRUGOS: sh cmd ver
CRUGOS: sh cmd echo
CRUGOS: sh cmd cat
CRUGOS: sh cat /hello.txt 16 byte(s)
CRUGOS: sh cmd ls
CRUGOS: sh cmd write
CRUGOS: storage remounted read-write
CRUGOS: storage remounted read-only
CRUGOS: sh write /shout.txt 22 byte(s)
CRUGOS: sh cmd cat
CRUGOS: sh cat /shout.txt 22 byte(s)
CRUGOS: sh cmd exit
CRUGOS: sh exit 7
CRUGOS: user shell finished (exit 7)
CRUGOS: user selftest ok (3 program(s), 1 fault(s))
boot   : pmm=28575 us cpu=414 us vm=617 us traps=317 us timer=65299 us heap=494 us storage=95001 us sched=5614 us syscall=90 us total=196426 us
CRUGOS: phase 7c boot ok
```

A `user.test=7` boot, the second leg of `make test-app-bundles`, adds the app
stage after the large fork and ends its user stage with these lines (the
`app run` child's own output and the shell leg's boot report are elided
here):

```
CRUGOS: user app test start
CRUGOS: sh app count 3
CRUGOS: sh app orbit 1.0.0
CRUGOS: sh app orbit tid N
CRUGOS: sh app orbit exit 5
CRUGOS: user app ok (exit 9)
```

All three user excerpts keep the lines the check asserts; a real boot also prints the
report's `trap rip=` line and the two `trap regs` lines around the `trap fault`
line, which the curated samples above leave out.

A `pkg.test=1` boot (gate 57) adds the package stage after the user stage and
ends with these lines:

```
CRUGOS: pkg selftest start
CRUGOS: pkg selftest probe captest 1.0.0 trusted
CRUGOS: pkg selftest probe untrusted rejected (untrusted-key)
CRUGOS: pkg installed captest 1.0.0 (2 file(s), 1 cap(s))
CRUGOS: pkg selftest installed captest 1.0.0
CRUGOS: pkg updated captest 1.0.0 -> 2.0.0 (2 file(s), 1 cap(s))
CRUGOS: pkg selftest updated captest 1.0.0 -> 2.0.0
CRUGOS: pkg rejected captest.crpkg (downgrade)
CRUGOS: pkg selftest rejected downgrade (downgrade)
CRUGOS: pkg rejected captest2.crpkg (already-installed)
CRUGOS: pkg selftest rejected second install (already-installed)
CRUGOS: pkg removed captest (0 package(s))
CRUGOS: pkg selftest removed captest (0 package(s))
CRUGOS: pkg remove rejected captest (not-installed)
CRUGOS: pkg selftest rejected second remove (not-installed)
CRUGOS: pkg rejected untrusted.crpkg (untrusted-key)
CRUGOS: pkg selftest rejected untrusted (untrusted-key)
CRUGOS: pkg installed captest 1.0.0 (2 file(s), 1 cap(s))
CRUGOS: pkg selftest app registered captest
CRUGOS: pkg selftest ok (2 installed, 1 updated, 1 removed)
```

A `pkg.test=2` boot (gate 58), the capability leg, installs the bundle when it
is missing and then proves the mask the policy leaves it:

```
CRUGOS: caps selftest start
CRUGOS: caps selftest manifest 0x1 policy 0x1 granted 0x1
CRUGOS: caps selftest launched tid N caps 0x1
CRUGOS: captest caps 0x1
CRUGOS: captest cap check ok
CRUGOS: captest read ok
CRUGOS: captest denied fs.write
CRUGOS: captest denied pkg.install
CRUGOS: captest ok
CRUGOS: caps selftest ok (exit 5, caps 0x1)
```

`make test-user4` adds the spawn stage after the shell, and its lines sit between
`user shell finished` and `user selftest ok`:

```
CRUGOS: user spawn test start
CRUGOS: user spawn start
CRUGOS: user spawn tid 15
CRUGOS: user child running (tid 15)
CRUGOS: user child argc 2
CRUGOS: user child arg from-spawn
CRUGOS: user spawn child exit 5
CRUGOS: user spawn exec
CRUGOS: user exec /bin/child (tid 14)
CRUGOS: user child running (tid 14)
CRUGOS: user child argc 2
CRUGOS: user child arg from-exec
CRUGOS: user spawn test ok (child exit 5, exec exit 5)
```

The two tids are the proof: the spawned child runs as a new thread (tid 15) and
exits 5, while the exec'd child prints tid 14, which is the spawn program's own
tid, because `SYS_EXEC` replaced the image inside the calling thread. The
`from-spawn` and `from-exec` arguments distinguish the two argument blocks.

`make test-user5` adds the fork stage after the spawn stage, and its lines sit
between `user spawn test ok` and `user selftest ok`:

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
```

The parent (tid 16) forks, the clone shares seven pages and marks them
copy-on-write, the child (tid 17) reports the shared count and the copy it takes
when it writes `0x2222` (8738) into the word the parent had set to `0x1111`,
and the parent reaps it and still reads `0x1111` (4369) from its own copy. The
parent's own store then takes the sole-owner path because the child is gone, so
the `(sole owner)` fault flushes without copying, and `user fork parent write
17476` proves the write landed. The last two lines are printed by `/bin/fork`
and by the kernel self-test respectively, and both require the isolated read.

`make test-user6` adds the large-fork stage after the fork stage, and its lines
sit between `user fork test ok` and `user selftest ok`:

```
CRUGOS: user forkbig start
CRUGOS: user forkbig tid 16
CRUGOS: user forkbig child tid 17
CRUGOS: user cow fault tid 17 va=0x... (copy)
CRUGOS: user forkbig reap exit 7
CRUGOS: user forkbig ok (child exit 7, 300 page(s) isolated)
```

`/bin/forkbig` maps 300 pages of `.bss` (1.2 MiB), plants `0xA5` in the first
byte, forks, and the child writes `0x5A` to the first and last byte and exits 7.
The parent reaps exit 7 and checks that its sentinel survived and that its last
byte is still 0, which only holds if every one of the 300 pages was shared
copy-on-write instead of aliased. The fault address varies by run.

`/bin/hello` writes one line through `SYS_WRITE` and exits 0. `/bin/badptr`
passes an address above `USER_TOP`, gets `SYS_EFAULT` and still exits 0, which
is the copy boundary doing its job without a fault. `/bin/fault` stores past
the window, so the fault is real: the phase H3 report prints first with
`from=user`, the ring-3 kill lines follow and only that thread ends with code -14.
`/bin/iso` is the isolation proof: its `.iso` section pins a writable page at
`0x0000008000030000`, two instances write `0x15500000` plus the tid shifted
left 8 bits to the same virtual address and re-check their own word after 300
yields, and the kernel prints the two distinct physical frames. `/bin/sh` is
the shell proof: `user.test=3` points it at `/etc/sh.rc`, it opens and reads the
script through `SYS_OPEN`/`SYS_READ`, runs `ver`, `echo`, `cat`, `ls`, `write`
and `exit`, and the `write` remounts the root read-write, writes `/shout.txt`
and returns it to read-only, so `check-disk --expect-shell-write` finds the 22
bytes after a clean umount. `/bin/spawn` is the process proof: it starts
`/bin/child` with `from-spawn`, prints the new tid, waits for its exit code
through `SYS_WAIT` and then execs the same program with `from-exec`, so one
thread id appears before and after the image swap. `/bin/fork` is the memory
proof: it calls `SYS_FORK`, which shares the parent's writable pages with the
child copy-on-write, the child overwrites a word the parent had set to `0x1111`
and exits 3, and the parent's own word must still read `0x1111` after the wait,
which only holds if the child's store copied its frame instead of aliasing the
parent's. `/bin/forkbig` is the large-fork proof: it maps 300 pages, forks
them, lets the child dirty both ends and exits 7, and checks that its own
sentinel and trailing byte survived. No other gate may print a `CRUGOS: user`
line: `scripts/boot-check.py` takes the user expectation as `EXPECT_USER`
(argv[27]), the isolation
expectation as `EXPECT_USERS` (argv[28]), the shell expectation as
`EXPECT_USHELL` (argv[29]), the spawn expectation as `EXPECT_SPAWN`
(argv[30]), the fork expectation as `EXPECT_FORK` (argv[31]) and the large-fork
expectation as `EXPECT_FORKBIG` (argv[32]). Phase 7A adds no positional: the app
bundle expectation is the flag `--expect-app=yes`, so the plain boot markers
above stay
the contract for every gate except `make test-user` (gate 48),
`make test-user2` (gate 49), `make test-user3` (gate 50),
`make test-user4` (gate 51), `make test-user5` (gate 52),
`make test-user6` (gate 53) and `make test-app-bundles` (gate 56). Phase U6
widened the argument guard to
`if len(sys.argv) < 3 or len(sys.argv) > 33:` so argv[32] fits; see
[userspace.md](userspace.md).

The `virtio : net eth0 mac ...` line, the lease, the ARP resolution, the echo
reply and `net selftest ok` appear only when a NIC is present and the network
self-test runs; without one the kernel prints `CRUGOS: net selftest skipped (no
network device)` and the boot still succeeds. The test gate wires a slirp
user-mode NIC whose DHCP server hands out `10.0.2.15/24` and whose gateway
`10.0.2.2` answers ARP and echo. The five `tcp` lines appear only when the
loader command line carried `net.tcp=<port>`: `make test-tcp` runs
`scripts/run-net-tcp.py`, which answers the guest over slirp's host loopback and
fetches the guest listener through a QEMU host forward. The five `dns` lines
appear only when the loader carried `net.name=<host>`; `make test-net-dns` runs
`scripts/run-net-dns.py`, which answers DNS over TCP on the host loopback and
reaches the guest through slirp. See [network.md](network.md).

The `usb` lines appear only on machines with an xHCI function: a default `q35`
boot prints `CRUGOS: usb controller absent`, while `make test-usb-hotplug` adds
`-device qemu-xhci` and hotplugs a keyboard over QMP, so the log shows the
controller, the enumerated device and one `usb key` line per typed character.
`make test-usb-mouse` does the same with `usb-mouse` and prints `usb mouse
ready`, one `usb mouse move` line per report and a `usb mouse button` line for
the button edge. See [usb.md](usb.md).

The `gfx` lines appear only in gate mode: `make test-gfx` boots with
`gfx.wait=`/`gfx.moves=`, hotplugs a USB mouse and compares QEMU screendumps
before and after each injected move. A normal boot prints neither those lines
nor `CRUGOS: gfx cursor shown at`. See [gfx.md](gfx.md).

The `wb` lines appear only with `wb.enable=1`: `make test-workbench` boots that
way, hotplugs a `usb-mouse` and drags a window by its title bar, so the log
shows the desktop, the two demo windows, the drag lines and
`CRUGOS: wb selftest ok`. The gate then checks the active title bar in a
screendump, so a window that logs a move without repainting fails.
`make test-workbench-menu` adds `wb.menu=1 wb.acts=4` to that command line and
clicks menus, the `Run` gadget, the close gadget and a second menu, so the log
carries the `wb menu ...` and `wb action N ...` lines on top of the desktop and
window lines. `make demo` boots the whole Workbench
interactively instead of headlessly: it passes every desktop token
(`wb.enable=1 wb.wait=0 wb.desk=1 wb.dr=1 wb.dsel=1 wb.screens=1
wb.cx=1 wb.shell=1 wb.fill=<files> wb.hold=<seconds>`), so the log shows the
desktop, the four windows, `wb icon ready`, `wb drawer mode`,
`wb select mode`, `wb screens ready`, `wb cx ready (4 commodity(ies))`,
`wb shell ready`, `CRUGOS: wb holding desktop for <seconds> s` and
`CRUGOS: wb hold finished` before the guest exits with status 33.
`make test-workbench-icons` keeps `wb.desk=1` and adds `wb.ikeys=1 wb.acts=5`,
and hotplugs a `usb-kbd` as well as a `usb-mouse`, so the log carries the icon
focus, activation, drag, drop and send-to-front lines and
`CRUGOS: wb icons selftest ok`. `make test-workbench-drawer` adds
`wb.dr=1 wb.fill=24 wb.acts=4`, so the log carries the read-write remount, the
fill count, the drawer ready, scroll and refresh lines and
`CRUGOS: wb drawer selftest ok`, and the driver resizes the drawer between
screendumps to prove the rows reflowed. `make test-workbench-select` adds
`wb.dsel=1` on top of that command line (with `wb.acts=3`), so the log also
carries `CRUGOS: wb select mode`, the row-selected, scrollbar-grabbed and
scrollbar-to lines and `CRUGOS: wb select selftest ok`, while the driver clicks
rows and drags the thumb between screendumps. `make test-workbench-screens`
sets `wb.screens=1` (with `wb.acts=4`) on top of the desktop command line,
clicks the `Tools` box of the screen strip, the `Send to Workbench` gadget and
then the `Workbench` box, and checks the Tools background, the vanished
Workbench window and the moved window's amber title bar in screendumps; the run
then calls `scripts/boot-check.py --expect-screen=yes` (a flag, not a positional
argument, so the argument order and the argc range are unchanged) before its
usual check-disk pass. `make test-workbench-cx` sets `wb.cx=1` (with
`wb.acts=4`) on that same desktop line, clicks the Mute row at screen
(600,235), presses F4 while the commodity is disabled and then twice more with
it enabled, presses F6 for the Clock, and checks the Exchange body and active
title bar in a screendump; the run then calls
`scripts/boot-check.py --expect-cx=yes` (again a flag, so the argv layout is
unchanged) before its check-disk pass. `make test-app-bundles` has two legs:
the first sets `wb.app=1` on the desktop line, double-clicks the Applications
icon at (48,324) and the first row in the window it opens, and calls
`scripts/boot-check.py --expect-app=yes` (again a flag, so the argv layout is
unchanged); the second runs `make run CMDLINE_EXTRA="user.test=7"` and drives
the registry from the shell. Both pass check-disk. See [workbench.md](workbench.md),
[apps.md](apps.md) and [build.md](build.md).

`make test-workbench-pkg` (gate 59) sets `wb.pkg=1 wb.acts=3` on that
desktop line, presses the Packages window body three times (install, remove and
reinstall, with one retry allowed) and checks in screendumps that the body first
has no amber row, then gains the amber `installed` line and later the
`removed` one; it then calls
`scripts/boot-check.py --expect-wbpkg=yes` (again a flag, so the argv layout is
unchanged) and `scripts/check-disk.py`, and the driver prints
`wbp: highlight pixels after the reinstall 153` followed by
`ok: workbench package toggle (2 installs, 1 remove, exit 33)`.

`make test-pkg` (gate 57) boots with `pkg.test=1` and `--expect-pkg=yes`, and
`make test-caps` (gate 58) boots with `pkg.test=2` and `--expect-caps=yes`;
both are flags too, so the positional expectations above are unchanged. The
first proves the signed package installs, updates to 2.0.0, is refused when the
header goes backwards, is removed and reinstalled, and that the untrusted one is
refused; the second proves the installed bundle runs with `fs.read` only. See
[packages.md](packages.md) and [build.md](build.md).

The `audio` lines appear only on machines with a USB audio function: a default
`q35` boot prints `CRUGOS: audio device absent`, while `make test-audio` adds
`-device usb-audio` behind `qemu-xhci` on an `-audiodev none` backend, so the log
shows the attached function, the mixer state, a streamed half second of a tone
and either captured frames or the "capture unavailable" marker. See
[audio.md](audio.md).

`raid ready` and `raid healthy`/`raid degraded` appear only on the RAID
root path, and the `crypt` lines only on the encrypted path; the single-disk
`crugroot` path goes straight from `gpt ready` to `ext4 ready`. The console
prompt lines appear only when the loader carried no `crypt.pass=` token; the
passphrase can come from the serial line or from the PS/2 keyboard that
`input.device` decodes (see [input.md](input.md)), and the serial log contains
the mask and the markers, never the passphrase. A failed
unlock prints `CRUGOS: crypt unlock failed` (or a `crypt locked` variant) and
then `CRUGOS: no root filesystem`, and the kernel exits with status 35 (a
deterministic boot failure, not a panic).

Phase H1's crafted-image gate checks the same fail-closed property from the
other direction. A disk whose superblock, root inode or crypt header has been
patched is rejected with `CRUGOS: ext4 mount rejected (geometry)`,
`CRUGOS: ext4 mount rejected (root extent)`,
`CRUGOS: crypt unlock failed (kdf policy)` or
`CRUGOS: crypt unlock failed (volume exceeds partition)` and the same status
35; `make test-hardening` boots six such images plus an unpatched control and
also forbids
`CRUGOS: panic` and a page fault. See [hardening.md](hardening.md).

A stick booted from the hybrid ISO is the same boot with the storage block
replaced by the skip marker, and the USB device the firmware booted from shows
up in the USB lines as an unsupported mass-storage function:

```
CRUGOS: version 0.45.0
CRUGOS: usb controller ready (xhci, 8 port(s))
CRUGOS: usb device 46f4:0001 on port 1 (speed 4)
CRUGOS: usb device class 8 on port 1 (unsupported)
CRUGOS: storage skipped (storage=none)
CRUGOS: heap check ok (6 slab(s), 8 object(s), 1 large block(s))
CRUGOS: selftest ok
CRUGOS: phase 7c boot ok
```

`kernel/core/report.c` prints the machine description between them: kernel
extent, firmware pointers, memory totals, allocator totals, framebuffer mode,
redacted command-line length, entropy, CPU features, W^X audit, heap totals, scheduler switches,
per-stage microsecond timings and the timer source/frequency.

## Console

Before `console_init` only the 16550 UART at `0x3F8` (115200 8N1) is used. After
init, newlines are also mirrored to the GOP text console, which uses an embedded
8x16 font generated from a host PSF by `tools/gen-font.py`.

Input has one entry point, `console_try_getc()` in `kernel/core/console.c`: it
drains the serial line first and only then polls the i8042 keyboard and runs
`input.device`'s registered poll hook (which drains the xHCI event ring) before
reading the byte queue. See [input.md](input.md) for the scancode decoder and
[usb.md](usb.md) for the USB side.

## Known limitations

- The identity map of normal memory is limited to the low 4 GiB, although
  `vm_map_page()` can map any canonical address (the NVMe BAR at
  `0xc000000000` uses this; the AHCI BAR at `0x810c4000` is already inside the
  identity map, so the driver reprotects its pages with `vm_protect_page()` and
  `PCD|PWT`). Phase U2 maps the user window through a per-thread PML4, but a
  higher-half kernel and demand paging are still outstanding.
- Since phase U2 every ring-3 thread owns its PML4: slot 0 is copied from the
  kernel root so the shared identity map and kernel stacks stay present, and
  slot 1 holds the thread's 512 GiB to 1 TiB window. Two user programs can run
  at once and the same virtual address maps to different frames, but that is
  isolation between user programs, not against the kernel: slot 0 is still
  shared. Phase U3 adds file descriptors and an exec-time `argv` block, phase U4
  adds spawn, wait and in-place exec, and phase U5 adds `SYS_FORK`, which copies
  the calling user window into a new address space. Phase U6 makes that copy
  lazy: writable leaves are shared with `VM_COW` and copied on the first store
  on either side, read-only text leaves are shared plainly (still tracked in the
  reference table, so the last space to drop one frees it), and the cap rises to
  4096 pages. A fork whose shared frames would overflow the fixed 4096-slot
  reference table fails with `ENOMEM` instead of falling back to an eager copy,
  and the child inherits memory only (its descriptor table starts empty, so it
  inherits no open file). There is still no
  environment, no pipes or seek, no wait-any or signals, and descriptors are per
  thread; there is still no `SYSCALL` MSR gateway, `int 0x80` reaches the same
  dispatcher.
- RAID support is RAID1 and RAID10: one `md0` array. A fully blank member is
  rebuilt at boot before ext4 mounts; there is no online rebuild, no hot
  spare, no scrub, no write-intent bitmap and no generation bump. The kernel
  writes metadata only for members it rebuilds and never rewrites a valid
  member; a non-zero but invalid record is fatal.
- Only the boot CPU runs. The LAPIC is enabled and `%gs` per-CPU data exists,
  but AP startup, IOAPIC and external interrupts are still outstanding.
- USB input is polled from the console read path: no xHCI interrupt handler, one
  controller, no hubs and boot-protocol HID keyboards only.
- Empty slab pages are not returned to the PMM, and page-table pages created
  while splitting huge pages are not reclaimed.
- Storage is writable with JBD2 replay, ordered metadata journaling and
  ext4/JBD2 checksums, over legacy virtio-blk, NVMe and AHCI/SATA roots with
  polled requests and a single extent-tree level.
- Encryption is a single `crypt.device` over one partition: AES-256-XTS has no
  per-sector authentication, there is no TPM or keyfile slot, no rekey tool, and
  RAID below encryption is not implemented yet. The passphrase can be typed on
  the serial console or on a PS/2 keyboard (masked, three attempts, per-attempt
  timeout), but there is no framebuffer/GOP console input, no USB HID keyboard
  and no virtual-terminal switching yet; the
  `crypt.pass=` loader token remains the non-interactive path and is
  ESP-visible. The header and keyslots are MAC-protected, the passphrase, keys
  and bounce buffer are wiped by `storage_lock()` before the final marker, and
  the boot report redacts the command line.
- Networking stops at a polled IPv4 host plus a minimal TCP transport: no DNS
  resolver, routing table, fragmentation/reassembly or IPv6; TCP has no options,
  congestion control, SACK, TIME_WAIT timer, window probing or out-of-order
  buffering, and the socket layer is six fixed syscalls over four concurrent
  sockets with fixed 5000 ms timeouts. ARP entries never expire and there is no
  DHCP renewal; UDP receive checksums are not verified and the stack has no
  authentication or firewalling. At most four NICs and four UDP port bindings.
- `SMEP`/`SMAP` are enabled only when the CPU reports them; `qemu64` does not.
- No shutdown/reset path; QEMU's debug port is the exit mechanism.
- Desktop objects are draggable (one at a time), keyboard-focusable and
  selectable, but they cannot be renamed, there is no multi-select and no
  rubber-band selection. A drawer reads the root once on open and scrolls one
  row per arrow key; `r` re-reads the listing, but nothing refreshes it
  automatically, the listing is a flat read of the root and only one drawer
  window exists. Since phase 6H a click selects a single row and the scrollbar
  thumb drags, but there is no marquee or multi-select, a refresh paints no
  selection, the track has no page-step buttons and the keyboard has no row
  cursor in the drawer (the arrow keys still scroll the viewport).
