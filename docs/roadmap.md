# Roadmap

The specification defines 68 phases/gates. Phase 0 established the smallest
end-to-end path (firmware -> loader -> kernel -> report -> deterministic exit);
phase 1 makes the kernel own physical memory, page tables, exceptions and
timers; phase 2 adds the execution model (heap, threads, scheduling, handles,
IPC and syscalls); phase 3A adds a working block/GPT/ext4 read-only path to a
real disk; phase 3B-1 makes that filesystem writable and gates it on
`e2fsck`; phase 3B-2 adds JBD2 journal replay and ordered metadata journaling;
phase 3B-3 mirrors the root across two disks and boots from a degraded array;
phase 3B-4 adds a polled NVMe root path; phase 3B-5 stripes the root across
RAID10 mirror pairs and boots with one member missing per pair; phase 3B-6
rebuilds a blank replacement member at boot; phase 3B-7 boots the same
GPT/ext4/RAID stack from an AHCI/SATA disk; phase 3B-8 adds ext4/JBD2
checksums; phase 3B-9 encrypts the root with AES-256-XTS and an Argon2id
derived key; phase 3B-10 unlocks that volume with a masked passphrase typed on
the serial console; phase 4A adds a polled PS/2 keyboard whose scancodes feed
`input.device` and a merged console read path; phase 4B brings up IPv4 over a
polled virtio-net NIC, with a DHCP lease, ARP resolution and an ICMP echo
self-test; phase 4C adds a minimal TCP transport with a client, a listener, an
HTTP exchange in both directions and six socket syscalls; phase 4D adds a
caching DNS stub resolver with a UDP query, a TCP fallback and a name syscall;
phase 5A adds a polled xHCI host controller, a USB HID boot keyboard and a
hotplug gate; phase 5B adds a USB HID boot mouse, a four-device table on that
controller and a pointer device; phase 5C adds `audio.device`, a per-channel
mixer and a USB Audio Class driver with an isochronous xHCI path; phase 5D adds
`gfx.device`, a pointer-driven sprite composited onto the GOP framebuffer
console; phase 6A adds `intuition.device`, a software window manager of four
stacked, draggable windows, and the two-window Workbench desktop; phase 6B adds
menu bars with popups, body gadgets, a close gadget and a typed event queue;
phase 6C adds a live shell window with a scrollback text grid, a line editor and
a command set over the mounted ext4 root; phase 6D makes every window resizable
from a corner grip and lets Tab, the arrow keys and Enter drive focus, window
movement and the menu popups; phase 6E adds desktop objects that select or open
a `System:` drawer, and a depth gadget that sends a window to the back of the
stacking order; phase 6F makes those icons draggable, gives every title bar a
send-to-front gadget beside the depth box and gives the icon table keyboard
focus; phase 6G makes the drawer a scrolling viewport with arrow keys, a
refresh key and a scrollbar, and fills the root from a boot token so there is
something below the fold; phase 6H hands a window's body presses, motions and
releases to a registered handler, which the drawer uses to select a row on a
click and to drag its scrollbar thumb; phase 6I gives Intuition up to three
retained screens, a screen strip in the menu bar, per-screen windows and icons,
and a window move between screens; phase 6J adds a Commodities Exchange, a
small registry of global hotkey utilities plus an Exchange manager window whose
rows toggle them; phase 7A turns programs into `Applications` bundles, a
`manifest.toml` with Amiga-style tool types beside a user ELF, with a registry
built at boot and both a desktop window and a shell builtin to launch them.
Release
`v0.23.1` adds packaging rather than a spec phase: `make iso` produces a hybrid
UEFI ISO that boots from a USB stick or a CD-ROM, with the kernel's
`storage=none` token so an image with no root filesystem still passes the boot
contract.
Every later phase keeps the same BootInfo contract, so the loader does not
change for memory management, storage, SMP or drivers.

## Phase 0 - bootstrap (done)

- UEFI loader: file I/O, ELF64 loading, GOP, RSDP, memory map, entropy,
  ExitBootServices, handoff.
- Kernel: freestanding Crug, serial/debugcon/framebuffer consoles, `ksnprintf`,
  panic with stack scan, boot report, size budget, host unit tests, QEMU boot test.

## Phase 1 - memory and CPU (done)

- Loader invariant: post-snapshot allocations (kernel, framebuffer, BootInfo and
  the normalized map itself) are marked `BOOTLOADER`/`KERNEL`/`FRAMEBUFFER`, and
  boot stops if any `USABLE` range overlaps them.
- Bitmap physical allocator over `BootInfo.memory_map`: 4 KiB frames, one bit
  per frame, 1 = used. Never allocates the kernel, bitmap, framebuffer, ACPI or
  boot metadata; allocation zeroes the frame and supports contiguous/aligned
  ranges.
- Kernel page tables: identity map of 0..4 GiB with 2 MiB huge pages, 4 KiB
  granularity for the kernel image and the boot stack. `W^X`: `.text` is
  read/execute, `.rodata` is read-only NX, `.data`/`.bss` are writable NX, and a
  4 KiB guard page sits below the boot stack.
- GDT/TSS, per-CPU data via `%gs`, IDT with 256 vectors, exception dispatch
  (`#BP` returns; other faults panic), double-fault IST stack and `int3` selftest.
- LAPIC + timer: PIC masked, TSC frequency from CPUID 0x15/0x16 or a PIT
  calibration, TSC-deadline when available, otherwise LAPIC periodic at 100 Hz;
  monotonic nanoseconds and calibrated boot-stage timings.
- AVX2 detection and XSAVE enablement (`CR4.OSXSAVE`, `XCR0=0x7`); the default
  build requires AVX2, `REQUIRE_AVX2=0` builds a compatibility kernel.

## Phase 2 - execution model (done)

- Kernel heap with eight slab classes (16..2048 bytes) and contiguous large
  allocations up to 1 MiB; interrupt-safe allocation and double-free detection.
- Threads with a 16 KiB stack and guard page, `cpu_switch` context switching,
  per-class run queues and the six thread states.
- Scheduling classes `IDLE`, `NORMAL`, `INTERACTIVE`, `REALTIME_SYSTEM`
  with round-robin quanta, timer preemption, sleep/wakeup queues, join and an
  idle thread.
- Per-process typed handle tables (256 entries, process-local ids, no kernel
  pointers) and a kernel process.
- `MsgPort` IPC with typed messages (1024-byte payload, depth 64), O(1)
  send/receive and blocking senders/receivers.
- A single syscall dispatcher behind an `int 0x80` DPL3 gate, with ping,
  gettid, yield, sleep, port create/send/recv, handle close and exit.
- Boot self-tests for the heap, handles, scheduler, IPC and syscalls.

## Phase 3A - storage vertical slice (done)

- Block layer with a device registry, bounds-checked reads/writes and
  partition offset views.
- Legacy PCI virtio-blk driver (`vda`), polled single-request I/O, 2..256-entry
  queue and a 500 ms completion timeout.
- GPT parsing: protective MBR, header and entry CRC32, up to 16 partitions,
  UTF-16 names; `storage_init()` prefers the `crugroot` partition.
- Read-only ext4: 1/2/4 KiB blocks, 32/64-byte group descriptors, extents and
  the legacy single-indirect mapping, linear directories, clean-state check.
  Rejects journal recovery, htree, `metadata_csum`, `inline_data`, `encrypt`
  and `meta_bg`.
- Generated 16 MiB GPT/ext4 test disk (`make disk`) with `/hello.txt`, read and
  verified during boot; `block ready`, `gpt ready`, `ext4 ready` and
  `storage ready` markers.

## Phase 3B-1 - writable ext4 (done)

- Write path: `ext4_create`, `ext4_mkdir`, `ext4_write_file`, `ext4_truncate`,
  `ext4_unlink`, `ext4_rmdir`, `ext4_rename` and `ext4_fsync`, with extent
  allocation and one extent-tree leaf level.
- Consistent allocation: block bitmap bits relative to
  `s_first_data_block + group * blocks_per_group`, inode bitmap bits relative to
  `group * inodes_per_group`, group free counts plus `used_dirs_count`, and
  parent link counts persisted on every directory change.
- Block-layer flush forwarding and the legacy virtio `VIRTIO_BLK_T_FLUSH`
  command, used by `ext4_fsync` and clean unmount.
- Boot self-test that creates, writes, renames, truncates, deletes, unmounts
  cleanly and remounts read-only to verify persistence.
- Host on-disk gate: `scripts/check-disk.py` extracts the partition, requires
  `e2fsck -fn` to be clean and checks the persisted `/hello.txt` and
  `/selftest/b.txt` contents. `make test` runs it automatically.

## Phase 3B-2 - JBD2 journaling (done)

- JBD2 V2 journal in inode 8: descriptor/commit blocks, big-endian tags,
  sequence/start handling and a 1 MiB journal generated by `mkdisk.sh`.
- Ordered write path: file data is flushed before the journal descriptor, then
  metadata is journaled, committed, checkpointed and the journal is cleared.
- Replay at mount: a dirty journal is validated (commit block checked before
  any write), applied and cleared; read-only mounts of a dirty journal are
  refused, read-write mounts recover.
- Boot self-test leaves a dirty journal, unmounts, remounts and verifies that
  replay restored the block; `e2fsck` still passes on the resulting disk.
- Transaction shape is one descriptor (42 metadata blocks for 1 KiB blocks);
  larger operations fail rather than emit an unreplayable transaction.

## Phase 3B-3 - RAID1 (done)

- Multiple legacy virtio-blk devices (`vda`..`vdh`) probed through every PCI
  function; each block device keeps its own virtqueue and flush support.
- 512-byte checksummed RAID1 metadata in the last two sectors of each member:
  magic `CRA1`, level, slot, member count, geometry, generation and UUIDs.
- `raid_assemble()` validates members, tolerates missing ones and registers
  `md0`; reads use the first healthy member, writes/flushes go to all healthy
  members, and a failed member is dropped for the rest of the boot.
- `storage_init()` still prefers a direct `crugroot` partition and otherwise
  assembles `md0` from `fd00` partitions; `raid ready`,
  `raid healthy`/`raid degraded` and the existing ext4/JBD2 path follow.
- Host tools `mkraid.sh`, `raid-meta.py` and `check-raid.py`; `make
  test-raid` gates a healthy two-member boot, `make test-raid-degraded` gates
  a one-member boot, and `check-raid.py` validates metadata, `e2fsck` and
  data-region equality.

## Phase 3B-4 - NVMe (done)

- Polled NVMe driver for PCI class `01/08/02`: 64-bit (possibly above 4 GiB)
  BAR sizing and mapping through generic page tables, controller disable/reset,
  admin queue, one I/O queue pair, identify namespace 1.
- Read, write and flush commands with PRP1/PRP2/PRP-list bounce buffers and a
  1000 ms deadline; namespace registered as `nvme0n1`.
- `storage_init()` probes virtio-blk and NVMe, parses every GPT and mounts a
  `crugroot` partition (or RAID1) regardless of the transport.
- `make test-nvme` boots with only an NVMe image, requires `nvme ready` and
  `nvme0n1`, and runs the same JBD2/writable-ext4 self-tests plus `e2fsck`.

## Phase 3B-5 - RAID10 (done)

- RAID metadata now carries level 1 or 10 and a chunk size; RAID10 requires an
  even 4..8 members and maps sector ranges to mirror pair
  `(lba / chunk) % (member_count / 2)` with slots paired as `2p`/`2p+1`.
- `raid_assemble()` validates level/chunk/geometry, unique slots and pair
  coverage, then registers `md0`; reads use a present member of the pair,
  writes go to both, and assembly fails if a pair has no member.
- `storage_init()` prints the array geometry (`raid   : level 10, 4 member(s),
  2 pair(s), chunk 128 sectors`) before `raid healthy`/`raid degraded`, and the
  ext4/JBD2 path is unchanged.
- Host tools `mkraid10.sh` and `check-raid10.py` build and verify four-member
  arrays; `check-raid10.py` rebuilds the logical image from one member per pair
  before `e2fsck`.
- `make test-raid10` gates a healthy four-member boot and `make
  test-raid10-degraded` gates a two-member boot (slots 0 and 2).

## Phase 3B-6 - RAID cold-spare rebuild (done)

- Metadata probing now distinguishes a valid member, a fully blank member (both
  metadata sectors zero) and a corrupt one. Blank members are cold spares
  instead of fatal errors; non-zero invalid metadata is still rejected.
- `raid_assemble()` assigns each spare to the lowest missing slot and rebuilds
  it synchronously from a present member of the same mirror: RAID1 copies the
  whole data region, RAID10 copies only the chunks owned by its pair, in
  64-sector chunks followed by a flush.
- The kernel writes fresh checksummed metadata to the rebuilt member (current
  geometry, array UUID, generation, new member UUID), flushes, counts it as
  present and reports `CRUGOS: raid rebuilt (N member(s))` before
  `raid healthy`. The rebuild happens before ext4 mounts and does not bump the
  array generation.
- A rebuild failure leaves the slot missing, so the array boots degraded; extra
  spares with no missing slot are reported and ignored. A RAID10 pair that lost
  both members still cannot be assembled.
- Host tool `scripts/raid-blank.sh` zeroes a member partition while keeping its
  GPT, simulating a replacement disk. `make test-raid-repair` gates one rebuilt
  RAID1 member and `make test-raid10-repair` gates two rebuilt RAID10 members;
  `check-raid.py`/`check-raid10.py` then validate the written metadata and the
  filesystem on every member.

## Phase 3B-7 - AHCI/SATA (done)

- `drivers/block/ahci.c` discovers controllers by PCI class `01/06/01` (q35
  ICH9 at `00:1f.2`), caps ports by `CAP.NP` and by the mapped BAR window, and
  brings up a command list, FIS area and command table per populated port.
- `IDENTIFY DEVICE` supplies the 48-bit LBA capacity; drives register as
  `sda`..`sdh` and use `READ/WRITE DMA EXT`, `WRITE DMA EXT` and `FLUSH CACHE
  EXT` with an 8 KiB bounce buffer and a single PRDT entry.
- BAR sizing fixes: the 32-bit mask must set the upper 32 bits before
  `~mask + 1`, and a BAR that already falls inside the 4 GiB identity map is
  reprotected with `vm_protect_page()` (`PCD|PWT`) instead of `vm_map_page()`.
- Empty ports use a 20 ms link window instead of the 1000 ms command timeout,
  which removed 4 s from a q35 boot with four unpopulated ports.
- `storage_init()` probes virtio-blk, NVMe and AHCI together and prefers a GPT
  partition named `crugroot`; the serial log names the root device.
- `make test-ahci` boots from a SATA disk attached on `ide.1` and then runs
  `check-disk.py`, so the SATA path is gated by `e2fsck` and file contents.

## Phase 3B-8 - checksums (done)

- `kernel/lib/crc32c.c` implements the Castagnoli CRC used by ext4 and JBD2;
  `tools/gen-crc32c.py` generates it and `tests/host/test_crc32c.c` pins 629
  vectors including the seed-0 empty value and `0x1CF96D7C` for
  `"123456789"`.
- ext4 `metadata_csum` is written: superblock, group descriptors, block and
  inode bitmaps, inodes, directory tails (chained with the inode generation)
  and extent tails.
- JBD2 checksum v3 journals: 16-byte tags, a checksummed journal superblock,
  a descriptor block tail and a commit-block checksum; legacy and v2 journals
  still mount.
- Transactions span multiple descriptors (62 blocks per 1 KiB v3 descriptor)
  instead of failing when one descriptor fills up, and the journaled
  superblock is part of the transaction.
- A dirty journal in the test image is replayed at boot: the RO mount is
  refused, the kernel remounts read-write, prints
  `CRUGOS: journal replayed N block(s) at mount` and verifies the contents;
  replay validates the whole transaction before writing and fails closed on a
  checksum mismatch.
- Gates: `make test` (e2fsck-verified metadata checksums),
  `make test-journal-csum` (e2fsck replays our journal) and
  `make test-journal-replay` (the kernel replays it). See
  [checksums.md](checksums.md).

## Phase 3B-9 - encryption (done)

- `crypt.device` (`kernel/block/crypt.c`) is a GPT partition with type GUID
  `8f1c7d2e-3b45-4c6a-9e01-c2a1b7f0d9e3` named `crugcrypt`: a 16 KiB header with
  eight 256-byte keyslots and an AES-256-XTS data area whose XTS tweak is the
  data-relative 512-byte sector number.
- The passphrase is stretched with Argon2id (RFC 9106) into a 96-byte
  key-encryption/MAC key pair that unwraps a random 64-byte master key; each
  keyslot carries a keyed BLAKE2b-256 MAC over the header and slot, and a
  recovery passphrase can be sealed as a second slot.
- AES-256-XTS is the P1619 XEX construction with AES-NI dispatch and a software
  fallback; it is pinned to IEEE 1619 vector 10, and Argon2id is pinned to
  libargon2 vectors. BLAKE2b provides the MAC.
- `storage_init()` prefers `crugroot`, then unlocks a `crugcrypt` partition
  (`CRUGOS: crypt ready ...`, `CRUGOS: crypt aes: ...`), then falls back to
  RAID. The loader reads `\crugos\cmdline.txt` into `boot_info`; the kernel
  consumes `crypt.pass=`, wipes the loader page, redacts the command line in
  the boot report, and `storage_lock()` wipes the passphrase, the volume keys
  and the bounce buffer before the final marker.
- Host tools `tools/mkcrypt.c` (`format`, `extract`), a `crypt` mode of
  `scripts/mkdisk.sh`, `scripts/tamper.py`, and five gates: healthy unlock,
  recovery keyslot, wrong passphrase, tampered MAC and AES-NI. See
  [crypto.md](crypto.md).

## Phase 3B-10 - console unlock (done)

- `crypt.device` can now be unlocked from the console: when the loader command
  line has no `crypt.pass=`, `storage_prompt_unlock()` prints
  `CRUGOS: crypt passphrase required` and reads a masked line from COM1,
  echoing only `*`.
- The line editor (`kernel/lib/lineedit.c`, `include/crug/lineedit.h`) handles
  CR/LF, backspace, Ctrl-C and overflow past
  `CRUG_CRYPT_MAX_PASSWORD`; it is pinned by host tests, so
  `make test-host` now covers 809 checks.
- Three attempts are allowed, each bounded by `crypt.prompt_timeout=`
  (1..3600 seconds, 60 s per attempt by default); a wrong line prints
  `CRUGOS: passphrase rejected (N attempt(s) left)`, and giving up prints
  `CRUGOS: crypt locked (too many attempts)` or
  `CRUGOS: crypt locked (no passphrase)` for an empty, cancelled or timed-out
  prompt. Every line buffer is zeroed and the secret is never logged.
- `scripts/run-crypt-prompt.py` drives QEMU's serial port over TCP
  (`SERIAL_TCP`), answers one prompt per attempt and tees the console into the
  normal serial log; three gates cover a correct typed passphrase, three
  rejections and an unanswered timeout.
- All 26 gates pass at `v0.14.0`; an encrypted boot stays well under a second
  with software AES under TCG.

## Phase 4A - console input (done)

- `kernel/input/input.c` implements `input.device`: a set-1 scancode decoder with
  make/break extraction, the `0xE0` prefix, Shift/Ctrl/Alt/Caps state, a US
  layout table, named keys (Enter, Tab, Backspace, Esc, space), a Ctrl mapping
  to control codes, and a 64-byte byte queue drained by `input_try_getc()`.
- `kernel/arch/x86_64/keyboard.c` programs the i8042 (translation on, IRQ1 and
  IRQ12 masked, scanning enabled) and polls the output buffer from
  `keyboard_poll()`; a missing controller is reported instead of being fatal.
- `console_try_getc()` merges the inputs: the serial line is drained first, then
  the keyboard is polled and `input.device` is read, so the crypt passphrase
  prompt accepts a real keyboard with no change to the prompt logic.
- The boot prints `CRUGOS: input ready (serial + keyboard)` (or
  `(serial only)`) and the prompt wording is now
  `CRUGOS: type the passphrase on the console (serial or keyboard) (3 attempt(s))`.
- `scripts/run-crypt-keyboard.py` injects keystrokes over QEMU's QMP monitor
  (`send-key`, one KeyValue event per character) while the serial line stays a
  write-only log, so the keyboard path is the only input. Two gates cover a
  correct typed passphrase and three wrong ones; `make test-host` covers 903
  checks including the scancode decoder. See [input.md](input.md).
- All 27 gates pass at `v0.15.0`.

## Phase 4B - networking (done)

- `net.device` (`include/crug/net.h`, `kernel/net/net.c`): device registry,
  static frame buffers, big-endian wire helpers, an 8-entry ARP cache, four UDP
  port bindings and a boot self-test.
- Legacy virtio-net PCI driver (`drivers/net/virtio_net.c`): only
  `VIRTIO_NET_F_MAC` negotiated, 32 pre-posted 2 KiB receive buffers on queue 0,
  one 2 KiB transmit bounce buffer on queue 1 with a 500 ms completion wait, up to
  four polled interfaces.
- Ethernet with 60-byte padding and a MAC/broadcast filter, ARP request/reply and
  resolution with retries, IPv4 with header checksum validation, ICMP echo, UDP
  with a transmit pseudo-header checksum.
- DHCP client (`kernel/net/dhcp.c`): DISCOVER/OFFER/REQUEST/ACK with three
  attempts, broadcast flag, options 53/55/50/54, mask/router/DNS parsed into
  `net_config`.
- Boot integration: `net_init()` after `input_init()` prints
  `CRUGOS: net ready (N device(s))`; `net_selftest()` prints the lease, the
  resolved gateway MAC, the echo reply and `CRUGOS: net selftest ok`, skipping
  cleanly without a NIC.
- `make test-net` boots with QEMU user-mode networking (`NET_MODE=user`, guest
  `10.0.2.15/24`, gateway `10.0.2.2`) and `scripts/boot-check.py` requires the
  DHCP, ARP and ICMP markers; `make test-host` checks the wire helpers, RFC 1071
  checksums and DHCP option parsing (938 checks).
- All 28 gates pass at `v0.16.0`.

## Phase 4C - TCP and sockets (done)

- TCP transport (`kernel/net/tcp.c`): four sockets, 1400-byte segments, 4 KiB
  receive rings, connect with bounded SYN retries, listen/accept, send with
  bounded retransmission, receive and close; in-order only, no options.
- Listener and command line: `tcp_listen()`/`tcp_accept()`, `net.tcp=<port>`
  selects the remote peer for the client leg and `net.serve=<seconds>` bounds the
  served connection.
- Socket syscalls 9-14 (`CONNECT`, `SEND`, `RECV`, `CLOSE`, `LISTEN`, `ACCEPT`)
  hand typed `HANDLE_SOCKET` handles to the current process.
- `tcp_selftest()` inside `net_selftest()`: listens on 8080, fetches
  `GET / HTTP/1.0` from the gate's responder, repeats it through the syscalls
  and serves one request itself; prints `tcp listen ready`, `tcp connect ok`,
  `tcp http ok`, `tcp socket syscall ok` and `tcp http served`.
- `make test-tcp` drives both directions through `scripts/run-net-tcp.py`
  (slirp responder plus a QEMU host forward to guest port 8080) and then runs
  `check-disk.py`; `scripts/boot-check.py` takes `EXPECT_TCP=yes`; `make
  test-host` covers TCP and UDP checksums (943 checks).
- All 29 gates pass at `v0.17.0`.

## Phase 4D - DNS resolver (done)

- Caching stub resolver (`kernel/net/dns.c`): a UDP query to `net.dns=` (or the
  address from DHCP option 6) and a TCP retry with the two-byte length prefix
  when the answer is truncated or the UDP leg times out.
- Wire handling: `dns_build_query()`, `dns_parse_response()` (id, `QR` and
  rcode checks, `-2` on `TC`), `dns_decode_name()` (compression pointers must
  walk strictly backwards, 16-hop and 128-byte caps) and `dns_skip_name()` (a
  compressed field ends at `p+2`, keeping the following records aligned).
- Up to four CNAME aliases per lookup and an eight-entry TTL cache keyed by the
  lowercased name (TTL clamped to 86400 s), emptied by `dns_cache_flush()`.
- `SYS_RESOLVE` (15) resolves a name with a 3000 ms budget into a `u32 *`.
- `dns_selftest()` inside `net_selftest()` resolves `net.name=` over the
  configured transport, repeats the lookup from the cache and exercises the
  syscall; it is skipped without `net.name=`.
- `make test-net-dns` runs `scripts/run-net-dns.py` (a hermetic DNS-over-TCP
  responder on the host loopback), `scripts/boot-check.py` takes
  `EXPECT_DNS=yes`, and `make test-host` covers query building, name walking,
  parsing and the cache (1042 checks).
- All 30 gates pass at `v0.18.0`.

## Phase 5A - USB keyboard (done)

- Polled xHCI host controller (`drivers/usb/xhci.c`): PCI class `0C/03/30`, a
  64-bit BAR sized and, when firmware left it at zero, assigned from a free
  window below `0xffc00000`; HCRST, 4 KiB command/event/EP0/HID rings with
  Link TRB wrap, the event ring segment table, the device context array and an
  input context, then `RUN` with a `CRCR` readback.
- Enumeration: `ENABLE_SLOT`, `ADDRESS_DEVICE` at the default packet size,
  `GET_DESCRIPTOR` (8, 18, 9 and the full configuration), `EVALUATE_CONTEXT`
  for the real `bMaxPacketSize0`, `SET_CONFIGURATION(1)` and
  `CONFIGURE_ENDPOINT` for the interrupt IN endpoint, then one armed Normal TRB
  per report.
- HID boot decoding (`drivers/usb/usb_hid.c`): the descriptor walk accepts only
  a boot keyboard (class 3, subclass 1, protocol 1); the eight-byte report maps
  modifiers, letters with Shift XOR Caps, digits, punctuation, Enter,
  Backspace, Tab, Escape, Space and the same Ctrl folding as the PS/2 decoder,
  with held-key suppression; characters enter `input.device` through
  `input_char()`, and `usb_poll()` runs as the queue's poll hook.
- Ports and hotplug: `PORTSC` change bits are acknowledged, a connect is
  reset and enumerated, and losing the port unbinds the keyboard. Normal boots
  print `CRUGOS: usb controller absent` unless the machine has xHCI.
- `usb.wait=<seconds>` and `usb.keys=<n>` bound the optional self-test wait;
  `make test-usb-hotplug` boots with `usb=off,i8042=off` and a QMP socket,
  plugs a keyboard into `xHCI` while the guest runs, types `abc`, and requires
  the five `usb` markers, `CRUGOS: usb selftest ok`, a clean `e2fsck` and exit
  33. `make test-host` covers descriptors and the decode matrix (1117 checks).
- All 31 gates pass at `v0.19.0`.

## Phase 5B - USB mouse (done)

- The xHCI driver keeps a table of up to four devices (`struct xhci_dev`): each
  enumerated boot keyboard or mouse gets its own device context page, interrupt
  ring, report buffer and DCI, so a keyboard and a mouse coexist in either order.
  A fifth device prints `CRUGOS: usb device table is full`.
- `usb_hid_parse_config()` accepts protocol 2 (mouse) as well as protocol 1
  (keyboard), records the endpoint's kind, and rejects a mouse endpoint that
  cannot carry three report bytes. `usb_hid_mouse_decode()` turns the button
  bitmap, signed `dx`, signed `dy` and optional wheel into a
  `struct usb_hid_pointer`, with the first report after a reset acting as the
  button baseline.
- `input.device` gains a pointer state beside its byte queue:
  `input_pointer_event()` accumulates the deltas and button edges,
  `input_pointer_read()` drains them, and `input_pointer_count()` /
  `input_pointer_reset()` support tests and future consumers.
- Port scans sweep every port, enumerate while table space remains, and unbind a
  device whose port loses `CCS` (`CRUGOS: usb keyboard gone` / `usb mouse gone`).
- `usb.mouse=<seconds>` and `usb.moves=<n>` bound the optional self-test wait;
  `usb_selftest()` now reports `CRUGOS: usb selftest ok (N key(s), M pointer
  event(s))`.
- `make test-usb-mouse` boots with `usb=off,i8042=off` and a QMP socket, plugs
  `usb-mouse` into the running controller, injects relative motion and a
  left-button press, and requires the `usb mouse` markers, a clean `e2fsck` and
  exit 33; `make test-usb-hotplug` still passes unchanged and `make test-host`
  covers the mouse decode matrix and the pointer state (1168 checks).
- All 32 gates pass at `v0.20.0`.

## Phase 5C - USB audio and the mixer (done)

- The xHCI driver grew a small isochronous API: `usb_control()` for the class
  requests, and `usb_isoc_open()`/`usb_isoc_submit()`/`usb_isoc_buf()`/
  `usb_isoc_buf_pa()`/`usb_isoc_packets()`/`usb_isoc_done()`/`usb_isoc_errors()`/
  `usb_isoc_close()` for the stream. Normal TRBs carry `TRB_SIA`, without which
  QEMU schedules every packet in frame 0 of a 16384-frame window and the stream
  never runs; the endpoint context interval is encoded in microframes, so a
  full-speed `bInterval` of 1 becomes 3 (= 8 microframes = 1 ms).
- `drivers/usb/usb_audio.c` is attached by `xhci_enumerate()` when interface 0
  of the configuration has class `0x01` and is detached on unplug. It parses
  UAC1 and UAC2 descriptors (AC header, input terminal, feature unit, output
  terminal, streaming interface, format type I, endpoint), selects alternate
  setting 1, sets the sampling frequency and the hardware volume/mute, and
  keeps 32 packets of 48 frames in flight. Refilling counts only successful
  completions: counting ring-underrun events too wrapped the in-flight window
  and stalled the stream after exactly 32 packets.
- `audio.device` (`kernel/audio/audio.c`) is a 16384-frame 16-bit stereo ring.
  `audio_write()` appends, `audio_read()` pops through the mixer and zero-fills
  the rest of the packet, played/captured totals come back from the driver, and
  `audio_tone()` generates a 440 Hz test tone from a 360-entry Q15 sine table.
- `kernel/audio/mixer.c` holds a per-channel volume in 1/256 dB (from -30 dB to
  0 dB) and a mute flag, converts the attenuation with `(attenuation + 128) /
  256` to a whole dB and looks it up in a 31-entry Q15 table; mute wins and
  unity gain skips the multiply entirely.
- `audio.rate=`, `audio.volume=` and `audio.seconds=` bound the boot check, and
  `audio_selftest()` streams half a second of the tone and requires real pacing
  (`audio streamed N frame(s) in M ms`), then reports captured frames or the
  `audio capture unavailable (no input endpoint)` marker.
- `make test-audio` boots the audio gate (`usb=off,i8042=off`, `qemu-xhci`,
  `usb-audio` and an `-audiodev none` backend, so no host sound server), and
  requires the `audio` markers, a clean `e2fsck` and exit 33. QEMU's
  `usb-audio` volume handler never accepts a channel selector and only handles
  class interface *out* requests, so the volume and rate requests stall there;
  both are non-fatal and the software mixer stays authoritative.
  `make test-host` covers the UAC1 descriptor, the mixer table, the tone and the
  PCM ring (2372 checks).
- All 33 gates pass at `v0.21.0`.

## Phase 5D - Pointer-driven framebuffer cursor (done)

- `kernel/arch/x86_64/fb.c` now owns a 12x19 sprite: `fb_cursor_show_at()`
  saves the pixels it covers, draws the white outline and amber body, and every
  text path (`fb_putc()`, `fb_clear()`, `fb_scroll()`) restores, writes and
  recomposites so neither the text nor the cursor damages the other. The
  `fb_pixel_color` packing was inverted for `CRUG_PIXEL_BGRX8888` since phase 0
  and only the coloured sprite exposed it; the gate compares framebuffer colour,
  not just coordinates, so the encoding stays honest.
- `gfx.device` (`kernel/gfx/gfx.c`, `include/crug/gfx.h`) drains
  `input_pointer_read()`, moves the cursor by the raw delta with
  `GFX_CURSOR_STEP`, clamps to the screen, tracks button transitions and prints
  one `CRUGOS: gfx cursor at (X, Y)` marker per move in gate mode.
  `gfx.wait=` and `gfx.moves=` bound the boot check, and `gfx_selftest()` is
  silent unless those tokens asked for work, so no existing gate changed
  behaviour.
- `CRUGOS: gfx cursor shown at` is printed before the wait and
  `CRUGOS: gfx selftest ok (N move(s), buttons 0xNN)` after it, followed by a
  1.5 s linger so the gate's final screendump cannot race the QEMU exit.
- `make test-gfx` boots `usb=off,i8042=off` with `qemu-xhci`, hotplugs a
  `usb-mouse` over QMP, injects relative motion three times, and compares QEMU
  screendumps: the amber sprite centroid must move by the same delta the guest
  reported (2.0 pixel tolerance). It requires the `gfx` markers, exit 33 and a
  clean `e2fsck`.
- `tests/host/test_gfx.c` stubs the framebuffer and drives the real pointer
  state; under `CRUG_HOST_TEST` the `sti; hlt; cli` idle becomes a stub that
  advances the shared fake clock, so the self-test linger and timeout loops
  terminate. `make test-host` reports 2426 checks.
- All 34 gates pass at `v0.22.0`.

## Phase 6A - Workbench desktop foundation (done)

- `kernel/arch/x86_64/fb.c` gained five primitives above the console:
  `fb_fill_rect()`, `fb_frame_rect()`, `fb_draw_text()` and the
  `fb_cursor_begin()` / `fb_cursor_end()` pair that hides and restores the
  sprite around a batch of drawing, plus `fb_set_text_enabled()` so the desktop
  can take the framebuffer away from the character console. The cursor save
  buffer is a single 12x19 slot, so every composite is bracketed by the
  begin/end pair.
- `intuition.device` (`kernel/gfx/intuition.c`, `include/crug/intuition.h`)
  keeps four window slots in a stacking order, hit-tests title bars top-down,
  raises and activates on press, drags with a grab offset `(px - x, py - y)`,
  clamps to the screen, repaints the whole desktop (desktop fill, then windows
  bottom-to-top) and prints `CRUGOS: wb window N opened`, `... active`,
  `... drag start`, `... drag to` and `... moved to` markers in gate mode.
- `gfx.device` gained `gfx_set_pointer_hook()`: after it moves the sprite it
  hands the raw `struct input_pointer` to the desktop, so windows follow the
  same deltas the cursor does.
- `kernel/gfx/workbench.c` reads `wb.enable=`, `wb.wait=` and `wb.drag=`, takes
  the text console away, opens window 1 `(80, 90) 360x220 Workbench` and window
  2 `(480, 300) 320x200 Shell`, installs `usb_poll()` + `gfx_pointer_poll()` as
  the console poll hook and prints `CRUGOS: wb desktop ready (1024x768, 2
  window(s))`. `wb_selftest()` is silent unless `wb.enable=1`, and otherwise
  waits for `wb.drag=` title-bar drags before printing
  `CRUGOS: wb selftest ok (N drag(s))` and lingering 1.5 s for the gate.
- `make test-workbench` boots `wb.enable=1 wb.wait=$(WB_WAIT) wb.drag=$(WB_DRAG)`
  with `usb=off,i8042=off` plus `qemu-xhci`, hotplugs a `usb-mouse`, presses
  the title bar of window 2, injects two `(+45, +25)` motions and releases.
  QEMU screendumps then prove the active title bar is drawn at the new origin,
  the old bar is idle grey and the frame of window 1 stayed white (which pins
  the RGB packing of the new primitives). It requires the `wb` markers, exit 33
  and a clean `e2fsck`.
- `tests/host/test_intuition.c` drives the real window manager (open, stacking,
  hit test, drag, clamp, close, repaint counts) over a fake framebuffer.
  `make test-host` reports 2487 checks.
- All 35 gates pass at `v0.23.0`.

## Phase 6B - menus and gadgets (done)

- `include/crug/intuition.h` gained the 6B limits and API: two menus of six items,
  four gadgets, a 16-entry event queue, the menu-bar height and the seven menu
  colours, plus `intuition_set_menus()`, `intuition_add_gadget()`,
  `intuition_event_poll()` and the `menu_open` / `menu_hover` / counters.
- `kernel/gfx/intuition.c` draws an 18-pixel menu bar per window, opening a
  150-pixel popup at (`strip_x`, 18) with 18-pixel rows, highlights the row under
  the pointer and queues a `MENU` event on selection. Window bodies now hit-test
  gadgets in insertion order, a press starts a gadget and a release inside the
  same gadget queues `GADGET`; a 12x12 close gadget at the top right of the
  title bar closes the window on press and queues `CLOSE`. Window presses queue
  `ACTIVATE` and drags queue `DRAG`, so the queue carries every interaction, not
  only the ones that need a decision.
- Press precedence is popup row, then menu bar, then close gadget, then body
  gadget, then title-bar drag; a press anywhere in the popup that is not on a row
  dismisses it. `intuition_event_poll()` drains the FIFO, which drops the newest
  event when full and counts the drop in `intuition_event_dropped()`.
- `kernel/gfx/workbench.c` reads `wb.menu=` and `wb.acts=` on top of the 6A
  tokens and drains the queue into `CRUGOS: wb action N menu|gadget|close ...`
  lines. The two demo windows carry `Workbench`/`Window` and
  `Shell`/`Workbench` menus and window 1 a `Run` gadget, and `wb_selftest()` waits
  for drags and actions before `CRUGOS: wb selftest ok (N drag(s), M action(s))`.
- `make test-workbench-menu` boots `wb.enable=1 wb.wait=$(WB_MENU_WAIT)
  wb.menu=1 wb.acts=$(WB_MENU_ACTS)`, hotplugs a `usb-mouse` and clicks
  `scripts/run-workbench-menu.py` through a menu open, a hover (proved by a
  screendump of the highlighted row), a selection, the `Run` gadget, the close
  gadget and a second selection. Every pointer move is at most 100 pixels,
  because QEMU splits a larger relative motion into 8-bit HID reports and the
  guest drops some of them.
- `tests/host/test_intuition.c` gained menu, gadget, close and event-queue
  groups over the fake framebuffer, including the clamps, the popup dismissal and
  the 20-click queue overflow. `make test-host` reports 2639 checks.
- All 37 gates pass at `v0.24.0`.

## Phase 6C - the Workbench shell (done)

- `include/crug/textgrid.h` + `kernel/lib/textgrid.c`: a scrollback buffer of
  `TG_LINES` (96) lines of `TG_COLS` (38) characters -- 37 usable, every line
  stays NUL-terminated -- with `first` / `count` / `row` / `col`, `\n` for a new
  line, `\r` for a carriage return, `0x08`/`0x7f` for backspace and `.` for any
  other control byte, an overwrite that keeps the rest of the tail, oldest-line
  eviction on overflow, and `tg_draw()` painting the last
  `min((h - 6) / 20, 8)` lines at a 20-pixel pitch with a clipped
  inverse-video cursor cell.
- `include/crug/shell.h` + `kernel/shell/shell.c`: a 79-character line editor
  with `Ctrl-U` (erase), `Ctrl-C` (cancel and print `^C`), an eight-entry
  `Ctrl-P`/`Ctrl-N` history that restores the draft line, a tokenizer that folds
  extra words into the last argument, and the command table `help`, `ver`,
  `mem`, `uptime`, `ps`, `ls`, `cat`, `echo`, `write`, `sync`, `cls` over a
  five-call `struct shell_io` (`list`, `read`, `write`, `sync`, `info`).
- `kernel/shell/shell_ops.c` binds that interface to `ext4_list()`,
  `ext4_read_file()`, `ext4_lookup()` + `ext4_create()` + `ext4_write_file()`
  and `ext4_fsync()` on `storage_root()`, plus the heap counters,
  `sched_current()` and `sched_switches()`; a failing operation prints an
  `sh:` diagnostic instead of panicking.
- `intuition_set_paint()` and `intuition_redraw_window()` let a window body be
  painted by a callback, so the shell grid redraws on every keystroke while the
  static body-text path of the other windows is unchanged.
- `kernel/gfx/workbench.c` reads `wb.shell=1` / `wb.sh=`, initialises the grid
  and the shell, installs the painter on window 2, routes every `input_try_getc()`
  byte to the shell, prints `CRUGOS: sh run '<line>'` per executed line and
  `CRUGOS: sh out <text>` per output line, and prints
  `CRUGOS: wb shell ready (window 2, 37x8 text)`; `wb_selftest()` and `wb_hold()`
  both pump the shell.
- `make test-workbench-shell` boots `wb.enable=1 wb.shell=1 wb.sh=4`, hotplugs a
  `usb-kbd` and types `help`, `ls /`, `ver` and `echo hi` one key at a time;
  `scripts/run-workbench-shell.py` keeps in step with the `sh run` markers,
  requires the version string and `hi` in the output, compares two
  `screendump`s of the shell body (at least 200 changed pixels and `0xE0E0E0`
  text) and requires `CRUGOS: sh selftest ok (4 command(s))` plus exit status 33.
- `tests/host/test_textgrid.c` and `tests/host/test_shell.c` cover the wrap, the
  ring, the cursor cell and the whole command table against a fake `shell_io`.
  `make test-host` reports 2792 checks.
- All 38 gates passed at `v0.25.0`.

## Phase 6D - resizing and keyboard navigation (done)

- `include/crug/intuition.h` + `kernel/gfx/intuition.c`: every window carries a
  12x12 resize grip (`INTUITION_RESIZE_SIZE`) in its inner bottom-right corner
  at `(x + w - INTUITION_BORDER - 12 - 2, y + h - INTUITION_BORDER - 12 - 2)`;
  a press arms a size drag instead of a body press, and every motion recomputes
  the size from the size at press plus the pointer delta.
- clamping: the width and height are floored at `INTUITION_MIN_WIDTH` /
  `INTUITION_MIN_HEIGHT` (120x80), and a motion that would push the right or
  bottom edge past the framebuffer is refused -- the size simply does not
  change -- so a window can never be dragged larger than the screen. A release
  without motion still counts a resize.
- the release prints `CRUGOS: wb window %d resized to %ux%u`, increments
  `intuition_resize_count()`, counts one action and queues
  `INTUITION_EVENT_RESIZE` (type 6); `struct intuition_event` gained `u32 w`
  and `u32 h` for the new size, left at 0 by the five earlier event types.
- `intuition_key_event(u8)` gives the desktop a keyboard of its own: Tab cycles
  the focus through the stacking order (raising the next window, activating it
  and queueing `INTUITION_EVENT_ACTIVATE`), the four arrow bytes move the active
  window by `INTUITION_KEY_STEP` (8) pixels and clamp it to the screen, Enter
  opens the active window's first screen-top menu as a popup with item 0
  highlighted, Down/Up move that highlight (stopping at the ends), Enter selects
  it through the normal menu path and Escape closes the popup without a
  selection.
- `include/crug/input.h` gained `INPUT_KEY_UP` `0x90`, `INPUT_KEY_DOWN`
  `0x91`, `INPUT_KEY_LEFT` `0x92` and `INPUT_KEY_RIGHT` `0x93`; the PS/2
  set-1 decoder maps E0 make codes `0x48`/`0x50`/`0x4B`/`0x4D` to them and
  ignores E0 break codes and E0 modifier releases, while the HID decoder maps
  usage page 0x07 usages `0x52` UpArrow, `0x51` DownArrow, `0x4F` RightArrow
  and `0x50` LeftArrow -- Right `0x4F` comes before Left `0x50`.
- `kernel/gfx/workbench.c` reads `wb.keys=1`: `wb_key_pump()` forwards Tab,
  Enter, Escape and the four arrow bytes to `intuition_key_event()` and prints
  `CRUGOS: wb keys ready (window 2 active)` after the desktop-ready line, while
  `wb.shell=1` keeps the keyboard for the shell. `wb.acts=` now counts resizes
  as well, and `wb_drain_events()` prints
  `CRUGOS: wb action %u resize window %d (%ux%u)`.
- `make test-workbench-keys` boots `wb.enable=1 wb.wait=30 wb.keys=1
  wb.acts=2`, hotplugs a `usb-mouse` and drags window 2's grip from (790,490)
  to (730,430) so it shrinks from 320x200 to 260x140, checks a `screendump` for
  desktop background where the old corner was at (798,498), then hotplugs a
  `usb-kbd` and sends Tab, Right twice, Enter, Down and Enter to focus window 1,
  move it to (96,90) and select `Info` from its `Workbench` menu, requiring
  exit status 33 and `CRUGOS: wb selftest ok (2 action(s))`.
- `tests/host/test_input.c` and `tests/host/test_usb.c` pin the PS/2 arrow
  bytes and the HID arrow usages, and `tests/host/test_intuition.c` gained
  `intuition resize grip` and `intuition keyboard focus / keys / menu popup`
  sections; `make test-host` reports 2862 checks.
- limits: no Shift-Tab (the input layer delivers one byte per key and carries no
  modifier state), no sub-menus, no keyboard text entry outside the 6C shell,
  12x12 grips that are not themeable, body text re-laid out from the top-left
  rather than reflowed, and no send-to-back in the stacking order at that point
  (the phase 6E depth gadget later sends a window back and the phase 6F front
  gadget sends one forward).
- All 39 gates pass at `v0.26.0`.

## Phase 6E - desktop objects and window depth (done)

- `include/crug/intuition.h` + `kernel/gfx/intuition.c`: a table of
  `INTUITION_MAX_ICONS` (8) desktop objects. An object is a
  `INTUITION_ICON_WIDTH` x `INTUITION_ICON_HEIGHT` (64x48) tile with one label
  line `INTUITION_ICON_LABEL_GAP` (4) pixels under it, drawn on the background
  under every window, so a press that lands inside a window wins. Tiles are
  clamped so all 64x48 pixels stay on screen, and `intuition_add_icon()` fails
  with -1 without a framebuffer, a label or a free slot.
- kinds and palette: `INTUITION_ICON_VOLUME` (0), `INTUITION_ICON_DRAWER` (1)
  and `INTUITION_ICON_FILE` (2); `INTUITION_ICON_RGB` `0x00c0c0c0` fill,
  `INTUITION_ICON_INNER_RGB` `0x00081018` panel, `INTUITION_ICON_TEXT_RGB`
  `0x00e0e0e0` label and `INTUITION_ICON_SELECTED_BORDER_RGB` `0x00ffb000`
  for the selected frame and label.
- selection: a left press inside a tile selects the object, counts one action,
  queues `INTUITION_EVENT_ICON` (type 7) with the index and the tile origin and
  prints `CRUGOS: wb icon '<label>' selected`; `intuition_icon_selected()`,
  `intuition_icon_count()`, `intuition_icon_clicks()` and
  `intuition_icon_rect()` report the state, and a press on the empty desktop
  clears the selection.
- depth gadget: a 12x12 `INTUITION_DEPTH_SIZE` box four pixels left of the
  close gadget in every title bar. A press sends that window to the bottom of
  the stacking order, activates the new top window (amber title bar), queues
  `INTUITION_EVENT_DEPTH` (type 8) followed by `INTUITION_EVENT_ACTIVATE`,
  counts one action, increments `intuition_depth_count()` and prints
  `CRUGOS: wb window N sent to back`; the backmost window's box is a no-op.
- painters: `intuition_set_paint(id, fn, ctx)` installs a per-window body
  painter that receives the body rectangle instead of the static body text
  (gadgets are still drawn after it), `intuition_redraw_window(id)` repaints
  one window, and a NULL callback restores the static text.
- `kernel/gfx/workbench.c` reads `wb.desk=1`: one `System:` volume object at
  (16,240), `CRUGOS: wb icon ready ('System:', volume)` after the desktop,
  shell and keys markers, and a double click inside `WB_DOUBLE_CLICK_NS`
  (600000000 ns) opens a `System:` window at (560,60) 360x260 whose painter
  lists the mounted ext4 root, one name per line, at most 10 lines clipped to
  the body width, printing `CRUGOS: wb drawer 'System:' opened (N entries)`
  (an empty or unmounted root prints `no volume`). `wb.desk=1` is a reporting
  mode, so `wb.acts=` is honoured, and the self-test prints
  `CRUGOS: wb desk selftest ok (N drawer(s))` before the usual `wb selftest
  ok`. The Workbench body text is now `phase 6e`.
- `make test-workbench-desk` boots `wb.enable=1 wb.wait=30 wb.desk=1
  wb.acts=3`, clicks the `System:` object, opens its drawer and sends it back,
  checking the six-step pixel sequence (idle frame white and inner panel dark,
  selected frame amber, drawer title bar amber and body dark at the overlap
  point, drawer title bar idle grey and the Shell title bar amber after the
  depth press), exit status 33 and a clean `e2fsck`; `boot-check.py` gained
  the `EXPECT_DESK` positional and its `ok` line reports `desk yes`.
- `tests/host/test_intuition.c` gained desktop-object and depth-gadget
  sections; `make test-host` reports 2957 checks.
- docs: README, workbench, boot, build, versioning, gfx and iso describe the
  objects, the drawer, the painter API, `wb.desk=1` and the gate.
- All 40 gates pass at `v0.27.0`.

## Phase 6F - icon dragging, send-to-front and keyboard icons (done)

- `include/crug/intuition.h` + `kernel/gfx/intuition.c`: the icon table became
  interactive. `INTUITION_ICON_DRAG_MIN` (4) is the first-motion dead zone (a
  wiggle under 4 pixels on both axes does not move the tile), and
  two events were added: `INTUITION_EVENT_ICON_MOVE` (type 9) with the object
  index and its new origin, and `INTUITION_EVENT_FRONT` (type 10);
  `intuition_icon_moves()` and `intuition_front_count()` count drags and
  raises.
- dragging: a left press inside a tile selects it and grabs it with the offset
  from the tile origin to the cursor; pointer motion moves the tile to the
  cursor minus that offset, clamped to `[0, screen width - 64]` x
  `[0, screen height - 48]`, so a tile never leaves the desktop. The release
  queues `ICON_MOVE` with the index and the final origin, increments
  `intuition_icon_moves()` and the action count and prints
  `CRUGOS: wb icon '<label>' dropped at (x, y)`; motion prints
  `CRUGOS: wb icon '<label>' drag to (x, y)`, and the tile stays selected, so
  the amber frame travels with it.
- send-to-front gadget: a 12x12 `INTUITION_FRONT_SIZE` box at
  `close_box_x - 32`, twelve pixels left of the depth box, drawn as an up arrow
  in every title bar. A press raises that window, makes it active, queues
  `INTUITION_EVENT_FRONT`, increments `intuition_front_count()` and the action
  count and prints `CRUGOS: wb window N sent to front`; pressing the gadget of
  the already-frontmost window is a no-op with no event, no count and no
  redraw. A window repaint is now five frames (window, close box, depth box,
  front box, resize grip).
- keyboard icon focus: `intuition_icon_key(u8 key)` drives the table. Tab,
  Right and Down select the next object and Left and Up the previous one, both
  wrapping; Enter selects the first object when nothing is selected and on the
  next press returns 1 so the caller activates it (the `System:` volume opens
  its drawer); Escape clears the selection. A selection change prints
  `CRUGOS: wb icon '<label>' focused` and repaints the idle and selected frames.
- `kernel/gfx/workbench.c` reads `wb.ikeys=<0|1>` before `wb.desk=`; the shell
  window still owns the keyboard while `wb.shell=1`. Icon mode prints
  `CRUGOS: wb icons ready (N object(s), keyboard focus)`, forwards Tab, Enter,
  Escape and the arrows to `intuition_icon_key()`, and an activation counts an
  action and prints `CRUGOS: wb action N icon I (keyboard)` and
  `CRUGOS: wb icon activated (keyboard)`. The self-test accepts icon mode and
  prints `CRUGOS: wb icons selftest ok (N move(s), M front(s))`. The Workbench
  body text became `phase 6f`.
- `make test-workbench-icons` boots `wb.enable=1 wb.wait=30 wb.desk=1
  wb.ikeys=1 wb.acts=5` with both a `usb-mouse` and a `usb-kbd`: Enter focuses
  the `System:` volume, Enter activates it and opens the drawer (5 root
  entries), the pointer drags the tile from (16,240) to (168,376) and a
  `screendump` proves the amber frame at the new origin, the window body at
  (176,384) and the desktop colour at the old spot, then the depth box at
  (410,102) sends window 1 back and the front gadget at (394,102) raises it
  again; the gate requires exit status 33 and a clean `e2fsck`.
- `scripts/boot-check.py` gained `EXPECT_ICONS` at positional argv[23] and its
  `ok` line reports an `icons` field; the phase marker is
  `CRUGOS: phase 6f boot ok`.
- `tests/host/test_intuition.c` gained `test_intuition_icon_drag`,
  `test_intuition_front` and `test_intuition_icon_keys`; the window-draw frame
  assertion is 75 because one repaint paints five frames, and `make test-host`
  reports 3054 checks.
- the kernel file is 230056 B (224 KiB) and the load image 749 KiB against the
  1024 KiB stretch and 2048 KiB hard budgets.
- docs: README, workbench, boot, build, versioning, gfx and iso describe icon
  dragging, the send-to-front gadget, `wb.ikeys=1` and the gate.
- All 41 gates pass at `v0.28.0`.

## Phase 6G - drawer scrolling, refresh and reflow (done)

- `include/crug/workbench.h` + `kernel/gfx/workbench.c`: the `System:` drawer
  body is a viewport over the root listing. `WB_DRAWER_ROW_H` (20) is the row
  pitch, `WB_DRAWER_MAX` (40) the entry cap and `WB_DRAWER_NAME_MAX` (32) the
  name buffer, and `wb_drawer_rows(h)` returns 0 for a body up to 22 px and
  `(h - 22) / 20 + 1` above it. The 360x260 drawer has a 236-pixel body and
  shows 11 rows; resized to 460x360 its 336-pixel body shows 16, so a resize
  reflows the list without touching the painter API.
- scrolling: `wb_drawer_scroll(delta)` moves `g_drawer_top` by one entry and
  clamps it to `[0, count - rows]`, so the list never scrolls past either end.
  A press that does not move returns 0 and counts nothing; a real move
  increments `g_drawer_scrolls` and `g_drawer_ops_seen`, repaints the window
  and prints `CRUGOS: wb drawer scrolled (top N of M)`.
- refresh: `wb_drawer_refresh()` re-reads the listing with `ext4_list()`,
  clamps the top row against the new count, repaints, counts one refresh and
  prints `CRUGOS: wb drawer refreshed (N entries)`, so a file created after the
  drawer opened appears on `r`.
- scrollbar: while the listing is longer than the body, the painter draws a
  4-pixel track at `x + w - 8` from `y + 6`, `h - 12` tall, in `0x00404040`,
  with a thumb of height `max(12, track_h * rows / entries)` in the menu
  background colour whose offset follows the top row.
- `wb.dr=1` gives the open drawer the arrow keys and `r`/`R` (the same pump
  still drives the icon table while the drawer is closed), prints
  `CRUGOS: wb drawer mode (arrow keys scroll, r refreshes)`, adds the
  `wb drawer ready (N entries, R row(s))` marker and makes the self-test wait
  for `wb.acts=` drawer operations, printing
  `CRUGOS: wb drawer selftest ok (N scroll(s), M refresh(es))`.
- `wb.fill=N` (0..24) creates `/f00`..`/f(N-1)` once before the drawer reads
  the root, so the gate has a listing longer than one screen; it prints
  `CRUGOS: wb fill N file(s)`.
- write access: the kernel mounts the ext4 root read-only at boot.
  `kernel/fs/storage.c` gained `storage_remount_rw()` / `storage_remount_ro()`
  (declared in `include/crug/storage.h`), which umount and remount the root
  around a short write window and print `CRUGOS: storage remounted read-write`
  / `CRUGOS: storage remounted read-only`. The drawer fill uses them, and so
  does the shell: `kernel/shell/shell_ops.c` `ops_write` now remounts
  read-write for its create and write and remounts read-only again, fixing a
  `write` command that had been failing silently against the read-only mount,
  and `ops_sync` returns 0 while the volume is not mounted read-write.
- `make test-workbench-drawer` is the 42nd gate: it boots
  `wb.enable=1 wb.wait=30 wb.desk=1 wb.dr=1 wb.fill=24 wb.acts=4`, opens the
  drawer on a double click (29 entries, 11 rows), scrolls it three times with
  Down (more than 200 changed pixels in the body), drags the grip from
  (904,304) to (1004,404) so the window becomes 460x360 and the rows reflow
  (desktop `(16,32,48)` at (830,400) becomes window `(8,16,24)`, with at least
  200 body-text pixels inside the widened body), refreshes with `r` and
  requires exit status 33, a clean `e2fsck` and `EXPECT_DRAWER=yes`.
- `scripts/boot-check.py` gained `EXPECT_DRAWER` and its `ok` line reports a
  `drawer` field; the phase marker is `CRUGOS: phase 6g boot ok`.
- `tests/host/test_intuition.c` gained `test_intuition_paint`, and
  `make test-host` reports 3075 checks.
- docs: README, workbench, boot, build, versioning, gfx and iso describe the
  scrolling drawer, `wb.dr=` / `wb.fill=`, the remount helpers and the gate.
- All 42 gates pass at `v0.29.0`.

## Phase 6H - drawer selection and a draggable scrollbar (done)

- `include/crug/intuition.h`: a window can now own the presses inside its body.
  `INTUITION_BODY_PRESS` (0), `INTUITION_BODY_MOVE` (1) and
  `INTUITION_BODY_RELEASE` (2) are the kinds, and
  `int intuition_set_body_input(int id, void (*fn)(int id, u32 kind, i32 x, i32 y, void *ctx), void *ctx)`
  registers a handler that receives body-relative coordinates; `NULL` clears it
  and an unknown id returns -1.
- precedence: `kernel/gfx/intuition.c` gives the window an internal body window
  on a press inside a body that has a handler, forwards `PRESS`, repaints and
  returns, so the press never starts a title-bar drag, a resize or a gadget
  press; while the button is held every pointer motion is forwarded as `MOVE`;
  the release forwards `RELEASE`, repaints and returns before the icon, resize,
  gadget and drag branches.
- geometry: the new `body_rect()` helper and `win_draw_body()` share one body
  rectangle, `x + BORDER`, `y + BORDER + TITLE_HEIGHT`, `w - 2 * BORDER` by
  `h - 2 * BORDER - TITLE_HEIGHT`, so a handler and the painter agree on the
  body edge.
- `kernel/gfx/workbench.c`: `wb.dsel=1` (parsed before `wb.shell=`) gives the
  drawer body that handler. The drawer opens at (560,60,360x260), a 356x236
  body over 29 entries with 11 visible rows on a 20-pixel pitch and a maximum
  top row of 18; a click at body `y` selects index `top + (y - 6) / 20` and
  paints an amber `INTUITION_MENU_HIGHLIGHT_RGB` (`0x00FFB000`) band with the
  text in the window-background colour, printing
  `CRUGOS: wb drawer selected '<name>' (index N of M)`.
- scrollbar: the 4-pixel `WB_TRACK_W` track sits at body-local
  `x = body width - 8`, `y + 6`, `h - 12`, and the thumb is
  `max(12, track * rows / entries)` tall with its position proportional to the
  first visible row. A press on the track jumps the view and grabs the thumb at
  the press point, dragging follows the pointer, and the release prints
  `CRUGOS: wb drawer scrollbar to (top N of M)`.
- markers: `wb.dsel=1` prints
  `CRUGOS: wb select mode (click a row, drag the scrollbar)` after
  `CRUGOS: wb desktop ready` and makes the self-test wait for `wb.acts=`
  operations, printing
  `CRUGOS: wb select selftest ok (2 select(s), 1 scrollbar drag(s))` or
  `CRUGOS: wb select timeout (N/M)`. The `wb.fill=N` fill from phase 6G is
  what gives the gate a listing long enough to overflow the 11 rows.
- `make test-workbench-select` is the 43rd gate: it boots
  `wb.enable=1 wb.wait=30 wb.desk=1 wb.dr=1 wb.dsel=1 wb.fill=24 wb.acts=3`,
  verifies row 0 is window background `(8,16,24)`, clicks row 2 at (600,136)
  for an amber `(255,176,0)` band at (890,136) and index 2 of 29, drags the
  thumb from (911,100) to (911,280) for top row 18 and a repaint that clears
  row 2's band, clicks the new row 0 at (600,96) to select index 18, waits for
  the self-test line and requires exit status 33, `EXPECT_SELECT=yes` and a
  clean `e2fsck`.
- `scripts/boot-check.py` gained its 20th `EXPECT_` positional,
  `EXPECT_SELECT` (argv[25]), and reports `select yes` in its summary line;
  the phase marker is `CRUGOS: phase 6h boot ok`.
- `tests/host/test_intuition.c` gained `test_intuition_body_input` (the
  body-relative press, move and release coordinates, the release that stops
  forwarding, title-bar and close-gadget presses that never reach the handler,
  the closed window that leaves nothing stale, the `NULL` clear and
  `intuition_set_body_input(99, ...) == -1`), and `make test-host` reports
  3104 checks.
- docs: README, workbench, boot, build, versioning, gfx and iso describe the
  body-input hook, the selectable rows, the draggable scrollbar and the gate.
- All 43 gates pass at `v0.30.0`.

## Phase H1 - hardening pass 1 (done)

- Phase H1 treats the bytes on the boot path as hostile: an inserted disk, a
  crafted partition or filesystem image, a tampered ESP file and a PCIe device
  that lies about its registers. CrugOS is ring-0 only, so this is the
  adversarial surface that exists today; the review in `specs/fix-p1/REPORT.md`
  counted 6 High, 16 Medium and 18 Low, and H1 closes the Highs reachable now.
- `include/crug/harden.h` and `kernel/lib/harden.c` add seven pure validators,
  one per input class: `harden_range_ok()` (a register-supplied buffer against
  the 4 GiB identity map, no wrapping addition), `harden_payload_ok()` (a NULL
  payload with a non-zero length), `harden_extent_header_ok()` (ext4 magic,
  entries, depth and the real 60-byte `i_block` / leaf size),
  `harden_ext4_geometry_ok()` (power-of-two block size and
  `bpg`/`ipg` at most `block_size * 8`), `harden_nvme_doorbell_ok()`
  (a doorbell inside BAR0), `harden_kdf_cost_ok()` (lanes 1..16,
  `m >= 8 * lanes`) and `harden_volume_ok()` (a volume inside its partition).
  `tests/host/test_harden.c` tests every boundary; the host suite reports 3200
  checks.
- `kernel/core/syscall.c`: `SYS_PORT_SEND` and `SYS_PORT_RECV` validate the
  `rdx`/`r10` buffer, and `SYS_PORT_RECV` validates a non-zero `rsi` as an
  8-byte writable range, returning `SYS_EFAULT` (`-14`) instead of writing
  through a bad pointer. `msgport_send()` rejects `data == 0` with
  `len > 0`, so the uninitialised-heap copy is gone. The in-kernel self-test
  prints `CRUGOS: syscall selftest failed (pointer gate)` on failure.
- `kernel/fs/ext4.c`: a crafted `s_blocks_per_group` / `s_inodes_per_group`
  fails mount with `CRUGOS: ext4 mount rejected (geometry)` before the
  superblock checksum is even verified, and a root inline extent whose
  `eh_max` does not fit the inode's 60-byte `i_block` area fails after
  `read_inode()` with `CRUGOS: ext4 mount rejected (root extent)`, so the
  image must forge a valid inode checksum to get there. Either way
  `storage_init()` fails, prints `CRUGOS: no root filesystem` and exits 35.
- `drivers/block/nvme.c`: the doorbell offset from `CAP.DSTRD` is bounded
  against `bar_size`; a lying stride is refused at probe
  (`nvme   : doorbell stride outside BAR0`) instead of writing up to ~192 KiB
  past BAR0.
- `kernel/lib/string.c`: `secure_zero()` wipes through volatile stores so the
  compiler cannot elide the wipe; the crypt header/bounce/scratch/master paths
  and `storage_wipe()` use it. `boot/efi/main.c` also zeroes the loader's
  `cmdline.txt` pool copy, because the kernel's `boot_info` wipe left
  `crypt.pass=` in USABLE memory.
- `kernel/block/crypt.c`: the unlock-time KDF policy (m 1024..65536 KiB, t 2..8,
  lanes 1..16, `m >= 8 * lanes`) sits below the on-disk format ceiling
  (1 GiB / t 64, `mkcrypt` unchanged), so a hostile header cannot force a
  15-20 minute pre-authentication Argon2id. A `volume_sectors` past the parent
  partition is refused too; both refusals wipe the 16 KiB header copy.
- `make test-hardening` is the 44th gate: `scripts/run-hardening.py` patches
  one field per case (geometry-bpg, geometry-ipg, root-extent, kdf-high,
  kdf-low, volume-huge), boots each crafted image with `HARDEN_TIMEOUT_SEC`
  (120 s) and requires the rejection marker, exit 35 and no panic, while the
  control boots to exit 33.
- docs: `docs/hardening.md` plus README, roadmap, versioning, build, boot,
  crypto, storage, nvme, workbench and iso.
- All 44 gates pass at `v0.31.0`.

Phase H2 closed the heap free-list and magic work (report items F7/F8) with
address-keyed page cookies, validated slab headers and a read-only integrity
checker. Phase H3 then replaced the per-vector panic one-liner with a trap policy
and put the four stack-destroying vectors on IST stacks, phase H4 took the
mount path to the host with a seeded image fuzzer, phase U1 crossed into ring 3,
phase U2 gave every ring-3 thread its own PML4, so two programs now run at the
same time without sharing a window, phase U3 put a real shell in ring 3 with
file descriptors and an exec-time `argv` block, and phase U4 let a user program
start another one, wait for its exit code and replace its own image in place.
Phase U5 then added `fork`: `SYS_FORK` copies the caller's user window into a
new address space, so a child owns its pages and neither side sees the other's
writes. Phase U6 made that copy lazy: the parent's writable frames are shared
read-only and copied on the first store from either side, so a fork of 300 pages
costs a page table and a few reference counts, and the page cap rose from 256 to
4096. Phase 7A gave the desktop a bundle format and a registry, so a directory
with a manifest and an ELF became a launchable application from both the
Workbench and the shell, and phase 7B put a capability mask on every thread and
a signature check in front of installation, so the disk a program can reach is
now filtered by what the program was granted. What remains is making those
capabilities hold against a program that starts another one (`SYS_SPAWN` and
`SYS_EXEC` still hand out `CAP_ALL`), a populated environment, weak EFI
entropy, and the AHCI timeout bounce reuse. Those are the prerequisites for a
shell that builds process trees, and several of the remaining Mediums stay
reachable until then.

## Phase H2 - hardened kernel heap (done)

- Phase H2 hardens the kernel heap in `kernel/mm/heap.c` after the H1 pass.
  Before it, a slab page carried no cookie, the free list was trusted blindly,
  large blocks were a singly linked list, and there was no way to ask whether
  the heap was still consistent.
- Every slab page and every large-block header starts with a `u64` cookie keyed
  by the page address: `HEAP_COOKIE_KEY 0x435255474f5348ull` XOR the page XOR
  `SLAB_COOKIE_SALT 0x534c4142ull` for slabs, and XOR
  `LARGE_COOKIE_SALT 0x4c415247ull` for large blocks. A header forged at the
  wrong address no longer matches.
- `struct slab_header` keeps its 64-byte footprint and is validated field by
  field: cookie, class index, object counts, used bitmap, every free-list entry
  (inside the page and unallocated), the listed count against `free_count` and
  a cycle cap of `objects`. `kmalloc` panics with
  `heap: corrupt slab header` or `heap: corrupt slab free list`.
- Large blocks form a doubly linked list (`struct large_header` with `cookie`,
  `size`, `pages`, `next` and `prev`) whose links and fields are checked;
  `large_contains()` turns a `free()` of an interior large pointer into the
  panic `heap: free of interior large pointer`.
- `heap_check()` walks every slab class and the large list and returns one of
  nine `HEAP_CHECK_*` codes (`heap_check_name()` prints `ok`, `slab cookie`,
  ...), cross-checking `g_slab_pages`, `g_large_pages` and `g_large_bytes`
  against the live structures. It masks interrupts around the walk.
- `heap_stress(ops)` churns 55% allocations and 45% frees over up to 64 live
  objects of 1..8000 bytes, tags and verifies every object, checks the heap every
  64 operations and ends with an exact `heap_used_bytes()` accounting check
  (`HEAP_STRESS_MAX` 200000). `heap_guard()` plants a bad free-list link, a
  zeroed slab cookie and a zeroed large-block cookie, proves `heap_check()`
  reports codes 5, 1 and 6, then repairs them.
- `heap_cmdline()` parses `heap.stress=<1..200000>` and `heap.guard=1` right
  after `heap_init()`; `heap_selftest()` prints
  `CRUGOS: heap check ok (6 slab(s), 8 object(s), 1 large block(s))` and
  requires a freed 16-byte object to be reused at the same address.
- `tests/host/test_heap.c` supplies a fake 2048-page 8 MiB `.bss` arena with
  its own bitmap, mirrors the two kernel structs and covers every check code,
  the large-block free path, `heap_stress(2000)`, `heap_guard()` and every
  `heap_check_name()` string; the `pmm_alloc_pages`/`pmm_free_pages` and
  `kzalloc` stubs moved out of `test_argon2.c` and `test_handle.c`.
  `make test-host` reports 3235 checks.
- `make test-heap` is the 45th gate: it boots
  `heap.stress=$(HEAP_STRESS_OPS) heap.guard=1` under `HEAP_RUN_TIMEOUT_SEC`
  (240 s), requires the stress, guard and check markers and a clean `e2fsck`,
  and `make size` reports a 244264 B (238 KiB) file and a 783106 B (764 KiB)
  load image. `HEAP_STRESS_OPS` defaults to 4000 and `EXPECT_HEAP` gates the
  markers.
- docs: `docs/heap.md` plus README, roadmap, versioning, build, boot, hardening,
  workbench and iso.
- All 45 gates pass at `v0.32.0`.

## Phase H3 - trap policy and crash reports (done)

- Phase H3 replaces the per-vector panic one-liner after the H1 and H2 passes.
  Before it, every non-`#BP` exception printed one
  `trap   : vector N (...) error=.. rip=..` line and panicked immediately: no
  saved-frame contract, no kernel-versus-user decision, and only `#DF` on an
  IST stack.
- `include/crug/trap.h` keeps the `trap_frame` layout and adds
  `TRAP_IST_DF 1u`, `TRAP_IST_NMI 2u`, `TRAP_IST_MC 3u`, `TRAP_IST_DB 4u`,
  `enum trap_class { TRAP_HANDLED = 0, TRAP_USER = 1, TRAP_FATAL = 2 }`,
  `struct trap_report_info { u64 vector; u64 error; u32 ist; u64 cr2; int has_cr2; const char *name; }`
  and the `trap_classify()`, `trap_ist_for()`, `trap_format()`,
  `trap_cmdline()` and `trap_test_run()` entry points.
- `kernel/lib/trapfmt.c` is the pure formatter (no `kprintf`, no serial, no
  framebuffer) and writes at most 1024 bytes: a
  `CRUGOS: trap report <name> vector=<dec> error=0x<hex> ist=<dec> from=<kernel|user>`
  head, the saved `rip`/`cs`/`rflags`/`rsp`/`ss` line, a `#PF`-only line
  with `present`/`write`/`user`/`reserved`/`instruction` and `cr2`, and two
  register lines. Hex is lowercase and unpadded, so zero prints as `0`.
- `kernel/arch/x86_64/idt.c` drops `page_fault_panic()`: vectors 32 (timer),
  128 (syscall) and 3 (`#BP`, still handled and counted in `g_int3_seen`) return
  early, and every other vector builds a `trap_report_info`, formats the report,
  `console_write()`s it and classifies it.
- `trap_classify()` keeps `#BP` `TRAP_HANDLED` in both rings and `#OF`
  `TRAP_HANDLED` only for a user frame; the user-killable set {0 divide, 1 debug,
  6 invalid opcode, 11 segment not present, 12 stack-segment, 13 general
  protection, 14 page fault, 16 x87, 17 alignment, 19 SIMD, 21 control
  protection} is `TRAP_USER` when `cs & 3 == 3` and `TRAP_FATAL` from ring 0,
  and everything else (`#NMI`, `#DF`, `#MC`, 5, 7, 9, 10, 15, 20, 22..31 and
  >= 32) is fatal in both rings. `TRAP_USER` currently prints the report and
  panics with `panic("user-mode <name>")`, which is where thread killing will
  go.
- `kernel/arch/x86_64/linker.ld` and `cpu.c` give the four stack-destroying
  vectors their own IST stacks in `.bss` after the boot stack: `__df_stack` is
  16 KiB (was 8 KiB) and `__nmi_stack`/`__mc_stack`/`__db_stack` are 8 KiB each
  (+32 KiB of bss) and loaded into TSS `ist1`..`ist4`;
  `trap_ist_for()` routes vector 8 to IST1, 2 to IST2, 18 to IST3 and 1 to
  IST4, everything else staying on the current stack. `rsp0` still points at
  the boot stack because the kernel never `iretq`s to CPL3 yet.
- `kernel/core/trap_test.c` parses `trap.test=<recipe>` (at most 16 chars;
  de, ud, gp, pf, df, nmi, mc, db) right after `heap_cmdline()` and
  `trap_test_run()` injects exactly one fault as the last action before
  `arch_exit_qemu(0x10)`, so it runs after `CRUGOS: phase h3 boot ok`. An
  unknown value prints `CRUGOS: trap test unknown <value>` and injects
  nothing.
- `make test-trap` is the 46th gate: `scripts/run-trap.py` boots the control
  plus the eight recipes from per-case disk copies under `build/x86_64/trap/`
  with a `TRAP_TIMEOUT_SEC` (60) budget and requires the recipe marker, the
  report vector/IST/`from=kernel` fragments, `CRUGOS PANIC: <name>`,
  `CRUGOS: panic` and exit 35 for each hostile boot, while the control exits 33
  with the phase marker and no panic.
- `tests/host/test_trap.c` covers the classification matrix, the IST routing,
  the page-fault fault line, the no-fault-line path, a user frame and the
  truncation bound on the host; `make test-host` reports 3400 checks and
  `make size` reports a 253144 B (247 KiB) file and an 821986 B (802 KiB) load
  image.
- docs: `docs/traps.md` plus README, roadmap, versioning, build, boot, hardening,
  heap, workbench and iso.
- All 46 gates pass at `v0.33.0`.

## Phase H4 - host mount-path fuzzer (done)

- Phase H4 attacks the storage mount path from the host instead of from QEMU.
  No kernel C file changes: `fuzz_mount.c` links the real ext4, GPT, journal,
  block, crypt-metadata, heap, lib and crypto sources against host stubs, so the
  kernel image, the `BootInfo` v1 ABI and every boot path stay byte-identical.
- `tests/host/fuzz_mount.c` (about 890 lines) supplies `kprintf()` (silent
  unless `--verbose-kernel`), a `panic()` that exits 97 and a `pmm_*` bump
  allocator over a 32 MiB static arena, and presents the image through an
  in-memory block device with strict bounds checks. It is a separate
  `tests/host/build/fuzz-mount` binary because it installs its own stubs;
  `tests/host/Makefile` gained `FUZZ_SOURCES` and a `fuzz` target that reuses
  the unit-test flags.
- It loads the `scripts/mkdisk.sh` images -- a 16 MiB GPT disk whose single
  partition is `crugroot` at LBA 2048, and the encrypted `crypt.img` -- and
  locates the partition with the real `gpt_parse()`. During bring-up the
  harness rejected a pristine image with `fuzz: the image has no GPT partition`
  because it tested for zero; the function returns the partition count, so
  callers test `< 0`. `control_mount()` now mounts the untouched image and
  requires `/hello.txt`, so a harness wiring bug cannot masquerade as a kernel
  finding.
- Six weighted mutation regions: the ext4 superblock (partition offset + 1024
  bytes, 1024 bytes, weight 22), the group descriptor table (14), the inode
  table (16), the root directory block (16), a 64 KiB metadata window at the
  partition start (17) and the whole partition (15). Geometry comes from the
  superblock and is clamped to sane ranges.
- Per case it builds a plan, forks a child on its own copy-on-write image,
  applies one to four mutations, arms `alarm(FUZZ_TIMEOUT_SEC)` and walks
  `gpt_parse()` -> `ext4_mount()` (a quarter of the cases read-write) ->
  `ext4_lookup("/")`/`/hello.txt` -> `ext4_list("/")` ->
  `ext4_read_file("/hello.txt")`/`/selftest/b.txt` -> the write path (create,
  write, truncate, fsync, mkdir, create/write/rename/unlink inside, rmdir,
  rename, unlink, a 32-byte rewrite of `/hello.txt`) -> `ext4_umount()`. The
  exit code encodes the depth (0 rejected, 1 mounted, 2 walked a path, 3 wrote)
  and the parent counts it.
- Eight mutation kinds (one-byte XOR, `0xFF` fill, zero fill,
  `u16`/`u32`/`u64` write, a 16-byte splice, a 16-byte random fill) with values
  weighted towards 0, 1, 2, `0xFFFF`, `0x7FFFFFFF`, `0x80000000`,
  `0xFFFFFFFF`, all ones and `0x1000`; everything derives from `--seed`, so a
  finding reproduces exactly.
- A child that dies on a signal, exits 97 through the panic stub, or exits above
  3 is a finding: `report_case()` prints the plan (`region=... rw=...
  mutation(s)=...` and one line per mutation) and `dump_case()` writes
  `fuzz-ext4-<N>.img` (or the 256-byte `fuzz-crypt-<N>.img`) under
  `--crash-dir`. `--selftest` forks a null dereference and requires a fatal
  signal, so a green gate proves the detector works at all.
- The second leg copies the 256-byte `struct crug_crypt_header` out of
  `crypt.img`, mutates it and calls
  `crypt_meta_validate()`/`crypt_meta_scratch_bytes()` in a forked child: the
  on-disk header parser, not the Argon2id unwrap.
- `make test-fuzz` is the 47th gate: it removes and rebuilds `disk.img` and
  `crypt.img`, builds `make -C tests/host fuzz`, runs the one-case self-test
  and then `FUZZ_CASES` (20000) ext4 cases plus `FUZZ_CRYPT_CASES` (2000)
  header cases with `FUZZ_SEED` (1337) and `FUZZ_TIMEOUT_SEC` (10) in roughly
  23 seconds. It never boots QEMU, so it prints no kernel version string; a
  green run ends with
  `ok: fuzz 20000 ext4 case(s) and 2000 crypt header case(s), 0 crash(es)`.
- With the defaults about 10580 cases mount, 10535 walk a path and 2594 exercise
  the write path; seeds 1337, 7, 424242 and 999983 each ran 22000 cases with no
  crash, as did a 60000-case sweep before the write leg existed. The harness
  cannot see ring-0-only faults, device/DMA paths, dirty-journal replay, the
  KDF, the GPT header or concurrency; those limits are in `docs/fuzzing.md`.
- docs: `docs/fuzzing.md` plus README, roadmap, versioning, build, boot,
  hardening, traps and workbench.
- All 47 gates pass at `v0.34.0`.

## Phase U1 - ring 3 and the first user program (done)

- Phase U1 is the specification's deliberately deferred "protected userspace"
  milestone: ring 3 end to end, from a DPL3 segment pair to a fault that is
  charged to the program instead of the kernel. It adds three freestanding ELF
  programs under `/bin` and one more gate; the kernel image and the `BootInfo`
  v1 handoff are unchanged.
- `include/crug/segment.h`/`kernel/arch/x86_64/cpu.c` grow the GDT from five to
  seven entries: kernel code/data at `0x08`/`0x10`, the 64-bit TSS at `0x18`,
  ring-3 code at `0x2B` (`SEG_USER_CS`) and ring-3 data at `0x33`
  (`SEG_USER_DS`). The TSS keeps the H3 IST1..IST4 stacks, and `rsp0` is now
  per thread: `cpu_set_kernel_stack()` is its only writer and `schedule()`
  installs the incoming thread's kernel stack top (kernel threads use the boot
  stack top), so a trap from CPL3 always lands on a valid kernel stack.
- `include/crug/vm.h` fixes the user window at `USER_BASE`
  `0x0000008000000000` (512 GiB, PML4 slot 1) through `USER_TOP`
  `0x0000010000000000` (1 TiB), above the supervisor-only identity map, so a
  `U/S=1` leaf can only exist in that range. `kernel/mm/vm.c` adds
  `vm_user_map_page`, `vm_user_map_pages` and `vm_user_unmap_pages`, which
  create every intermediate PDPTE/PDE with `VM_USER`, strip `VM_GLOBAL` and take
  W^X from the caller's flags (`VM_NX` on data pages).
- `kernel/exec/elf_exec.c` (with `include/crug/exec.h`) loads `/bin/<name>`:
  `ext4_read_file` into a 512 KiB (`ELF_MAX_BYTES`) kernel buffer, the same
  `boot/efi/elf.c` parser the UEFI loader uses, then per-`PT_LOAD` reservation
  with executable segments mapped read+execute and data segments
  `VM_WRITE|VM_NX`. Contents are copied through `pmm_phys_to_virt`, never through
  the user mapping, so SMAP cannot trip the loader; empty segments are ignored;
  the stack is four pages below `USER_STACK_TOP` with an unmapped page under
  it; `harden_user_segment_ok`/`harden_user_entry_ok` validate the segment,
  entry and initial stack against the window.
- `kernel/mm/uaccess.c` is the only path from the kernel to user memory:
  `uaccess_check` is pure window arithmetic (`harden_user_range_ok`), a
  page-table walk then reports `UACCESS_EFAULT` for an unmapped page instead of
  taking a kernel `#PF`, and `stac`/`clac` bracket only the copy when CPUID
  exposes SMAP. `uaccess_user_read`, `uaccess_user_write` and
  `uaccess_user_str` are the three copy primitives.
- `kernel/core/syscall.c` now serves both rings: `from_user()` tests
  `frame->cs & 3`, CPL0 callers keep the identity-map path so the in-kernel
  selftests are untouched, and CPL3 callers go through the bounce buffers.
  `SYS_WRITE` (16) writes fd 1/2 from at most 4096 bytes in 256-byte chunks,
  `SYS_MAX` is 17, `SYS_EBUSY` (-16) is new, and `SYS_EXIT` from CPL3 ends only
  the calling thread.
- `kernel/arch/x86_64/user.S`/`kernel/core/thread.c`: a `THREAD_FLAG_USER`
  thread carries `user_rip`, `user_rsp` and up to `THREAD_USER_MAP_MAX` (8)
  mapped regions; `thread_create_user()` returns `-EBUSY` for a second live user
  thread, `thread_destroy()` unmaps the recorded pages, and `thread_user_start`
  builds the `iretq` frame (`SS 0x33`, user `RSP`, `RFLAGS 0x202`, `CS 0x2B`,
  entry `RIP`).
- `kernel/arch/x86_64/idt.c`: a fault from CPL3 writes the H3 report and then
  `CRUGOS: user fault <name> at rip=... err=... tid N` plus
  `CRUGOS: user fault killed tid N code -<vector>`, killing only that thread via
  `thread_kill_current()`. User `#DF`/`#NMI`/`#MC` and every CPL0 fault still
  panic; the old `panic("user-mode <name>")` path is gone.
- `user/` holds `hello.c`, `badptr.c` and `fault.c`, the `user.ld` linker script
  (base `0x8000010000`, separate R+X text and R+W data `PT_LOAD`s) and the
  freestanding runtime in `user/lib.h` (inline `int $0x80` wrappers, `nr` in
  `rax`, arguments in `rdi`/`rsi`/`rdx`/`r10`). `make user` builds them,
  `scripts/mkdisk.sh` reads `USER_DIR` from the environment and writes them into
  `/bin`, and `disk` depends on `user`.
- `kernel/core/user_test.c` parses `user.test=1` from the boot command line and
  runs the three programs in sequence: `/bin/hello` exits 0, `/bin/badptr` gets
  `-EFAULT` for a kernel pointer and exits 0, and `/bin/fault` dies with -14
  after storing above the window. `make test-user` is the 48th gate: it rebuilds
  the disk with the programs, boots with the token under `USER_RUN_TIMEOUT_SEC`
  (240 s) and passes boot-check the new final `EXPECT_USER` argument, which
  fails any other gate that prints a user marker.
- `tests/host/test_uaccess.c` and `tests/host/test_exec.c` cover the window,
  segment and entry validators, `uaccess_check` and the ELF segment policy;
  `make test-host` reports 3470 checks, the kernel is 262760 B (256 KiB) on
  disk and 831006 B (811 KiB) loaded.
- docs: `docs/userspace.md` plus README, roadmap, versioning, build, boot, traps,
  hardening, iso, fuzzing and workbench.
- All 48 gates pass at `v0.35.0`.

## Phase U2 - per-process address space (done)

- Phase U2 replaces U1's single shared address space with one PML4 per ring-3
  thread: a `vm_space` owns the window, slot 0 is inherited from the kernel, and
  the kernel switches CR3 together with the thread. The phase adds the isolation
  program and one gate; the kernel image and the `BootInfo` v1 handoff are
  unchanged.
- `include/crug/vm.h` declares `struct vm_space { u64 *pml4; u64 phys; u32
  user_pages; u32 pad; }` and the API: `vm_space_set_kernel_root`,
  `vm_space_create`, `vm_space_destroy`, `vm_space_map_user_pages`,
  `vm_space_unmap_user_pages`, `vm_space_translate`, `vm_space_activate` and
  `vm_kernel_root_phys`.
- `kernel/mm/vm_space.c` is a separate, host-testable unit: it needs only
  `pmm_alloc_page`/`pmm_free_page` and the identity translations, never CR3 or
  the LAPIC. The CR3 write lives in `kernel/mm/vm.c`, where
  `vm_space_activate` is the only writer and skips the write when the root did
  not change.
- Every space copies slot 0 from the kernel root, so the shared supervisor
  identity map (kernel image, stacks, MMIO) is present in each; slot 1 is the
  512 GiB to 1 TiB window. Mapping upgrades every intermediate entry to
  `PRESENT|WRITE|USER`, clears `VM_GLOBAL` on the leaf, forces `VM_USER`, takes
  W^X from the caller's flags (`VM_WRITE|VM_NX` on data pages, neither on text)
  and validates page alignment, the window and an absent leaf before touching
  the tables.
- A failed mapping rolls back exactly the pages that call had mapped;
  `vm_space_destroy` walks slot 1 and frees every leaf frame, every PT, every
  PD, the PDPT and the root page.
- `kernel/mm/vm.c` keeps the active root in a static `g_root` that `pte_for`
  and `vm_translate` follow, so uaccess and every kernel-side translation see
  the running space.
- `struct thread` carries `vm_space *space` and loses the U1
  `thread_user_map[8]` array; `schedule()` installs the kernel stack top and
  then calls `vm_space_activate(next->space)`, `thread_destroy()` destroys the
  space, and `thread_create_user(name, entry, user_rsp, vm_space *space,
  sched_class)` no longer returns `-EBUSY`: the U1 one-user-thread rule is gone.
- `kernel/exec/elf_exec.c` owns one space per exec: segment reservation and the
  four-page stack with its unmapped guard go into that space, and the space is
  handed to the new thread. Landing U2 also changed `vm_init` to publish the
  root immediately after allocating it, because the section mapper walks the
  root and the old order page-faulted before the IDT existed.
- `user/iso.c` and `user/iso.ld` pin a writable page at
  `0x0000008000030000`; each instance writes `0x15500000 | (tid << 8)`, yields
  300 times and re-checks its own word. `kernel/core/user_test.c` under
  `user.test=2` starts two instances, prints
  `CRUGOS: user isolation frames 0x28000 vs 0x38000 at 0x8000030000` to prove
  the same virtual address maps to different frames, then joins both and counts
  mismatches.
- `make test-user2` is the 49th gate: it rebuilds the disk with all four
  programs, boots with `user.test=2` under `USER2_RUN_TIMEOUT_SEC` (240 s) and
  passes boot-check the new final `EXPECT_USERS` argument (the argument guard is
  now `tuple(range(3, 30))`), so the boot line reports `user yes isolated yes`.
  `make test-user` (gate 48) still runs the three U1 programs under
  `user.test=1`.
- `tests/host/test_vm_space.c` covers slot-0 inheritance, the window validation
  table, raw leaf flags, distinct frames for the same VA, partial rollback, full
  reclaim and an allocation-failure path that asks for the whole 512 GiB window;
  `host_pmm_free_pages_count()` in `test_heap.c` makes a leak visible.
  `make test-host` reports 3554 checks, the kernel is 263056 B (256 KiB) on disk
  and 832574 B (813 KiB) loaded.
- docs: `docs/userspace.md` (reworked for U2) plus README, roadmap, versioning,
  build, boot, traps, hardening and iso.
- All 49 gates pass at `v0.36.0`.

## Phase U3 - a ring-3 shell with file syscalls (done)

- Phase U3 gives ring 3 the filesystem. Four new syscalls reach the mounted
  ext4 root, the kernel passes `argc`/`argv` on the initial stack, and `/bin/sh`
  reads a script from the disk image and writes a file back. The phase adds the
  file layer and one gate; the kernel image, the `BootInfo` v1 handoff and every
  boot path are unchanged.
- `include/crug/syscall.h` adds `SYS_OPEN` (17), `SYS_READ` (18), `SYS_CLOSE`
  (19), `SYS_LISTDIR` (20) and `SYS_MAX` 21; `SYS_WRITE` (16) writes to an open
  file when `fd >= 3`; `SYS_ENOENT` (-2) is the new missing-path error and any
  number at or above `SYS_MAX` returns `SYS_ENOSYS`. Open flags are
  `SYS_O_RDONLY` 0, `SYS_O_WRONLY` 1, `SYS_O_CREAT` 2 and `SYS_O_TRUNC` 4.
- `include/crug/ufile.h`/`kernel/fs/ufile.c` hold a per-thread descriptor table:
  `UFILE_FD_MAX` 8 slots starting at fd 3, `UFILE_MAX_BYTES` 64 KiB for the read
  snapshot and the write buffer, `UFILE_PATH_MAX` 64, and `ufile_close_all()`
  from `thread_destroy()` so a dying thread never leaks a descriptor. Fds 0/1/2
  are permanent (stdin and the console).
- A read-only open snapshots the whole file into the heap; a writable open
  buffers and flushes on close (create when `SYS_O_CREAT` made the file, write
  the buffer, otherwise truncate to zero, then fsync). The flush brackets the
  filesystem with `begin_write`/`end_write`, which on ext4 are
  `storage_remount_rw()`/`storage_remount_ro()` because the root boots
  read-only - hence `CRUGOS: storage remounted read-write`/`read-only` around the
  write.
- `include/crug/exec.h` and `kernel/exec/argv.c` add the argument block: a
  4 KiB area at the top of the four stack pages, 16-byte aligned, holding the
  strings (`EXEC_ARGS_STR_MAX` 2048 bytes), `argv[]` (`EXEC_ARGS_MAX` 16),
  `envp[]` and `argc`. `elf_exec_run_args(path, argc, argv, out)` starts the
  thread with `rsp` at `argc`; `user/start.S` loads `rdi`/`rsi`/`rdx` and
  `elf_exec_run()` stays the `argc == 0` wrapper.
- `user/sh.c` and `user/sh.ld` add `/bin/sh` at CPL3 with text at
  `0x8000010000` and data at `0x8000020000`. It supports `ver`, `echo TEXT`,
  `cat FILE`, `ls PATH`, `write FILE TEXT` and `exit N`, reads `argv[1]` or
  `/etc/sh.rc`, and uses static buffers because there is no user heap yet.
  `scripts/mkdisk.sh` writes the shipped `/etc/sh.rc` alongside the programs.
- `kernel/core/user_test.c` understands `user.test=3`: the U1 trio, the U2
  isolation pair and then the shell, expecting exit 7. The shipped script reads
  `/hello.txt` (16 bytes) and writes `/shout.txt` (22 bytes with the newline),
  which exercises read and write on the same file layer.
- `make test-user3` is the 50th gate: it rebuilds the disk with all five
  programs, boots with `user.test=3` under `SHELL_RUN_TIMEOUT_SEC` (240 s), and
  passes boot-check the new final `EXPECT_USHELL` argument (`argv[29]`, guard
  `tuple(range(3, 31))`), so the line reports
  `user yes isolated yes ushell yes`. It then runs check-disk with
  `--expect-shell-write`, which asserts `/shout.txt` holds
  `hello-from-user-shell\n` after a clean umount and `e2fsck`.
- `make test-host` reports 3663 checks; the kernel is 267976 B (261 KiB) on disk
  and 842046 B (822 KiB) loaded (`.text` 190790 B, `.rodata` 28608 B, `.bss`
  622592 B), inside the 1 MiB stretch and 2 MiB hard budgets.
- docs: `docs/userspace.md` (reworked for U3) plus README, roadmap, versioning,
  build, boot and hardening.
- All 50 gates pass at `v0.37.0`.

## Phase U4 - spawn, exec and wait in ring 3 (done)

- Phase U4 gives ring 3 a process model. A user program can start another
  program image, block until it exits and read its exit code, or replace its own
  image without changing threads - all of it driven from CPL3 instead of the
  kernel. The phase adds three syscalls, two programs and one gate; the kernel
  image layout, the `BootInfo` v1 handoff and every boot path are unchanged.
- `include/crug/syscall.h` adds `SYS_SPAWN` (21), `SYS_WAIT` (22) and
  `SYS_EXEC` (23), so `SYS_MAX` becomes 24, plus `SYS_ESRCH` (-3) for a tid
  that is not the caller's child.
- `kernel/exec/elf_exec.c` gains
  `elf_spawn_user(path, argc, argv, parent_id, out)`, which is the old exec path
  with an argument block and a parent id: the new `parent_id` field of
  `struct thread` records the caller. `elf_exec_run()` stays the `argc == 0`
  wrapper and `elf_exec_run_args()` passes parent id 0, which is how
  kernel-launched programs (the U1 suite, the shell) differ from real children.
- `SYS_WAIT(tid)` blocks until that child exits and returns its code. The new
  `thread_find(id)` in `kernel/core/thread.c` walks an all-threads list kept by
  the scheduler that still holds zombies, so a wait arriving after the exit finds
  the record and its code; ids are monotonic, so a stale tid never matches a
  recycled thread. The kernel accepts the wait only when the target's
  `parent_id` is the caller's tid and the target is not the caller, and reaps it
  through the existing `thread_join_code()`.
- `SYS_EXEC(path, argc, argv)` replaces the calling image in place:
  `elf_exec_in_place()` loads the ELF into a fresh `vm_space`, points the
  thread's `space`/`user_rip`/`user_rsp` at it, and activates the new space
  before destroying the old one because CR3 still points at the old tables. The
  syscall layer then rewrites the saved trap frame (`rip`, `rsp`, `rax`), so the
  `iretq` lands in the new image. The thread keeps its tid and the log proves it
  with `CRUGOS: user exec /bin/child (tid 14)`; a CPL0 caller gets
  `SYS_EINVAL`.
- `syscall_argv()` in `kernel/core/syscall.c` is the first argument path that
  starts in user memory: CPL3 callers go through `uaccess_check` and
  `uaccess_user_read`/`uaccess_user_str` into `g_argv_buf`, CPL0 callers keep
  the identity-map path through `harden_range_ok` and `memcpy`, and both finish in
  `exec_args_ok(argc, str_bytes)`, a pure predicate in `kernel/exec/argv.c`
  that enforces `EXEC_ARGS_MAX` 16 and `EXEC_ARGS_STR_MAX` 2048 bytes before an
  address space is allocated.
- `user/spawn.c` and `user/child.c` become `/bin/spawn` and `/bin/child`.
  spawn starts the child with `from-spawn`, prints its tid, waits for exit 5,
  then execs the child with `from-exec` and exits 5; the child prints its tid,
  `argc 2` and the second argument, so the log shows the same program on two
  different tids and then the same tid on a replaced image.
- `kernel/core/user_test.c` understands `user.test=4`: the U1 trio, the U2
  isolation pair, the shell and then `user_spawn()`, which joins `/bin/spawn`
  and requires exit 5. `scripts/mkdisk.sh` now installs seven programs.
- `make test-user4` is the 51st gate: it rebuilds the disk with all seven
  programs, boots with `user.test=4` under `SPAWN_RUN_TIMEOUT_SEC` (240 s), and
  passes boot-check the new final `EXPECT_SPAWN` argument (`argv[30]`, guard
  `tuple(range(3, 32))`), so the line reports
  `user yes isolated yes ushell yes spawn yes`. It then runs the plain
  check-disk pass - no `--expect-shell-write`, which stays with gate 50.
- `make test-host` reports 3670 checks (the new `exec_args_ok` cases live in
  `tests/host/test_argv.c`); the kernel is 276408 B (269 KiB) on disk and
  844414 B (824 KiB) loaded (`.text` 192902 B, `.rodata` 28864 B, `.bss`
  622592 B), inside the 1 MiB stretch and 2 MiB hard budgets.
- docs: `docs/userspace.md` (spawn, wait, in-place exec and the argv boundary)
  plus README, roadmap, versioning, build, boot, traps, hardening, iso, workbench,
  fuzzing and heap.
- All 51 gates pass at `v0.38.0`.

## Phase U5 - fork (done)

- Phase U5 adds the second half of the process model: `SYS_FORK` copies the
  caller's whole user window into a new address space and resumes the child
  immediately after the `int 0x80`. The child owns its pages, so the parent's
  later writes are invisible to it and its writes are invisible to the parent.
  The kernel image layout, the `BootInfo` v1 handoff and every boot path are
  unchanged.
- `include/crug/syscall.h` adds `SYS_FORK` (24), so `SYS_MAX` becomes 25. The
  call returns the child's tid in the parent and 0 in the child.
- `include/crug/vm.h` and `kernel/mm/vm_space.c` add `vm_space_clone()`, capped
  at `VM_SPACE_FORK_MAX_PAGES` (256 pages). The child PML4 inherits slot 0, the
  shared supervisor identity map, and every present slot-1 leaf is copied into a
  fresh frame with the parent's `PRESENT`/`WRITE`/`NX` bits. Any failure
  destroys the half-built space, and a parent above the cap is refused rather
  than partly cloned.
- `kernel/core/thread.c` gains `thread_create_forked()`: it keeps a copy of
  the parent's trap frame in `THREAD_FLAG_FORKED`/`fork_frame` with `rax = 0`,
  so the child's first user instruction is the one after the `int 0x80`, and the
  thread carries the child's own kernel stack and the cloned `vm_space`.
- `kernel/arch/x86_64/user.S` gains `thread_fork_enter` (`cli`,
  `thread_fork_resume`, `movq %rax, %rsp`, `jmp isr_return`), and
  `kernel/arch/x86_64/isr.S` exports the `isr_return` label just before its
  `popq %r15`. `thread_fork_resume()` parks the saved frame
  `THREAD_FORK_FRAME_GAP` (1024) bytes below the child's kernel stack top,
  restores `%ds`/`%es` to `SEG_USER_DS`, and installs the child's stack through
  `cpu_set_kernel_stack()`; the trampoline then returns to CPL3 through the normal
  interrupt epilogue.
- `thread_create` calls `ufile_table_init`, so a forked child starts with an
  empty descriptor table. An inherited descriptor cannot be shared safely, and
  an empty table guarantees a buffered file write is never flushed twice.
- `user/fork.c` is the ring-3 probe: the child writes `0x2222` into a volatile
  word and exits 3; the parent waits for the tid, requires exit 3 and then must
  still read its own `0x1111`, which proves the pages were copied instead of
  aliased.
- `kernel/core/user_test.c` understands `user.test=5`: the U1 trio, the U2
  isolation pair, the U3 shell, the U4 spawn/exec probe and then `user_fork()`,
  which joins `/bin/fork` and requires exit 3. `scripts/mkdisk.sh` now installs
  eight programs.
- `make test-user5` is the 52nd gate: it rebuilds the disk with all eight
  programs, boots with `user.test=5` under `FORK_RUN_TIMEOUT_SEC` (240 s), and
  passes boot-check the new final `EXPECT_FORK` argument (`argv[31]`, guard
  `tuple(range(3, 33))`) along with the four earlier `EXPECT_*` user flags, so
  the line reports `user yes isolated yes ushell yes spawn yes fork yes`. It then
  runs the plain check-disk pass.
- `make test-host` reports 3723 checks; the kernel is 276616 B (270 KiB) on disk
  and 846206 B (826 KiB) loaded (`.text` 194310 B, `.rodata` 29248 B, `.bss`
  622592 B), inside the 1 MiB stretch and 2 MiB hard budgets.
- docs: `docs/userspace.md` (fork, the clone cap, the trampoline and the empty
  descriptor table) plus README, roadmap, versioning, build, boot, traps,
  hardening, iso, workbench, fuzzing and heap.
- All 52 gates pass at `v0.39.0`.

## Phase U6 - copy-on-write fork (done)

- Phase U6 makes `fork` cheap: instead of duplicating every user page, the child
  shares the parent's frames as read-only mappings and either side takes a
  private copy on its first store. The page cap rises from 256 to 4096, and a
  fork of a 300-page program now costs a page table plus a few reference counts.
  The kernel image layout, the `BootInfo` v1 handoff and every boot path are
  unchanged.
- `include/crug/cow.h` and `kernel/mm/cow.c` keep one reference count per shared
  physical frame in a flat open-addressing table: `COW_TABLE_SLOTS` 4096 slots,
  index `(pa >> 12) * 2654435761` folded with `x ^= x >> 16`, forward probing and
  tombstones. `cow_share()` records one more owner (the first share sets two) and
  returns -1 when the table has no free slot or tombstone; `cow_refs()` reports
  the owners or 0 for an untracked frame; `cow_release()` returns 1 when the
  caller is now the last owner and must free the frame, 0 when another address
  space still maps it. The module needs only `<crug/types.h>`, so the host tests
  link it directly.
- `include/crug/vm.h` adds the software PTE bit `VM_COW` (`1ull << 9`, part of
  `VM_PTE_FLAG_MASK`), raises `VM_SPACE_FORK_MAX_PAGES` to 4096, makes
  `vm_space_clone()` non-const and adds `vm_space_cow_fault()` (0 = nothing to
  resolve, 1 = frame copied, 2 = sole-owner repair, -1 = allocation failure) plus
  `vm_active_space()`.
- `kernel/mm/vm_space.c` shares instead of copying: every present slot-1 leaf
  calls `cow_share()`, the child maps the parent's frame read-only through
  `space_map_shared()` with the parent's `PRESENT`/`USER`/`NX`/`PWT`/`PCD` bits and
  `VM_COW` when the leaf is writable or already `VM_COW`, and a writable parent
  leaf loses `VM_WRITE` and gains `VM_COW`. A read-only text leaf is shared as a
  plain read-only mapping: it never takes `VM_COW` and never faults, but it is
  still entered in the refcount table, so the frame is freed only when the last
  space that maps it is destroyed. A full table (-1) or a mapping failure
  destroys the half-built child, so the fork fails with `ENOMEM` rather than
  falling back to an eager copy; a parent above the cap is refused outright.
- `vm_space_cow_fault()` repairs a leaf in place when `cow_refs()` is at most one
  (the sole-owner case, after the child exits) and otherwise allocates a fresh
  frame, `memcpy`s one page, maps it writable without `VM_COW` and releases the
  old frame, freeing it when that release was the last reference.
  `vm_space_unmap_pages()` and `vm_space_destroy()` free a frame only when
  `cow_release()` says it is the last reference.
- `kernel/mm/vm.c` tracks the active space beside the active page table, and
  `kernel/mm/uaccess.c`'s `uaccess_pages_writable()` resolves a `VM_COW` leaf
  before a kernel copy-out, so a syscall write through a user pointer never
  stores into a still-shared frame. `uaccess_user_read()` keeps the plain mapped
  check because a read cannot break a share.
- `kernel/arch/x86_64/idt.c` resolves the user page-fault path before the ordinary
  trap report: for vector 14 with the error code's U/S and W/R bits set and
  `trap_classify()` = `TRAP_USER`, a positive `vm_space_cow_fault()` flushes the
  page and returns to CPL3, printing `CRUGOS: user cow fault tid N va=0x...
  (copy)` or `(sole owner)`. A non-resolvable fault falls through to the normal
  report and the kill-only-that-thread policy.
- `SYS_FORK` (24, unchanged ABI) calls `vm_flush_all()` after the child exists,
  because the parent's TLB may still cache the pre-fork writable leaf, and prints
  `CRUGOS: user fork tid 16 -> 17 (7 page(s) shared, COW)`.
- `user/fork.c` holds two data pages (`0x1111` and `0x3333`): the child writes
  `0x2222`/`0x5555` and exits 3, the parent reaps it, must still read `0x1111` and
  `0x3333`, then stores `0x4444` into the first page - the sole-owner path - and
  prints `CRUGOS: user fork parent write 17476`. `user/forkbig.c` maps 300 pages
  of `.bss`, plants an `0xA5` sentinel, forks, and the child writes `0x5A` to the
  first and last byte and exits 7; the parent verifies its own copy and prints
  `CRUGOS: user forkbig ok (child exit 7, 300 page(s) isolated)`.
  `scripts/mkdisk.sh` now installs nine programs, and `user.test=6` adds
  `user_forkbig()` on top of the two-page probe.
- `make test-user6` is the 53rd gate: it rebuilds the disk with all nine programs,
  boots with `user.test=6` under `FORKBIG_RUN_TIMEOUT_SEC` (300 s), and passes
  boot-check `EXPECT_USER`, `EXPECT_USERS`, `EXPECT_USHELL`, `EXPECT_SPAWN`,
  `EXPECT_FORK` and the new `EXPECT_FORKBIG` (`argv[32]`, guard
  `if len(sys.argv) < 3 or len(sys.argv) > 33:`), so the line reports
  `user yes isolated yes ushell yes spawn yes fork yes forkbig yes`. It then runs
  the plain check-disk pass. `make test-user5` stays green with
  `forkbig no`.
- `make test-host` reports 7893 checks (`test_vm_space_clone` was rewritten for
  copy-on-write and `test_vm_space_cow` covers the table's share chains,
  tombstones, unaligned fold, full-table refusal and reset); the kernel is
  277040 B (270 KiB) on disk and 913790 B (892 KiB) loaded (`.text` 196038 B,
  `.rodata` 29568 B, `.data` 56 B, `.bss` 688128 B), inside the 1 MiB stretch
  and 2 MiB hard budgets.
- docs: `docs/userspace.md` (the `VM_COW` bit, the reference table, the clone, the
  fault taxonomy and the probes) plus README, roadmap, versioning, build, boot,
  traps, hardening, iso, workbench, fuzzing and heap.
- All 53 gates pass at `v0.40.0`.

## Phase 6I - screens (done)

- `include/crug/intuition.h` gains `INTUITION_MAX_SCREENS` (3) and the screen
  calls: `intuition_add_screen(name, bg_rgb)` returns the new index (or -1 with
  no framebuffer, a NULL name or three screens already present),
  `intuition_set_screen(index)` picks the target for later windows and icons,
  `intuition_screen_switch(index)` brings a screen to the front,
  `intuition_move_window_screen(id, index)` re-parents one window, and
  `intuition_screen_front()`, `intuition_window_screen()`,
  `intuition_screen_count()`, `intuition_screen_switches()` and
  `intuition_screen_moves()` read the state back. Screens are never destroyed
  and only the front one is painted.
- A switch drops any pending menu, drag, resize, gadget, body or icon
  interaction, activates the top window of the new screen, counts one switch,
  queues `INTUITION_EVENT_SCREEN` with window 0 and the new index in `item`,
  prints `CRUGOS: wb screen <n> front (<k> window(s))` and repaints. Switching
  to the screen already in front is a no-op. A move re-parents the window, drops
  the interactions it owned, hands activation to the next window of the front
  screen, queues `INTUITION_EVENT_SCREEN` with the window id and the target
  screen, prints `CRUGOS: wb window <id> moved to screen <n>` and repaints.
- A screen strip at the right end of the menu bar shows one box per screen with
  the front one highlighted; a box is `strlen(name) * 8 + 12` pixels wide, the
  strip starts at `screen_width - total_width - 6`, and presses below `y=18`
  hit it. A press on a box that is not in front switches screens and counts one
  action; a press on the front screen's own box does nothing.
- The palette grows a second desktop background, `0x00203010` (RGB 32,48,16)
  for the Tools screen, beside the screen 0 desktop `0x00102030` (16,32,48),
  the window body `0x00081018` (8,16,24) and the active title bar
  `0x00FFB000` (255,176,0).
- `wb.screens=1` (default 0) makes `kernel/gfx/workbench.c` add a `Tools`
  screen with a 340x200 window at (160,140), the `Tools` {Move, Close} and
  `Workbench` {About, Info, Quit} menus and a `Send to Workbench` gadget at
  body-relative (12,60,160,22) that moves the window to screen 0; it prints
  `CRUGOS: wb screens ready (2 screen(s), front 0)` and ends with
  `CRUGOS: wb screens selftest ok (2 switch(es), 1 move(s))`.
- `make test-workbench-screens` is the 54th gate:
  `scripts/run-workbench-screens.py` boots with
  `wb.enable=1 wb.wait=30 wb.screens=1 wb.acts=4`, clicks the Tools box at
  (992,9), checks the Tools background (32,48,16) and the vanished Workbench
  window, presses the gadget at (254,233), clicks the Workbench box at (924,9)
  and checks the amber title bar (255,176,0) over the body (8,16,24).
  boot-check gains the `--expect-screen=yes` flag (a flag, not a positional
  argument, so the argv order and the argc range are unchanged), and the run
  then executes the plain check-disk pass.
- `tests/host/test_intuition.c` gains `test_intuition_screens`: the
  three-screen limit, the target screen, per-screen window lookup, only the front
  screen painted (a body painter on a covered window is not called and a press
  aimed at it hits nothing), a move queueing one `SCREEN` event and re-handing
  activation, out-of-range window and screen indexes refused, a switch to the
  front screen being a no-op that still sets the target, and a pending drag
  dropped by a switch. `make test-host` reports 7961 checks.
- docs: `docs/workbench.md` (the API, the strip geometry and colours, the
  `SCREEN` event, the Tools screen, the gate and the limits) plus README,
  roadmap, versioning, boot, build and gfx.
- All 54 gates pass at `v0.41.0`.

## Phase 6J - commodities exchange (done)

- `include/crug/commodity.h` and `kernel/gfx/commodities.c` add a pure,
  framebuffer-free registry that the host test links directly. It fixes
  `COMMODITY_MAX` 4, `COMMODITY_NAME_MAX` 16, `COMMODITY_HEADER_H` 24,
  `COMMODITY_ROW_TOP` 28, `COMMODITY_ROW_H` 26 and `COMMODITY_ROW_X` 6, and
  `struct commodity` holds `name`, `hotkey`, `enabled`, `fires` and
  `blocked`.
- The API is `commodity_reset()`, `commodity_register(name, hotkey)` (the new
  index, or -1 for a NULL, empty or 16-byte name, a hotkey outside
  `INPUT_KEY_F1`..`F12`, a duplicate name or a duplicate hotkey),
  `commodity_count()`, `commodity_at()`, `commodity_index_of_hotkey()`,
  `commodity_toggle(index)` (the new state or -1),
  `commodity_set_enabled(index, on)`, `commodity_hotkey(key, &fired)` (the
  owning index or -1, `*fired` 1 when it ran and 0 when blocked),
  `commodity_fires()`/`commodity_blocked()`, `commodity_enabled_count()`,
  `commodity_key_name(key)` (`"F1"`..`"F12"`) and
  `commodity_row_at(body_relative_y)`. A registered commodity starts enabled
  with zero counters.
- `include/crug/input.h` puts `INPUT_KEY_F1` `0x94u`..`INPUT_KEY_F12`
  `0x9Fu` above the arrows. PS/2 set-1 make codes `0x3B`..`0x44` map to
  F1..F10, `0x57` to F11 and `0x58` to F12; break codes yield nothing and
  Shift/Ctrl do not change the byte. USB HID boot usages `0x3A`..`0x45` map
  to F1..F12 (`0x46`, F13, is ignored).
- `kernel/gfx/workbench.c` registers four built-ins: `Volume up` F2,
  `Volume down` F3, `Mute` F4 and `Clock` F6.
- The `Exchange` manager sits at (560,120), size 344x196, and is opened last so
  it starts on top. Rows are 26 pixels high and start 28 pixels below the body
  top; a row paints the name at `x+6`, the key name at `x+w-96` and
  `[on]`/`[off]` at `x+w-44` in amber when on. A row click toggles the
  commodity and prints `CRUGOS: wb action N commodity NAME on|off`.
- Dispatch is refused for a disabled commodity: the key bumps `blocked` and
  prints `CRUGOS: wb commodity NAME blocked (hotkey off)` without running the
  action. An enabled commodity bumps `fires` and runs, printing
  `CRUGOS: wb action N commodity NAME fired (...)`. `Volume up`/`Volume down`
  move the audio mixer by `WB_CX_VOLUME_STEP` 600 within
  `AUDIO_VOLUME_MAX` 0..`AUDIO_VOLUME_MIN` -7680, `Mute` toggles the mixer
  mute (`fired (muted)`/`fired (unmuted)`) and `Clock` prints the monotonic
  uptime (`fired (uptime N s)`). Keys no commodity owns stay with the desktop.
- `wb.cx=<0|1>` (only 0 disables, and it needs `wb.enable=1`) turns the
  exchange on and prints `CRUGOS: wb cx ready (4 commodity(ies))`; the
  self-test waits for two row toggles and two fires and ends with
  `CRUGOS: wb cx selftest ok (2 toggle(s), 2 fire(s))`. Hotkeys are consumed
  only while the Workbench runs with the exchange enabled and no other text
  consumer owns the keyboard.
- `make test-workbench-cx` is the 55th gate:
  `scripts/run-workbench-cx.py` boots with
  `wb.enable=1 wb.wait=30 wb.cx=1 wb.acts=4`, hot-plugs a `usb-mouse` and a
  `usb-kbd` over QMP, clicks the Mute row at screen (600,235), presses F4 while
  the commodity is off (blocked) and again when it is on (fired), presses F6 for
  the Clock and takes a `screendump` whose Exchange body (700,250) must be
  (8,16,24) and whose active title bar (600,130) must be (255,176,0).
  `scripts/boot-check.py --expect-cx=yes` (a flag, not an extra positional, so
  the argv layout is unchanged) then checks the markers and the run ends with a
  clean `e2fsck`.
- `tests/host/test_commodity.c` covers the registry (registration limits,
  duplicate name and hotkey, the toggle, the blocked and fired counters, the row
  hit test and the key names), and `tests/host/test_input.c` /
  `tests/host/test_usb.c` cover both function-key mappings.
  `make test-host` reports 8102 checks.
- Limits: four commodities at most in a fixed table, no persistence across
  boots, hotkeys global to the desktop rather than per window, the exchange
  exists only while the Workbench runs, the actions are fixed built-ins with no
  plugin loading, the manager window is not resizable, and the exchange is off
  unless `wb.cx=1`.
- docs: `docs/workbench.md` (the registry and API, the built-in table, the
  manager geometry, the dispatch rule, the function-key plumbing, the markers,
  the gate and the limits) plus README, roadmap, versioning, boot, build, input
  and gfx.
- All 55 gates pass at `v0.42.0`.

## Phase 7A - .app bundles and tool types (done)

- `kernel/app/app.c` adds the application registry: a static `struct app`
  table of at most `APP_MAX` (8) entries and no heap, filled once by
  `app_init()` in `kernel/core/main.c` after `storage_init()` and before
  `sched_init()`.
- A bundle is a directory, `/Applications/<Name>.app/manifest.toml` plus a
  user ELF at `bin/x86_64/<program>`; nothing else is required, and the program
  is loaded by the normal exec loader.
- The manifest is a bounded TOML subset with a hard cap of
  `APP_MANIFEST_MAX` (2048) bytes: `[app]` `id`, `name` and `version` are
  required (with `min_os` optional), `[exec]` `prog` is a relative path,
  `[tooltypes]` holds up to eight `KEY = "value"` lines and `[icon]` carries
  an `rgb = "RRGGBB"` colour. `#` starts a comment, blank lines are allowed,
  and values must be printable ASCII in double quotes.
- Every refusal has a stable reason string and is printed once at boot as
  `CRUGOS: app rejected <dir> (<reason>)` (`bad-entry`, `manifest-too-large`,
  `no-id`, `bad-id`, `dup-key`, `no-name`, `bad-name`, `no-version`,
  `bad-version`, `min-os`, `no-prog`, `bad-prog`, `bad-rgb`,
  `too-many-tooltypes`, `bad-tooltype`, `dup-id`, `dup-name` and
  `registry-full`); the accepted set is sorted by id and summarised as
  `CRUGOS: app registry ready (N app(s), M rejected)`.
- `app_launch()` starts the payload with `elf_spawn_user()` and passes the
  bundle directory as `argv[0]`, so a program can find its own manifest.
- `include/crug/syscall.h` grows `SYS_APP_COUNT` (25), `SYS_APP_INFO` (26)
  and `SYS_APP_SPAWN` (27) with `SYS_MAX` now 28. `SYS_APP_INFO(index, buf,
  len)` writes a text record `id|name|version|prog|RRGGBB|KEY=value;KEY=value`
  with no newline and uppercase hex, and rejects a bad index with `SYS_ENOENT`,
  a bad buffer with `SYS_EFAULT` and an overflow with `SYS_EINVAL`.
- `user/lib.h` gains `u_app_count()`, `u_app_info()` and `u_app_spawn()`,
  and `/bin/sh` gains the `app list`, `app info <id>` and `app run <id>`
  builtin with the `CRUGOS: sh app ...` markers.
- `wb.app=1` adds the `Applications:` icon at (16,300) and, on a double
  click, a lazily created window at (560,320,340,240): one click selects a row,
  a second click on the same row within 600 ms launches it, the desktop reaps the
  thread, and only one launch is in flight at a time.
- `scripts/mkdisk.sh` writes three bundles -- `Hello.app` (tool type
  `GREET="world"`, amber icon), `Orbit.app` (`MODE="demo"`, blue icon) and
  `Broken.app`, which has no version and is rejected with `no-version` -- and
  builds `user/app_hello.c` (exit 0) and `user/app_orbit.c` (exit 5) as their
  payloads rather than `/bin` programs.
- `user.test=7` runs `/bin/sh /etc/app.rc`, which performs `app list`,
  `app info orbit` and `app run orbit` and exits 9; the kernel requires that
  code and prints `CRUGOS: user app ok (exit 9)`.
- `make test-app-bundles` is gate 56 and has two legs: a desktop boot with
  `wb.app=1` that double-clicks the icon and the first row and checks both the
  window pixels and `scripts/boot-check.py --expect-app=yes` (a flag, so the
  argv layout is unchanged), then a `user.test=7` boot that drives the registry
  from the shell. Both end with `check-disk`.
- Host tests cover the manifest parser, the registry and the tool types, and
  `make test-host` reports 8216 checks; `make size` reports a 296848 B
  (289 KiB) file and a 942394 B (920 KiB) load image.
- Limits: eight applications at most, no signing or permission checks, one
  program with exactly one argument per launch, a rejected bundle is silently
  absent from the window, the icon is a colour plus a label, and metadata is read
  once at boot, so installing a bundle needs a reboot. See
  [apps.md](apps.md).
- docs: `docs/apps.md` (the bundle format, the manifest grammar, the rejection
  reasons, the registry, the Workbench window, the syscall ABI, the shell
  commands, the markers, the gate and the limits) plus README, workbench,
  userspace, roadmap, versioning, boot and build.
- All 56 gates pass at `v0.43.0`.

## Phase 7B - signed packages and permissions (done)

- The capability set is six bits in `include/crug/caps.h`: `fs.read` (0x01),
  `fs.write` (0x02), `app.launch` (0x04), `net` (0x08), `audio` (0x10) and
  `pkg.install` (0x20), with `CAP_ALL` 0x3F and the policy spelling the empty
  set `none`. Every thread carries a mask; kernel threads start with all of
  them and a ring-3 thread inherits its parent's.
- `kernel/lib/perms.c` reads `/System/perms.conf` (`<id> <caps>` rules plus a
  `default` line, eight rules at most) and prints
  `CRUGOS: perms policy ready (default <caps>, N rule(s))` at boot. A bundle
  launch narrows the new thread to `cap_intersect()` of the manifest's
  `caps` and the policy grant for that id, so the manifest can only ask for
  less than the policy allows.
- The syscall layer enforces the mask: `SYS_OPEN` needs `fs.read` (plus
  `fs.write` for a writable open), `SYS_LISTDIR` needs `fs.read`, socket and
  resolve need `net`, `SYS_APP_SPAWN` needs `app.launch` and
  `SYS_PKG_INSTALL` needs `pkg.install`; a missing bit returns
  `SYS_EPERM` (-1) before the operation runs. `SYS_CAP_QUERY` (28) returns the
  mask and `SYS_CAP_CHECK` (29) answers 1 or 0.
- `kernel/crypto/sha512.c` and `kernel/crypto/ed25519.c` are the first
  cryptographic code in the tree: a 512-byte package header carries the
  publisher public key and a 64-byte Ed25519 signature, and the payload is
  covered by a SHA-512 digest, so a package is verified without a heap and
  without floating point.
- A package is a single `.crpkg` file with a 512-byte header (`PKG_MAGIC`,
  id, name, version, publisher, payload size, capability mask, key id) and a
  bounded payload of at most 32 files, 96 KiB each and 128 KiB total.
  `/System/trusted/keys.conf` holds `pub <hex> <publisher>` lines; an
  unknown key is refused as `untrusted-key`.
- Installation is staged and atomic: the payload unpacks under
  `/Applications/.stage/<id>`, the directory is renamed to
  `/Applications/<id>.app`, a record is written to `/System/pkgdb/<id>.txt`
  and the bundle is registered in the same boot, so a failed step leaves no
  half-installed application.
- `make mkpkg` builds a package from a directory with `MKPKG` and
  `PKG_DATA_DIR`, and the seeds `PKG_SEED` and `PKG_BAD_SEED` produce a
  trusted and an untrusted package for the selftest; the host suite also links
  `kernel/app/pkg.c`, `kernel/lib/caps.c`, `kernel/lib/perms.c` and the two
  crypto modules.
- `include/crug/syscall.h` grows `SYS_PKG_COUNT` (30), `SYS_PKG_INFO` (31)
  and `SYS_PKG_INSTALL` (32) with `SYS_MAX` now 33; the info record is
  `id|name|version|publisher|RRGGBB|caps` and is capped at
  `SYS_PKG_INFO_MAX` (192) bytes.
- `wb.pkg=1` adds the desktop installer: a `Packages` window at
  (40,330,420,200) lists the installed packages, one body click installs
  `/System/packages/captest.crpkg`, and a failed attempt paints the refusal
  reason in the row. `make test-workbench-pkg` is gate 59 and checks the
  window pixels, the install count and the serial markers before `check-disk`.
- `make test-pkg` is gate 57 (`pkg.test=1`): a trusted package installs, a
  second install and an untrusted key are rejected, the bundle is registered and
  the marker reads `pkg selftest ok (1 installed, 2 rejected)`.
- `make test-caps` is gate 58 (`pkg.test=2`): it launches
  `/bin/captest` with the policy grant of `fs.read` only, requires the
  denied write and denied install and the exit code 5, and prints
  `caps selftest ok (exit 5, caps 0x1)`.
- Host tests cover SHA-512, Ed25519, the capability table, the policy parser,
  the trust list, the header, the payload container and the reason strings:
  `make test-host` reports 8403 checks. `make size` reports a 329016 B
  (321 KiB) file and a 980414 B (957 KiB) load image.
- Limits: no revocation and no way to drop a capability at run time,
  `SYS_SPAWN` and `SYS_EXEC` are not gated and hand the child `CAP_ALL`,
  `audio` is never checked, the trust list has no rotation or expiry, and the
  key id is informational. See [packages.md](packages.md).
- docs: `docs/packages.md` (the capability model, the policy, the container,
  the signing tool, the trust list, the install pipeline, the syscall ABI, the
  selftests, the desktop installer, the gates, the failure modes and the
  limits) plus README, workbench, userspace, roadmap, versioning, boot and
  build.
- All 59 gates pass at `v0.44.0`.

## Phase 7C - package update and removal (done)

- `pkg_install()` reads the record in `/System/pkgdb/<id>.txt` and compares
  the header version with `app_version_compare()` before it stages anything:
  the same version is refused as `already-installed`, an older header as
  `downgrade`, and a newer one becomes an update.
- An update keeps the staged-and-renamed install path and adds a backup: the
  installed tree is renamed to `/Applications/.stage-<id>.old` before the
  stage is renamed into place, every failure restores it, and it is deleted only
  after the new record is on disk and the registry rescanned. The marker is
  `CRUGOS: pkg updated <id> <old> -> <new> (N file(s), M cap(s))`.
- `pkg_remove(id)` validates the id, requires a registry entry or a bundle
  directory, remounts the root writable, deletes `/Applications/<id>.app`
  recursively, unlinks the pkgdb record, fsyncs, drops the registry entry and
  rescans, then prints `CRUGOS: pkg removed <id> (N package(s))`; the reason
  set grows `downgrade`, `not-installed` and `read-only`.
- `SYS_PKG_REMOVE` (33) is the only removal path: it takes the package id,
  needs `pkg.install` and returns `SYS_EPERM`, `SYS_EFAULT` or
  `SYS_EIO`; `SYS_MAX` is now 34.
- The capability hole is closed: `SYS_SPAWN` and `SYS_EXEC` require
  `app.launch` (`SYS_EPERM` otherwise) and `elf_spawn_user_caps()` hands
  the child the caller's mask instead of `CAP_ALL`, so a narrowed program
  cannot regain rights by starting `/bin/sh`.
- The ring-3 shell gained `pkg list`, `pkg info <id>`, `pkg install
  <path>` and `pkg remove <id>`; `user/lib.h` adds `u_pkg_remove()` and
  `/bin/captest` now also proves that `pkg.remove`, spawn and exec are
  denied.
- `wb.pkg=1` turns the desktop window into a toggle: the body press installs
  `captest` when it is absent and removes it when it is present, paints
  `installed` or `removed`, and the self-test counts the two installs and
  the remove.
- Gate 57 now boots `pkg.test=1` through install, update to 2.0.0, the
  `downgrade` refusal, the `already-installed` refusal, a removal, the
  `not-installed` refusal, an untrusted refusal and a reinstall, and
  `scripts/mkpkgtest.sh` builds the second package from the same seed. Gate 59
  presses the body three times and prints
  `ok: workbench package toggle (2 installs, 1 remove, exit 33)`.
- Host tests add the version-comparison cases the update and the `downgrade`
  refusal depend on: `make test-host` reports 8408 checks. The gate count
  stays at 59 because each phase's gate grew rather than adding another one.
- Limits: no file-level merge (an update replaces the bundle wholesale), no
  rollback beyond the stage/backup swap, `audio` is still never checked, and
  a removal takes no reference count, so a running bundle is unlinked from under
  itself. See [packages.md](packages.md).
- docs: `docs/packages.md` gains the updating and removing sections, the
  syscall row, the new markers and the revised limits, alongside README,
  userspace, workbench, versioning, boot and build.
- All 59 gates pass at `v0.45.0`.

## Phase 8A - virtio-gpu 2D scanout (done)

- `include/crug/gpu.h` and `kernel/gpu/gpu_util.c` hold the pure half of
  the path (the 24-byte control header, the response classes and names,
  rectangle clipping, the display-mode pick, the backing-page count and the
  damage bounding box), so the host tests can exercise the encoding rules
  without a device; `drivers/gpu/virtio_gpu.c` is the driver itself.
- The driver binds a **modern** virtio-pci function (device id 0x1050) and
  sets the legacy id 0x1010 aside as reported-but-unusable: it maps the
  common, notify, ISR and device capability structures, builds control queue
  0 from three contiguous pages, keeps one command in flight and puts a
  two-second deadline on each.
- Six control commands: `GET_DISPLAY_INFO`, `RESOURCE_CREATE_2D`,
  `SET_SCANOUT`, `RESOURCE_ATTACH_BACKING`, `TRANSFER_TO_HOST_2D` and
  `RESOURCE_FLUSH`. One `B8G8R8X8_UNORM` resource (id 1) is created with
  `ceil(w*h*4 / 4096)` contiguous backing pages (1000 at 1280x800), attached
  as a linear framebuffer and handed to `SET_SCANOUT`; the boot prints
  `CRUGOS: virtio-gpu ready (1280x800, 1000 backing page(s), resource 1,
  modern pci)` and `CRUGOS: gpu scanout ok (1280x800, 1 transfer(s), 1
  flush(es))`.
- The mode comes from `gpu.size=WxH` when given, otherwise from the host
  display info, otherwise from the 1024x768, 800x600 and 640x480 ladder,
  capped at 4096x2160 and 8192 backing pages.
- `include/crug/fb.h` gained `fb_attach_linear()` plus `fb_present_begin()`
  / `fb_present_end()`: `fb_store()` merges every pixel it writes into one
  damage rectangle and a present clips that rectangle, transfers it and
  flushes it, so a whole desktop repaint costs a single round trip because
  `intuition_redraw()` and `intuition_redraw_window()` bracket their work.
  The console flushes once per line rather than once per glyph.
- The software path is intact: without a device, or with `gpu.enable=0`,
  the boot keeps the firmware framebuffer, prints one reason line (or nothing
  when the PCI bus is empty), `gpu_selftest()` returns 0 and boot continues,
  so all 59 earlier gates still boot a GOP framebuffer unchanged.
  `gpu.test=1` only controls the `CRUGOS: gpu selftest skipped (software
  framebuffer)` line.
- Gate 60 `make test-gpu` boots `-vga none -device virtio-gpu-pci,id=gpu0`
  with `wb.enable=1 wb.wait=30 gpu.enable=1 gpu.test=1`, so no firmware
  framebuffer exists and every Workbench pixel travelled through the device;
  `scripts/run-gpu.py` takes a QMP screendump and checks the desktop
  background, the amber shell title, the idle Workbench title and its frame,
  then prints `ok: virtio-gpu scanout 1280x800 (13 transfer(s), 13 flush(es),
  exit 33)`. `boot-check.py` gained the `--expect-gpu=yes` flag and
  `check-disk.py` finishes the gate; the knobs are `GPU_WB_WAIT` (30 s),
  `GPU_RUN_TIMEOUT_SEC` (300 s) and `GPU_QEMU_EXTRA`.
- Gate 61 `make test-gpu-usb` repeats that scanout with
  `-device qemu-xhci,id=xhci` from `GPU_USB_QEMU_EXTRA` and
  `MACHINE_EXTRA='usb=off,i8042=off'`, hot-plugs a `usb-mouse` on
  `xhci.0` over QMP and requires
  `CRUGOS: usb controller ready (xhci, 8 port(s))` plus two
  `CRUGOS: usb mouse move` reports before the same four pixels.
  It pins the fix for a 64-bit BAR decode bug: `bar_hi = orig_hi & 0xfffff000u`
  zeroed the `0xc0` high dword OVMF uses at `0xc000004000` when the
  GPU occupies the low PCI window, so the driver mapped `0x4000`, logged
  `CRUGOS: usb controller absent (reset timeout)` and left `make demo`'s
  keyboard and mouse dead; the address now takes the high dword unmasked and
  the size probe is built from both dwords.
- Host tests add `tests/host/test_gpu.c` (command encoding, response
  classes, clipping, backing pages, display pick, damage merge): `make
  test-host` reports 8488 checks and `make size` a 347208 B (339 KiB) file
  and a 994626 B (971 KiB) load image (`.text` 248230 B, `.rodata` 37696 B,
  `.data` 92 B, `.bss` 708608 B).
- Limits: 2D only -- no virgl, Vulkan, command stream, hardware cursor
  plane or the specification's accelerated triangle, which phase 8B has to
  add; one resource and one display, no mode set or hotplug after boot, one
  command in flight, an 8192-page contiguous backing cap, and the QEMU
  screendump is the only automated proof. See [gpu.md](gpu.md).
- docs: `docs/gpu.md` is new and README, boot, build, workbench and
  versioning follow it.
- All 61 gates pass at `v0.46.0`.

## Phase 8B - EDID and the hardware cursor (done)

- `include/crug/gpu.h` gained the pure EDID and cursor surface:
  `GPU_EDID_MAX_BYTES` (1024), `GPU_EDID_MAX_MODES` (4), `GPU_CURSOR_PX` (64),
  `struct gpu_edid_mode` (clock in kHz, width, height, refresh),
  `struct gpu_edid` (block count, vendor, product, serial, week, year, size in
  cm, up to four modes, the preferred timing and a `valid` flag),
  `gpu_edid_parse()`, `gpu_edid_vendor()`, `gpu_edid_available()` and the
  `gpu_cursor_ready()` / `_updates()` / `_moves()` counters;
  `kernel/gpu/edid.c` is the parser and is compiled into the host tests.
- Parser rules: at least 128 bytes, the `00 FF FF FF FF FF FF 00` header and a
  checksum of 0 mod 256, then the four 18-byte detailed timing descriptors at
  offset 54. Descriptors whose first word is 0 are display descriptors and are
  skipped, at most four timings are kept, the first one is the preferred mode,
  and a payload with no timing returns -1 with a zeroed result. The vendor id
  is one big-endian word split into three five-bit letters; a letter outside
  1..26 prints `???`. Bit 15 of that word is *not* a validity flag -- treating
  it as one was the bug 8B fixed.
- The driver asks `GET_EDID` (0x20) on control queue 0 right after
  `GET_DISPLAY_INFO`, after seeing feature bit 2 (`VIRTIO_GPU_F_EDID`; bit 1 is
  virgl, bit 0 the 3D feature) and expecting response 0x1104. The mode order
  is now `gpu.size=`, then the EDID preferred timing, then the host display
  info, then the 1024x768/800x600/640x480 ladder, and the boot names the source:
  `CRUGOS: gpu edid 1152x864 (vendor RHT, product 0x1234, 1 mode(s))` and
  `CRUGOS: gpu mode 1152x864 (edid preferred)`.
- The pointer moved onto the device's hardware cursor plane: a second
  virtqueue (queue 1) with its own 64x64 resource (id 2) and 56-byte
  `UPDATE_CURSOR`/`MOVE_CURSOR` commands, each submitted with one non-write
  descriptor and a single 16-bit notify store, waiting only for the used index
  because the cursor queue returns no response. Queue 1 is enabled before
  `DRIVER_OK`; without it the boot prints
  `CRUGOS: gpu cursor queue unavailable (no cursor plane)` and stays on the
  software sprite.
- The wire layout was the release's real bug: `struct
  virtio_gpu_update_cursor` puts `struct virtio_gpu_cursor_pos` (scanout_id,
  x, y, padding) first, so `scanout_id` lives at offset 24. The first cut of
  8B put x at 24 in a 48-byte command, QEMU's `update_cursor()` read
  `scanout_id = 640`, returned at once and dropped every cursor command: no
  define, no plane, no pixels, while the guest printed healthy show and move
  counters. `GPU_CURSOR_CMD_BYTES` is now 56 and the driver writes scanout 0,
  x, y, padding, the resource id and hotspot 0,0 there; `gpu_cursor_upload()`
  also sends `TRANSFER_TO_HOST_2D` before the first show because 2D keeps the
  resource's pixels host-side.
- The cursor commands used to go through the control queue, which has no cursor
  case and answers `0x1200` err-unspec, so the kernel silently fell back to
  the sprite even when the device had a plane. `include/crug/fb.h` now carries
  `struct fb_cursor_ops` (show/move/hide) and `fb_set_cursor_ops()`; `fb.c`
  defers every show, move and hide to those ops and falls back to the old
  software sprite when an op fails or none is installed, and
  `fb_attach_linear()` resets the shown state. The boot prints
  `CRUGOS: gpu cursor ready (64x64, resource 2, hardware plane)`,
  `CRUGOS: gpu cursor show 640 400`, `CRUGOS: gpu cursor move 648 404` and
  `CRUGOS: gpu cursor selftest ok (2 show(s), 1 move(s))`; the selftest hides,
  shows at the centre, shows 8 px right and 4 px down, hides again and fails if
  either counter is zero.
- Gate 62 `make test-gpu-edid` pins the device to 1152x864 with
  `GPU_EDID_DEVICE ?= virtio-gpu-pci,id=gpu0,xres=1152,yres=864` and
  `GPU_EDID_QEMU_EXTRA ?= -vga none -device $(GPU_EDID_DEVICE)` -- a timing
  that is neither the driver's 1024x768 fallback nor QEMU's 1280x800 default,
  so only a real `GET_EDID` can produce it. `scripts/run-gpu.py` takes its
  `--expect-edid` leg and the gate ends on
  `ok: virtio-gpu scanout 1152x864, mode from the EDID (14 transfer(s),
  14 flush(es), exit 33)` with `boot-check.py --expect-gpu-edid=yes`.
- Gate 63 `make test-gpu-cursor` uses the default device from
  `GPU_QEMU_EXTRA`, where QEMU manufactures a 1280x800 EDID. A screendump of
  `gpu0` never contains the hardware cursor plane, so `run-gpu.py
  --expect-cursor`, the serial counters and the appended `gpu.cursor=hw` are its
  evidence and the gate prints
  `ok: virtio-gpu scanout 1280x800, hardware cursor (14 transfer(s),
  14 flush(es), exit 33)`. Those counters proved nothing on their own while
  the layout was wrong.
- Gate 64 `make test-gpu-cursor-shape` is the pixel proof: it boots under
  `QEMU_DISPLAY=vnc=127.0.0.1:<port>` with `GPU_SHAPE_QEMU_EXTRA ?= -vga none
  -device virtio-gpu-pci,id=gpu0`, and `scripts/run-gpu-cursor-shape.py` speaks
  RFB 3.8 with `gpu.cursor=hw` on the command line, requests `VNC_ALPHA_CURSOR`
  updates and waits for an alpha-cursor
  rect. `check_shape` demands 64x64, at least 100 opaque pixels and both the
  white border (`0xFFFFFF`) and the amber body (`0xFFD040`):
  `gpu: cursor shape 64x64 at 0,0 (120 opaque pixel(s), 2 colour(s))` and
  `ok: hardware cursor shape 64x64 carries 120 opaque pixel(s) in 2 colour(s),
  exit 33`, then `boot-check.py --expect-gpu=yes --expect-gpu-cursor=yes` and
  `check-disk.py`. `GPU_SHAPE_WB_WAIT` (30),
  `GPU_SHAPE_BOOT_TIMEOUT_SEC` (180), `GPU_SHAPE_TIMEOUT_SEC` (240),
  `GPU_SHAPE_RUN_TIMEOUT_SEC` (300) and `GPU_SHAPE_QEMU_EXTRA` are overridable;
  a dropped `UPDATE_CURSOR` produces no rect and a cursor-shape timeout. The
  same probe on the demo command line returned the shape, which is the proof
  that the `make demo` pointer is back.
- Host tests add a synthetic 128-byte EDID to `tests/host/test_gpu.c` (vendor
  `RHT` = id 0x4914, product 0x1234, a 1024x768@60 timing at 6500x10 kHz and an
  800x600@60 timing at 4000) plus bad-checksum, bad-header, 127-byte,
  no-timing, null-pointer, four-timing-cap and unknown-vendor cases: `make
  test-host` reports 8528 checks and `make size` a 352528 B (344 KiB) file and
  a 1002890 B (979 KiB) load image (`.text` 251622 B, `.rodata` 38464 B,
  `.data` 100 B, `.bss` 712704 B).
- Limits: 3D still needs virgl, which this host cannot exercise (no `/dev/dri`
  render node, so QEMU prints `The display backend does not have OpenGL
  support enabled`); blob resources need `blob=on` and are unused; one scanout
  and one display, no multi-head (`max_outputs=2` is not gated); `gpu.size=`
  still wins over the EDID by design; the pointer defaults to the guest's
  software sprite (12x19, `0xFFD040` body) because a GTK window does not
  composite the device cursor plane, while `gpu.cursor=hw` returns to the 64x64
  plane that only a compositing surface such as a VNC client shows. See
  [gpu.md](gpu.md).
- Follow-up `v0.47.1` fixes the invisible demo pointer: QEMU's GTK UI
  (`gd_update_cursor`) hides the window cursor whenever the pointer is grabbed
  or absolute and expects the guest to paint it, and `fb_cursor_composite()`
  had stopped painting as soon as a cursor op set was installed. The new token
  `gpu.cursor=sw|hw` defaults to `sw`, composites the guest's 12x19 sprite into
  the scanout and prints `CRUGOS: gpu cursor software sprite (gpu.cursor=sw)`;
  `hw` keeps the 8B plane (`CRUGOS: gpu cursor ready (64x64, resource 2,
  hardware plane)`), a bad value prints `CRUGOS: gpu cursor mode ignored (use
  gpu.cursor=sw|hw)` and keeps the sprite, and `gpu_cursor_mode_parse()` in
  `kernel/gpu/gpu_util.c` is the pure 1/0/-1 decoder. `make demo` passes
  `gpu.cursor=$(DEMO_CURSOR)` and defaults `DEMO_CURSOR` to `sw`; gates 63 and
  64 append `gpu.cursor=hw` explicitly.
- Gate 65 `make test-gpu-cursor-sw` is the pixel proof for the default: it boots
  with `gpu.cursor=sw`, waits for the sprite marker, fails if the hardware
  ready line appears, finds the last pointer position in the serial log and
  counts the `(255, 208, 64)` pixels inside the 12x19 sprite box, needing at
  least 40 (`gpu: sprite at (640, 400) carries 71 body px`), then prints `ok:
  virtio-gpu scanout 1280x800, software cursor sprite (15 transfer(s), 15
  flush(es), exit 33)`. `make test-host` adds `test_gpu_cursor_mode` (7 checks)
  for 8528.
- docs: `docs/gpu.md` gains the EDID mode-selection and hardware-cursor
  sections, the "Which pointer the guest draws" section, the sprite-upload note
  and the gate 62/63/64/65 paragraphs, alongside README, boot and build.
- All 65 gates pass at `v0.47.1`.

## Phase S1 - security pass 1 (done)

A review of the ring-3 and network surface (`specs/fix-p2/`, a tree at
`a1a99df`) raised 1 Critical, 10 High, 16 Medium, 14 Low and 9 Info items.
Phase S1 closes the Critical, four Highs and the cheap Mediums, proves each one
with an opt-in self-test and a gate, and writes the rest down for S2. It is a
fix pass rather than a feature, so the phase number is `S1` and the boot marker
is `CRUGOS: phase s1 boot ok`.

- C1: `dns_decode_name()` in `kernel/net/dns.c` is now iterative, with one
  `DNS_MAX_HOPS` (16) budget for the whole name and pointers that must move
  strictly backwards; a 12-link chain decodes and a 1200-link chain is
  rejected without touching the 16 KiB kernel stack (`dns.test=hostile`).
- H1: a DNS reply is accepted only from the server address and port the query
  went to, on the local port it used, with a matching id; ids come from an
  xorshift64 stream seeded from bootloader entropy and the TSC, and the
  resolver keeps two local ports instead of the old fixed 53000.
- H4: `tcp_listen_input()` answers a failed allocation with RST|ACK and counts
  an overflow, `tcp_reap_stale()` releases `SYN_SENT`/`SYN_RECEIVED` sockets
  older than `NET_TCP_SYN_TIMEOUT_NS` (10 s) from `tcp_socket_new()`, and
  `tcp_rst_valid()` keeps an off-path RST from killing a healthy connection
  (`net.synflood=1`).
- H8: `kernel/arch/x86_64/idt.c` installs a shared `isr_ignore` stub on gates
  33-255 (syscall vector excluded) and prints `CRUGOS: idt gates 256 present`;
  a user-mode vector 4 is `TRAP_USER`, `trap_dispatch()` returns for
  `TRAP_HANDLED`, and `/bin/trap4` executes `int $4` from CPL3 to prove only the
  thread dies (`user.test=8`).
- H9: `pkg_id_valid()` (declared in `include/crug/pkg.h`) rejects empty ids,
  `.`, `..` and any path separator before the installer touches the
  filesystem, closing the `..` staging-directory redirect.
- Mediums: UDP datagrams are accepted only for our address or broadcast and a
  non-zero checksum is verified over the pseudo-header (`net_udp_checksum()` /
  `net_udp_checksum_ok()` in `kernel/net/net_util.c`; zero stays legal), the
  virtio-net driver clamps the device's `used_len` to `VIRTIO_NET_BUF_SIZE`,
  and `msgport_recv()` returns the copied byte count instead of the full
  length, repairing the P1-F12 truncation report.
- Gates 66, 67 and 68 are `make test-dns-hostile`, `make test-trap-vector` and
  `make test-net-synflood`, sharing `S1_RUN_TIMEOUT_SEC` (240); each boot
  token is opt-in, and every other gate fails if the markers appear without
  the matching flag.
- One finding is rejected rather than fixed: the claim that `GET_EDID` reads
  `size` at offset 28 is wrong, because `struct virtio_gpu_ctrl_hdr` is 24
  bytes (see `/usr/include/linux/virtio_gpu.h` lines 354-359). Deferred to S2
  are the two network Highs (no `net_poll()` locking, ARP slot eviction and
  TCP ACK re-entry), the handle and port lifetime work (H5, H6, H7, H10) and
  the remaining Mediums.
- docs: [security.md](security.md) is new; README, boot, build, network and
  traps gained the S1 tokens, gates, markers and limits.
- `make test-host` reports 8552 checks, 0 failure(s); `make size` reports a
  357592 B (349 KiB) kernel file and a 1010298 B (986 KiB) load image
  (`.text` 254310 B, `.rodata` 39072 B, `.data` 116 B, `.bss` 716800 B).
- All 68 gates pass at `v0.48.0`.

## Bootable media - hybrid UEFI ISO (done, `v0.23.1`)

- `kernel/fs/storage.c` gained the `storage=none` command-line token: every
  block transport is still probed and prints its `ready` line, then
  `CRUGOS: storage skipped (storage=none)` replaces the GPT/root/ext4/journal
  block and `storage_selftest()` becomes a soft skip. Without the token a boot
  with no disk still fails closed with `CRUGOS: no root filesystem`.
- `scripts/mkfat.py` writes the FAT16 ESP image directly (no mtools on this
  host): deterministic timestamps and ordering, 8.3 names only, FAT16 cluster
  bounds enforced, plus `--list` and `--read` for inspection.
- `scripts/mkiso.sh` copies the ESP, rewrites `crugos/cmdline.txt` from
  `ISO_CMDLINE` (default `storage=none`), builds and `fsck.vfat`-checks the FAT
  image, grafts the same files at the ISO root and runs `xorriso -as mkisofs`
  with the El Torito entry, `-part_like_isohybrid` and
  `--protective-msdos-label`.
- `scripts/check-iso.py` re-parses the artifact: ISO 9660 PVD, El Torito boot
  record and catalog checksum, the FAT16 BPB at the catalog's load address, the
  protective MBR's type-`0xEF` entry, an `xorriso -osirrox` extraction compared
  against the catalog range, `fsck.vfat -n`, and the four files.
- `make test-iso` boots the image twice headless -- `dd`'ed onto a file and
  attached through `usb-storage` on `qemu-xhci`, then as a CD-ROM -- and runs
  `boot-check.py --storage=none` on both logs. `make usb USB_DEV=...
  USB_CONFIRM=yes` writes real media and refuses a mounted device.
- The image has a protective MBR, not a GPT: `xorriso` cannot synthesize a GPT
  without the Syslinux `isohybrid` MBR, and a hand-written GPT would collide
  with the ISO 9660 volume descriptors in the first 32 KiB. See
  [iso.md](iso.md).
- All 36 gates pass at `v0.23.1`.

## Security programme

Security work runs as its own sequence of fix phases rather than as features
folded into the bring-up phases. Each phase starts from a written review of a
tree, ends with a gate per fix, and never deletes a finding: a fixed item is
marked done with the phase that closed it, and a deferred one stays listed with
the reason it was deferred.

- S1 (`v0.48.0`, done): the Critical DNS decompression recursion, the DNS reply
  source/id checks, TCP table exhaustion and RST validation, the ring-3 trap
  vector hole and package-id validation, plus the UDP checksum/address verdict,
  the virtio-net length clamp and the message-port copied-length repair.
  Gates 66, 67 and 68. See [security.md](security.md).
- S2 (planned): the deferrals from `specs/fix-p2/`. The network lock comes
  first, because ARP slot eviction and TCP ACK re-entry cannot be fixed on a
  re-entrant receive path; then the handle/port lifetime work as one redesign
  (`SYS_HANDLE_CLOSE` never destroys a port, no handle teardown at thread exit,
  one global 256-entry table with no rights checks, and the lost-wakeup /
  use-after-free pair on a closed port); then the remaining Mediums (unwaited
  zombies pinning memory, no per-process quota, the trusting `kfree()` large
  path and deterministic ephemeral ports).
- A next review (`specs/fix-p3/`) is expected after S2 and follows the same
  shape: report, findings, fix, marker, gate, document.

## Phase 3B - remaining storage work

- Journal wrapping, revoke records and orphan-file recovery.
- Framebuffer/GOP console input (the serial line, the PS/2 keyboard or a USB
  HID keyboard can type the passphrase today) plus virtual terminals with
  Ctrl+Alt+Fn switching.
- USB storage drivers, hotplug and more than four NVMe/AHCI controllers.
- RAID online rebuild, hot spare, scrub, a write-intent bitmap and persisted
  failure tracking (rebuild is boot-time and synchronous today).
- RAID below encryption: unlock one `crypt.device` above `md0` so mirrored
  members share one ciphertext, plus TPM/keyfile keyslots, a rekey/slot tool and
  per-sector integrity.
- Redundant ESP and a recovery path.

## Phase 4 - devices and userspace

- USB HID devices beyond the boot keyboard, mouse and audio function (phase 5A
  delivered the xHCI controller and the keyboard, phase 5B the mouse, phase 5C
  `audio.device`, the mixer and the USB Audio Class driver, phase 5D the
  pointer-driven cursor on the GOP framebuffer, phase 6A the intuition desktop
  and window manager on top of it, phase 6B its menus, gadgets and event queue,
  phase 6C the shell window, phase 6D resize grips and keyboard navigation and
  phase 6E desktop objects and the depth gadget;
  phase 4A delivered the PS/2 console input step, phase
  4B the virtio-net/IPv4 slice, phase 4C the TCP/socket slice and phase 4D the
  DNS resolver).
- A routing table and richer resolver features (multiple servers, DNSSEC
  validation, AAAA/PTR/SRV, search domains, negative caching) on top of the
  phase 4B-4D stack, richer socket semantics (bind, options, non-blocking, more
  than four sockets); IPv6, fragmentation and reassembly.
- Software framebuffer -> VirtIO-GPU -> vendor GPU drivers: with the phase 6I
  desktop in place (window resizing, the depth and front gadgets, icon dragging,
  a scrolling, refreshing drawer and its selectable rows and draggable
  scrollbar, and up to three retained screens with per-screen windows and icons,
  a screen strip and a window move between screens are done), the next Workbench
  increments are application launching (application bundles, phase 7,
  specification section 35) and Commodities, plus keyboard row navigation and
  Enter activation of icons from the drawer, multi-select, a gadget toolkit,
  sub-menus on top of the phase 6B menu bar, renaming an icon and richer shell
  semantics (quoting, a working directory) on top of the shell, the phase 5D
  cursor, the phase 5B pointer stack and the phase 5C audio stack.
- Workbench userspace, `.app` bundles, `.crpkg` packages, capability model.
- POSIX-like layer: 64-bit UID/GID, ACLs, process groups, signals.

## Phase 5 - scale-out

- SMP bring-up (APIC/SIPI), IOMMU, PCIe enumeration.
- Secure Boot, virtualisation (VMX/SVM).
- Containers (OCI) and a Docker-style microVM workflow.
- HPC fabric libraries and performance work.

## Spec gaps to resolve

1. The spec sketches Crug namespaces, `let`, attributes and kernel-safe
   ownership annotations; crugc 0.2.3 implements C plus `own/ref/mut/move/
   adopt/drop/borrow/slice/chan/select` and rejects ownership in kernel mode.
   Phases 0-3 therefore use plain C-style Crug. Compiler work is tracked as a
   prerequisite for the capability/ownership-heavy phases (G0.5).
2. The spec once targeted x86_64 -> aarch64 -> riscv64, but only a native
   x86_64 `crugc` exists and the project owner dropped the AArch64 and RISC-V
   64 phases on 2026-10-30. The native x86_64 target is the only one; the spec
   marks Phase 13 and Phase 14 as dropped.
3. Boot performance targets (<250 ms) now have calibrated stage timings in the
   boot report, but the gate is not enforced yet; unencrypted boot currently
   spends most of its time in PIT-based TSC calibration, and an encrypted boot
   adds roughly 200 ms of Argon2id work under TCG (m=4096, t=3). A production
   install needs a deliberate KDF-parameter/boot-time tradeoff.
4. The spec's 16 MiB headless target cannot be met with OVMF: under this
   QEMU/OVMF pair `MEMORY_MB=16`, `32`, `64` and `128` fail before the loader
   runs (`No bootable option` or no output), and 256 MiB is not reliable. The
   kernel itself is small (about 200 KiB load, 128 KiB of that is the PMM
   bitmap for a 4 GiB cap), so the limit is firmware memory use, not kernel
   memory use. A leaner boot path or a lower RAM cap is future work.
