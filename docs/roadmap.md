# Roadmap

The specification defines 38 phases/gates. Phase 0 established the smallest
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
caching DNS stub resolver with a UDP query, a TCP fallback and a name syscall.
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

## Phase 3B - remaining storage work

- Journal wrapping, revoke records and orphan-file recovery.
- Framebuffer/GOP console input (the serial line or the PS/2 keyboard can type
  the passphrase today) and a USB HID keyboard through xHCI, plus virtual
  terminals with Ctrl+Alt+Fn switching.
- USB storage drivers, hotplug and more than four NVMe/AHCI controllers.
- RAID online rebuild, hot spare, scrub, a write-intent bitmap and persisted
  failure tracking (rebuild is boot-time and synchronous today).
- RAID below encryption: unlock one `crypt.device` above `md0` so mirrored
  members share one ciphertext, plus TPM/keyfile keyslots, a rekey/slot tool and
  per-sector integrity.
- Redundant ESP and a recovery path.

## Phase 4 - devices and userspace

- xHCI USB, HID, UAC2 audio (phase 4A delivered the PS/2 console input step,
  phase 4B the virtio-net/IPv4 slice and phase 4C the TCP/socket slice).
- A routing table and richer resolver features (multiple servers, DNSSEC
  validation, AAAA/PTR/SRV, search domains, negative caching) on top of the
  phase 4B-4D stack, richer socket semantics (bind, options, non-blocking, more
  than four sockets); IPv6, fragmentation and reassembly.
- Software framebuffer -> VirtIO-GPU -> vendor GPU drivers.
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
