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
Phases 1 through 5C did not change the layout; phase 3B-9 is the first
kernel to consume the `command_line` field, phase 3B-10 falls back to an
interactive serial passphrase prompt when no token is present, phases 4C and 4D
read the non-secret `net.tcp=` / `net.serve=` / `net.dns=` /
`net.dns-tcp=` / `net.name=` tokens from the same field, phase 5A adds
`usb.wait=` / `usb.keys=`, phase 5B adds `usb.mouse=` / `usb.moves=`, and phase
5C adds `audio.rate=` / `audio.volume=` / `audio.seconds=`. The
loader caps
the command line at 511 bytes and the kernel wipes the page after it copies the
`crypt.pass=` value.

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
7. `heap_init`: slab caches and large-allocation headers. `CRUGOS: heap ready`.
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
   the driver attached to interface class 0x01.
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
   from the loader command line and wipes that page. A partition named
   `crugroot` is mounted directly; otherwise a partition named `crugcrypt` (or
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
    otherwise.
14. `report_boot`, `storage_lock` (wipes the passphrase, the volume keys and
    the bounce buffer), `CRUGOS: phase 5c boot ok`, `arch_exit_qemu(0x10)`.

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
2048 bytes. Each slab page starts with a 64-byte header (magic, class, object
and used counts, a 256-bit used map and a free list); objects follow it.
Allocations larger than 2048 bytes, up to the 1 MiB cap, take contiguous PMM
pages with a 64-byte large-allocation header. Slab pages are not
returned to the PMM when they become empty (a phase 2 simplification); large
allocations are returned immediately. All heap paths run with interrupts
disabled.

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
CRUGOS: sched ready
CRUGOS: syscall ready
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
CRUGOS: selftest ok
CRUGOS: phase 5c boot ok
```

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
  `PCD|PWT`). User address spaces, a higher-half kernel and
  demand paging arrive with user-mode address spaces.
- Execution is kernel-mode only. There is no user mode, no address-space switch
  and no `SYSCALL` MSR gateway yet; `int 0x80` reaches the same dispatcher.
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
