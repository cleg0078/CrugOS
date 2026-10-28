# Roadmap

The specification defines 47 phases/gates. Phase 0 established the smallest
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
click and to drag its scrollbar thumb. Release
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
and put the four stack-destroying vectors on IST stacks, and phase H4 took the
mount path to the host with a seeded image fuzzer. What remains is a per-thread
`rsp0` and the rest of ring 3, a hook that kills a user thread instead of
panicking, weak EFI entropy, and the AHCI timeout bounce reuse. Those are
prerequisites for a hostile process: a DPL3 frame would reach the syscall and
trap paths that the crafted-image gate only exercises from ring 0, and several
of the remaining Mediums become Critical once one can call them directly.

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
- Software framebuffer -> VirtIO-GPU -> vendor GPU drivers: with the phase 6H
  desktop in place (window resizing, the depth and front gadgets, icon dragging,
  a scrolling, refreshing drawer and its selectable rows and draggable
  scrollbar are done), the next Workbench increments are keyboard row
  navigation and Enter activation of icons from the drawer, multi-select, a
  gadget toolkit, sub-menus on top of the phase 6B menu bar, renaming an icon
  and richer shell semantics (quoting, a working directory) on top of the
  shell, the phase 5D cursor, the phase 5B pointer stack and the phase 5C audio
  stack; application bundles (phase 7, specification section 35) are the next
  layer above the desktop.
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
2. The spec targets x86_64 -> aarch64 -> riscv64, but only a native x86_64
   `crugc` exists. aarch64/riscv64 bring-up requires a cross backend.
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
