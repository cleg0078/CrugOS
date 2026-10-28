# Roadmap

The specification defines 40 phases/gates. Phase 0 established the smallest
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
stacking order. Release
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
  rather than reflowed, and no send-to-back in the stacking order (the phase 6E
  depth gadget sends a window back, but nothing sends one forward).
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
- Software framebuffer -> VirtIO-GPU -> vendor GPU drivers: with the phase 6E
  desktop objects and depth gadget in place, the next Workbench work is icon
  dragging and renaming, a drawer that refreshes and scrolls while it stays
  open, a send-to-front gadget, keyboard focus for the icon table and reflowing
  a body (and relaying out its painter) when its window is resized, followed by
  sub-menus on top of the phase 6B menu bar, a gadget toolkit on top of its
  event queue and richer shell semantics (quoting, a working directory) on top
  of the shell, the phase 5D cursor, the phase 5B pointer stack and the phase 5C
  audio stack; application bundles (phase 7, specification section 35) are the
  next layer above the desktop.
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
