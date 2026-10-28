# CrugOS

CrugOS is a small 64-bit operating system written in Crug,
a C dialect with Rust-style ownership and Go-style concurrency. It targets the
feel of AmigaOS: a compact monolithic kernel, message-passing userspace, a
Workbench shell and applications that are self-contained bundles.

The design contract lives in
[specs/CrugOS_complete_kernel_spec_v3.md](specs/CrugOS_complete_kernel_spec_v3.md).

## Status: phase 6B menus and gadgets (`v0.24.0`)

Phases 1-3B-10 keep the boot path under a kernel that owns memory, CPU state,
the execution model, a checksummed crash-consistent filesystem, mirrored/
striped RAID roots that can rebuild a replacement member, an AES-256-XTS
encrypted root that can be unlocked with a passphrase typed on the console,
and three storage transports; phase 4A adds console input from a PS/2 keyboard,
phase 4B adds a small IPv4 stack over a virtio-net NIC, phase 4C extends it
with TCP, an HTTP exchange in both directions and six socket syscalls, phase 4D
adds a caching DNS stub resolver with a TCP fallback and a name syscall, phase
5A adds an xHCI host controller with a USB HID boot keyboard that can be plugged
into the running guest, phase 5B adds a USB HID boot mouse whose reports become
pointer events, phase 5C adds a USB Audio Class path with a PCM ring and a
per-channel mixer, phase 5D adds a pointer-driven cursor composited onto the
GOP framebuffer console, phase 6A adds the first Workbench slice -- a software
desktop of intuition windows on that same framebuffer -- and phase 6B gives it
the Intuition interaction model: a shared menu bar with popup menus, gadgets
inside a window body, the close gadget and a typed event queue. Release
`v0.23.1` made that tree bootable media: `make iso` writes a hybrid UEFI ISO
that boots from a USB stick or a CD-ROM, with no disk image and no test
tokens baked in.

```
OVMF -> BOOTX64.EFI (CrugOS UEFI loader) -> kernel.elf -> serial + framebuffer
```

The loader reads the kernel from its own boot device, loads the ELF64 segments at
their physical addresses, collects a GOP framebuffer, the ACPI RSDP, a
normalized memory map and entropy, marks every page the kernel must not use and
calls `ExitBootServices`. It then jumps to `kernel_main(BootInfo *)`.

The kernel owns its physical memory through a 4 KiB bitmap allocator, builds
its own identity page tables with W^X permissions and an unmapped stack guard,
installs a GDT/TSS and a 256-vector IDT with exception handlers, and runs a
LAPIC timer with a calibrated TSC. Phase 2 adds:

- a kernel heap: eight small-object slab classes from 16 to 2048 bytes plus
  contiguous large allocations up to 1 MiB, all interrupt-safe;
- kernel threads with 16 KiB stacks and a guard page, `cpu_switch` context
  switching, and preemptive round-robin scheduling in the `IDLE`, `NORMAL`,
  `INTERACTIVE` and `REALTIME_SYSTEM` classes;
- sleep queues, wait queues, thread join and a scheduler idle thread;
- per-process typed handle tables (256 entries, process-local ids, no kernel
  pointers) and a kernel process;
- `MsgPort` IPC with typed messages (1024-byte payload, depth 64) and O(1)
  send/receive with blocking waiters;
- a single syscall dispatcher behind an `int 0x80` gate, with a small phase 2
  call set (ping, gettid, yield, sleep, port create/send/recv, handle close,
  exit).

Phase 3A adds a working storage path from PCI to a real filesystem:

- a PCI configuration-space scanner and a legacy virtio-blk driver (`vda`),
  with a polled split virtqueue, a 500 ms completion timeout and virtio flush;
- a block-device registry with bounds-checked I/O, flush propagation and
  partition offset views;
- GPT parsing with protective-MBR, header and entry CRC32 validation;
- an ext4 driver (1/2/4 KiB blocks, extents, 32/64-byte group descriptors,
  linear directories) that rejects unsupported features instead of misreading
  them;
- a generated 16 MiB GPT/ext4 test disk (`make disk`) containing `/hello.txt`,
  read and verified during boot.

Phase 3B-1 makes that filesystem writable:

- create, mkdir, write, truncate, unlink, rmdir, rename and fsync, with extent
  allocation, inode/block bitmaps and group free counts kept consistent;
- a boot self-test that creates `/selftest`, writes a 5000-byte pattern,
  renames and truncates it to 1000 bytes, deletes a temporary file, unmounts
  cleanly and remounts read-only to verify the result;
- a host `e2fsck` gate (`scripts/check-disk.py`) that also extracts the image
  and checks the persisted file contents.

Phase 3B-2 adds JBD2 journaling:

- the JBD2 V2 journal in inode 8, with descriptor/commit blocks and a 1 MiB
  journal in the generated test disk;
- ordered metadata journaling: file data is flushed before the commit, then
  inodes, bitmaps, group descriptors, directories and extent leaves are
  journaled, checkpointed and the journal is cleared;
- replay at mount: a dirty journal is validated and applied before the
  filesystem is used; a read-only mount of a dirty journal is refused;
- a boot self-test that leaves a dirty journal, unmounts, remounts and proves
  replay restored the block, while `e2fsck` still passes on the host.

Phase 3B-3 mirrors the root filesystem with RAID1:

- up to eight legacy virtio-blk disks (`vda`..`vdh`) are discovered by
  scanning every PCI function, each with its own virtqueue and flush support;
- each member carries a checksummed 512-byte metadata record in its last two
  sectors (magic `CRA1`, level, slot, member count, geometry, generation,
  array/member UUIDs);
- `raid_assemble()` validates the members and registers `md0`; reads take the
  first healthy member, writes and flushes go to every healthy member, and a
  broken member is dropped for the rest of the boot;
- `storage_init()` still prefers a direct `crugroot` partition, otherwise it
  assembles `fd00` members and boots from the array, including a degraded
  boot with one member missing;
- host tools `mkraid.sh`, `raid-meta.py` and `check-raid.py`; `make
  test-raid` gates a healthy array and `make test-raid-degraded` gates a
  degraded one, with metadata, `e2fsck` and data-region checks.

Phase 3B-4 adds a polled NVMe driver:

- PCI class `01/08/02` discovery, 64-bit BAR sizing and mapping through generic
  page tables, including the QEMU BAR at `0xc000000000` above the 4 GiB
  identity map;
- the controller is disabled and reset (OVMF may leave it running), then admin
  and I/O queue pairs are created; Create CQ/SQ use queue id in the low 16 bits
  and size in the high 16 bits of CDW10, CSTS is at `0x1C` and AQA at `0x24`;
- namespace 1 is identified and registered as `nvme0n1`; reads, writes and
  flushes use PRP1/PRP2 or a PRP list with a 1000 ms deadline;
- `storage_init()` probes virtio-blk and NVMe and mounts `crugroot` (or a RAID
  array) regardless of transport; `make test-nvme` boots with only an NVMe
  image and runs the same JBD2, writable-ext4 and `e2fsck` gates.

Phase 3B-5 adds RAID10:

- the checksummed metadata now carries level 1 or 10 and a chunk size; RAID10
  requires an even 4..8 members and maps sector ranges to mirror pair
  `(lba / chunk) % (member_count / 2)`, with slots paired as `2p`/`2p+1`;
- reads use a present member of the pair, writes go to both, failed members
  are dropped for the rest of the boot, and assembly rejects an array whose
  mirror pair has no member;
- `storage_init()` prints the geometry, e.g. `raid   : level 10, 4 member(s),
  2 pair(s), chunk 128 sectors`, before `raid healthy`/`raid degraded`;
- host tools `mkraid10.sh` and `check-raid10.py` build and verify four-member
  arrays; `check-raid10.py` rebuilds the logical image from one member per pair
  before `e2fsck`;
- `make test-raid10` gates a healthy four-member boot and `make
  test-raid10-degraded` gates a two-member boot (slots 0 and 2).

Phase 3B-6 rebuilds a cold spare:

- member metadata is now probed as valid, blank or corrupt: a member whose last
  two sectors carry no `CRA1` magic is a blank replacement, while a non-zero
  invalid record is still fatal;
- valid members supply the geometry; blanks fill the lowest missing slots, and a
  RAID10 array still requires one valid member per mirror pair;
- the rebuild is synchronous at boot: 64-sector blocks are copied from a healthy
  member of the same pair (the whole region for RAID1, only its pair's chunks
  for RAID10), flushed, then a fresh checksummed metadata record with a new
  member UUID is written and flushed;
- `scripts/raid-blank.sh` recreates the cold-spare failure by zeroing a member's
  partition while keeping its GPT; `make test-raid-repair` rebuilds one RAID1
  member and `make test-raid10-repair` rebuilds slots 1 and 3, then `e2fsck`
  validates the array from the repaired members.

Phase 3B-7 adds a polled AHCI/SATA driver:

- PCI class `01/06/01` discovery (QEMU q35 exposes the ICH9 controller at
  `00:1f.2`), 32-bit BAR5 sizing and an uncached mapping — for a BAR inside the
  4 GiB identity window the driver uses `vm_protect_page()` with `PCD|PWT`
  instead of `vm_map_page()`, because the identity PTE is already present;
- ports are capped by `CAP.NP` and by the mapped BAR window, and an empty port
  is given a 20 ms link window instead of the 1000 ms command timeout, which
  keeps the six-port q35 boot at ~185 ms instead of ~4.1 s;
- `IDENTIFY DEVICE` supplies 48-bit LBA capacity and the logical sector size,
  drives are registered as `sda`..`sdh`, and reads/writes go through an 8 KiB
  bounce buffer with `READ/WRITE DMA EXT` and `FLUSH CACHE EXT`;
- `storage_init()` now probes virtio-blk, NVMe and AHCI and mounts `crugroot`
  (or a RAID array) on any of them; `make test-ahci` boots with only a SATA
  image and runs the same JBD2, writable-ext4 and `e2fsck` gates.

Phase 3B-8 makes the on-disk format self-checking:

- ext4 metadata checksums (`metadata_csum`) are written for the superblock,
  group descriptors, block and inode bitmaps, inodes, directory tails and
  extent tails, with the seed from `s_checksum_seed` or the UUID;
- a freestanding CRC32C (Castagnoli) lives in `kernel/lib/crc32c.c`, generated
  by `tools/gen-crc32c.py` and covered by 629 host checks;
- the JBD2 journal is checksummed end to end: checksum v3 tags, a checksummed
  journal superblock, a descriptor tail checksum and a commit block checksum,
  while legacy/v2 journals still mount;
- transactions span multiple descriptors, so a large mkdir/unlink batch is one
  atomic commit instead of a failure; replay validates the whole transaction
  before writing anything and fails closed on a checksum mismatch;
- a dirty journal in the test image is replayed at boot: the RO mount is
  refused, the kernel mounts read-write, prints `CRUGOS: journal replayed N
  block(s) at mount` and verifies the file contents;
- `make test-journal-csum` proves `e2fsck` replays our checksummed journal and
  `make test-journal-replay` proves the kernel replays it on the next boot;
  `make test` still requires a clean `e2fsck -fn` on the resulting image.

Phase 3B-9 adds encryption at the block layer:

- `crypt.device` is a GPT partition with type GUID
  `8f1c7d2e-3b45-4c6a-9e01-c2a1b7f0d9e3` named `crugcrypt`: a 16 KiB header
  with eight 256-byte keyslots followed by an AES-256-XTS data area, where the
  data-relative 512-byte sector number is the XTS tweak;
- the passphrase is stretched with Argon2id (RFC 9106) into a 96-byte
  key-encryption/MAC key pair that unwraps a random 64-byte master key held in
  a keyslot; each slot is authenticated with a keyed BLAKE2b-256 MAC over the
  header and slot, and a recovery passphrase can be sealed as a second slot;
- AES-256-XTS uses the P1619 XEX construction, AES-NI when the CPU advertises
  it and a table-driven software fallback otherwise; XTS is pinned to IEEE
  1619 vector 10 and Argon2id to libargon2 vectors;
- the UEFI loader copies `\crugos\cmdline.txt` into `boot_info`, the kernel
  consumes `crypt.pass=` and wipes the loader page immediately, and the boot
  report prints only `cmdline: <n> bytes (redacted)`;
- `storage_init()` prefers `crugroot`, then unlocks a `crugcrypt` partition
  (`CRUGOS: crypt ready ...` and `CRUGOS: crypt aes: ...`), then falls back to
  RAID; before the final marker `storage_lock()` wipes the passphrase, the
  volume keys and the bounce buffer, and the crypt device refuses further I/O;
- host tools `tools/mkcrypt.c` (`format`, `extract`), a `crypt` mode of
  `scripts/mkdisk.sh`, `check-disk.py --crypt-tool`, and five gates: healthy
  unlock, recovery keyslot, wrong passphrase, tampered MAC and AES-NI. See
  [crypto.md](docs/crypto.md).

Phase 3B-10 makes the encrypted root usable without storing the secret:

- when the ESP carries no `crypt.pass=` token, the kernel prints
  `CRUGOS: crypt passphrase required` and then `passphrase: `, echoes one
  `*` per accepted byte and gives the operator up to three attempts;
- the reusable line editor in `kernel/lib/lineedit.c` handles Enter,
  backspace, Ctrl-C/Esc cancel, UTF-8 passphrases and overflow, and is covered
  by host tests; every accepted line is zeroed after the unlock attempt;
- `crypt.prompt_timeout=<seconds>` (1..3600, default 60, per attempt) keeps a
  headless machine from hanging; a rejected line prints
  `passphrase rejected (N attempt(s) left)`, three of them print
  `crypt locked (too many attempts)`, and the boot exits with status 35;
- `scripts/run-crypt-prompt.py` connects to QEMU's `-serial tcp:` console
  (`SERIAL_TCP`), types the answers and asserts that the passphrase never
  appears in the serial log; `make test-crypt-prompt`,
  `test-crypt-prompt-wrong` and `test-crypt-prompt-timeout` prove the
  successful typed unlock, the three-attempt limit and the timeout;
- `crypt.pass=` remains the non-interactive automation path and never
  prompts. See [crypto.md](docs/crypto.md).

Phase 4A adds console input, so the passphrase prompt no longer needs a serial
line:

- a polled i8042/PS-2 keyboard driver (`kernel/arch/x86_64/keyboard.c`) enables
  the controller, turns on scancode translation, masks IRQ1/IRQ12 and drains
  the output buffer; a missing controller is reported, not fatal;
- `input.device` (`kernel/input/input.c`) decodes set-1 scancodes into a
  64-byte queue: make/break and `0xE0` prefixes, Shift/Ctrl/Alt/Caps Lock
  (Caps XOR Shift), letters, digits and punctuation, Enter/Backspace/Tab/Escape,
  and Ctrl chords as control bytes;
- `console_try_getc()` drains the serial line first and only then polls the
  keyboard, so `crypt.pass=` automation and the typed prompt share one path, and
  a waiting prompt sleeps in `sti; hlt` (`cpu_wait_for_irq()`) instead of
  spinning, so it burns no host core;
- the boot prints `CRUGOS: input ready (serial + keyboard)` (or
  `(serial only)`) after the heap; the passphrase prompt now says
  `on the console (serial or keyboard)`;
- `scripts/run-crypt-keyboard.py` boots with an empty `crypt.pass=`, adds a QMP
  socket through `QEMU_EXTRA`, and injects one `send-key` per character into
  the emulated keyboard; `make test-crypt-keyboard` and
  `test-crypt-keyboard-wrong` prove a keyboard unlock and the three-attempt
  lockout without any display;
- `make crypt-run` builds the encrypted image with `CRYPT_PASS=` and opens a QEMU
  window, so the passphrase can be typed on the host keyboard and watched on the
  framebuffer. See [input.md](docs/input.md).

Phase 4B adds networking up to a DHCP-configured IPv4 host:

- a device-neutral `net.device` layer (`include/crug/net.h`,
  `kernel/net/net.c`) with a device registry, static frame buffers, big-endian
  wire helpers, an 8-entry ARP cache and four UDP port bindings;
- a polled legacy virtio-net PCI driver (`drivers/net/virtio_net.c`, device
  `0x1AF4:0x1000`) that negotiates only `VIRTIO_NET_F_MAC`, pre-posts 32 receive
  buffers of 2 KiB on queue 0, transmits through one 2 KiB bounce buffer on
  queue 1 with a 500 ms completion wait, and probes up to four
  `eth0`..`eth3` interfaces;
- Ethernet framing (60-byte padding, MAC/broadcast filter), ARP request/reply and
  resolution with retries, IPv4 with header checksum validation and no
  fragmentation, ICMP echo, and UDP with a pseudo-header checksum on transmit;
- a DHCP client (`kernel/net/dhcp.c`) that runs DISCOVER/OFFER/REQUEST/ACK for
  up to three attempts, requests mask/router/DNS, and fills the kernel
  `net_config`;
- `net_init()` runs after `input_init()` and prints
  `CRUGOS: net ready (N device(s))`; `net_selftest()` leases an address, resolves
  the gateway over ARP and pings it, printing `dhcp lease`, `arp resolved`,
  `icmp echo reply` and `net selftest ok`, or skipping cleanly when no NIC is
  present;
- `make test-net` boots with QEMU user-mode networking
  (`NET_MODE=user`, slirp: guest `10.0.2.15/24`, gateway `10.0.2.2`) and
  `scripts/boot-check.py` requires the lease, ARP and ICMP markers; the host
  suite covers wire helpers, RFC 1071 checksums and DHCP option parsing.
  See [network.md](docs/network.md).

Phase 4C adds TCP and a socket path on top of that stack:

- a small in-order transport (`kernel/net/tcp.c`: four sockets, 1400-byte
  segments, 4 KiB receive rings, bounded retransmission) with connect,
  listen/accept, send, receive and close;
- `tcp_selftest()` runs after the ICMP check: it listens on port 8080, fetches
  `GET / HTTP/1.0` from a host responder named by the loader command line
  (`net.tcp=<port>`), repeats the exchange through the socket syscalls, and
  serves one request itself (`net.serve=<seconds>` bounds the wait);
- six socket syscalls (numbers 9-14: `CONNECT`, `SEND`, `RECV`, `CLOSE`,
  `LISTEN`, `ACCEPT`) hand sockets to the current process as typed handles;
- `make test-tcp` runs `scripts/run-net-tcp.py`, which answers the guest over
  slirp's host loopback and fetches the guest listener through a QEMU host
  forward, so both directions are exercised end to end before `e2fsck` checks
  the disk. See [network.md](docs/network.md).

Phase 4D adds name resolution on top of that stack:

- a caching stub resolver (`kernel/net/dns.c`): a UDP query to `net.dns=` (or
  the DHCP server address) and a TCP retry with the two-byte length prefix when
  the answer is truncated or the UDP leg times out; compression pointers must
  walk strictly backwards, up to four CNAME aliases are followed, and an
  eight-entry TTL cache answers repeats;
- `dns_selftest()` runs after the TCP checks and is skipped unless the loader
  command line carries `net.name=<host>`; `net.dns=<ip>[:port]` picks the
  server and `net.dns-tcp=1` forces the stream transport;
- `SYS_RESOLVE` (15) resolves a name with a 3000 ms budget and returns the
  first address to the caller;
- `make test-net-dns` runs `scripts/run-net-dns.py`, a hermetic DNS-over-TCP
  responder on the host loopback that slirp reaches as `10.0.2.2`, then checks
  the disk with `e2fsck`. See [network.md](docs/network.md).

Phase 5A adds a third input source, a USB keyboard behind xHCI:

- a polled xHCI host controller (`drivers/usb/xhci.c`): PCI class `0C/03/30`, a
  mapped register window, `HCRST`, one 4 KiB command, event, EP0 and interrupt
  ring each with a Link TRB wrap, the device context array and an input context;
- enumeration of the first boot-protocol keyboard: `ENABLE_SLOT`,
  `ADDRESS_DEVICE`, `GET_DESCRIPTOR` for the device and the full configuration,
  `EVALUATE_CONTEXT` for the real packet size, `SET_CONFIGURATION`,
  `CONFIGURE_ENDPOINT` and one armed Normal TRB per eight-byte report;
- `drivers/usb/usb_hid.c` decodes the boot report (letters with Shift XOR Caps,
  digits, punctuation, Enter, Backspace, Tab, Escape, Space, the same Ctrl
  folding as the PS/2 decoder) and hands characters to `input_char()`, so the
  USB keyboard shares `input.device` with the serial line and i8042;
- port hotplug is handled by acknowledging the `PORTSC` change bits, resetting
  and enumerating a new device, and unbinding the keyboard when its port drops;
- `usb.wait=<seconds>` and `usb.keys=<n>` bound the optional boot check;
  `make test-usb-hotplug` boots with `usb=off,i8042=off` so the USB keyboard is
  the only one, plugs `usb-kbd` into `qemu-xhci` over QMP while the guest runs,
  types `abc` and requires the `usb` markers, a clean `e2fsck` and exit 33. See
  [usb.md](docs/usb.md).

Phase 5B adds a pointer next to that keyboard:

- the driver keeps a table of up to four boot keyboards and mice, so every port
  that reports a connect is enumerated with its own device context, interrupt
  ring and report buffer, in either order and at the same time;
- `usb_hid_parse_config()` accepts protocol 2 (mouse) as well as protocol 1
  (keyboard) and reports the endpoint's kind; `usb_hid_mouse_decode()` turns the
  three- or four-byte boot report (button bitmap, signed `dx`, signed `dy`,
  optional wheel) into motion and button edges, with the first report after a
  reset acting as the button baseline;
- `input.device` gains a pointer state beside its byte queue:
  `input_pointer_event()` accumulates the deltas and button changes,
  `input_pointer_read()` drains them, and the console path is untouched, so a
  later graphics or shell consumer can read the pointer without changing the
  keyboard path;
- port scans sweep every port instead of stopping at the first device, and a
  bound device whose port drops is unbound with `CRUGOS: usb keyboard gone` or
  `CRUGOS: usb mouse gone`;
- `usb.mouse=<seconds>` and `usb.moves=<n>` bound the mouse boot check;
  `make test-usb-mouse` boots with `usb=off,i8042=off`, plugs `usb-mouse` into
  `qemu-xhci` over QMP, injects relative motion and a button press, and requires
  the `usb mouse` markers, a clean `e2fsck` and exit 33. See
  [usb.md](docs/usb.md).

Phase 5C adds sound: a PCM ring, a mixer and a USB audio function.

- `kernel/audio/audio.c` is `audio.device`: a 16384-frame (341 ms) ring of
  interleaved 16-bit stereo. `audio_write()` appends and counts what did not
  fit, `audio_read()` pops, runs the frames through the mixer and zero-fills the
  rest of the buffer, so the hardware always gets a complete packet. Playback
  and capture totals are fed back by the driver, and `audio_tone()` generates a
  test tone from a 360-entry Q15 sine table;
- `kernel/audio/mixer.c` keeps a volume and a mute flag per channel. Volume is
  in 1/256 dB from `AUDIO_VOLUME_MIN` (-30 dB) to 0 dB and becomes a Q15 gain
  through a 31-entry table, rounded to the nearest whole dB, with mute winning;
- `drivers/usb/usb_audio.c` is attached by the xHCI driver when interface 0 of
  the configuration has class `0x01`. It parses UAC1 and UAC2 descriptors
  (feature unit, streaming interface, format type I, endpoint), selects
  alternate setting 1, sets the sampling frequency and the hardware volume, and
  treats a refused request as non-fatal;
- the xHCI driver gained a small isochronous API (`usb_control()`,
  `usb_isoc_open()`/`submit()`/`buf()`/`done()`/`errors()`/`close()`) with the
  endpoint interval encoded in microframes and `TRB_SIA` set, without which the
  packets never leave frame 0;
- the playback path keeps 32 packets (32 ms) in flight, fills each one from
  `audio_read()` and only counts successful completions when refilling, so a
  ring underrun cannot wrap the window and stall the stream;
- `audio.rate=`, `audio.volume=` and `audio.seconds=` bound the boot check, and
  `make test-audio` boots with `qemu-xhci` plus `usb-audio` on an
  `-audiodev none` backend, then requires the audio markers, a clean `e2fsck`
  and exit 33. See [audio.md](docs/audio.md).

Phase 5D gives the pointer state a consumer: a cursor on the framebuffer.

- `kernel/arch/x86_64/fb.c` owns a 12x19 sprite. `fb_cursor_show_at()` saves the
  pixels it covers, draws the white outline and amber body, and every text path
  (`fb_putc()`, `fb_clear()`, `fb_scroll()`) restores, writes and recomposites,
  so text never eats the cursor and a scrolling console never smears it;
- `gfx.device` (`kernel/gfx/gfx.c`) drains `input_pointer_read()`, moves the
  cursor by the raw delta with `GFX_CURSOR_STEP`, clamps it to the screen, tracks
  button transitions and prints one `CRUGOS: gfx cursor at (X, Y)` marker per
  move when a gate asked for motion. `gfx.wait=` and `gfx.moves=` bound the boot
  check, and the self-test is silent without them, so no existing gate changed;
- `make test-gfx` boots `usb=off,i8042=off` with `qemu-xhci`, hotplugs a
  `usb-mouse` over QMP, injects relative motion, and compares QEMU screendumps:
  the amber sprite centroid must move by the delta the guest reported (2.0 pixel
  tolerance), which also pins the framebuffer pixel packing. See
  [gfx.md](docs/gfx.md).

Phase 6A starts the Workbench: a small software window manager in the kernel.

- `intuition.device` (`kernel/gfx/intuition.c`) owns four window slots, a
  stacking order and one active window. A left press hit-tests top down, raises
  and activates the window, and a press on the title bar starts a drag with the
  grab offset; drags are clamped into the screen, and the release prints the
  final position and counts the drag. The geometry and the palette live in
  `include/crug/intuition.h`;
- windows are drawn with the framebuffer primitives added in this phase
  (`fb_fill_rect()`, `fb_frame_rect()`, `fb_draw_text()`, `fb_set_text_enabled()`):
  a white frame, a 20-pixel title bar that is amber on the active window and
  grey on the others, and 8x16 body text on a 20-pixel line pitch. Every state
  change repaints the whole desktop;
- `kernel/gfx/workbench.c` is the bring-up resident: with `wb.enable=1` it takes
  the framebuffer away from the text console (the serial port keeps every
  diagnostic), opens two demo windows, installs the `gfx.device` pointer hook and
  an `input.device` poll hook that pumps the USB stack, and `wb_selftest()` waits
  for `wb.drag=` title-bar drags within `wb.wait=` seconds. Without the token no
  boot changes at all;
- phase 6B adds the Intuition menus and gadgets: `intuition_set_menus()` gives a
  window a strip menu bar (the first window carries `Workbench` and `Window`, the
  second `Shell` and `Workbench`), a press on a strip opens its popup below it,
  a hover highlights the row under the pointer and a press on a row selects it;
  `intuition_add_gadget()` places a body-relative rectangle such as the `Run` and
  `Stop` gadgets of the first window inside that body; and the 12x12 close
  gadget at the right end of every title bar closes its window. Every
  interaction queues a typed `struct intuition_event` (`ACTIVATE`, `CLOSE`,
  `GADGET`, `MENU`, `DRAG`) that `intuition_event_poll()` drains; the queue holds
  16 events and counts the ones it had to drop. `wb.menu=1 wb.acts=<n>` make the
  resident drive a scripted sequence and print one `CRUGOS: wb action N ...`
  line per event;
- `make test-workbench` boots `usb=off,i8042=off` with `qemu-xhci`, hotplugs a
  `usb-mouse`, drags window 2 by its title bar with QMP motion events and reads a
  `screendump`: the active title bar must be at the new origin and the old one
  must be idle, so a window that logs a move without drawing it fails. See
  [workbench.md](docs/workbench.md).
- `make test-workbench-menu` drives the 6B desktop the same way: it opens the
  `Shell` menu, checks the highlighted row pixel by pixel, selects an item,
  clicks the `Run` gadget, closes window 2 through its close gadget, opens the
  `Workbench` menu and selects `Quit`, then requires four action lines, the menu
  markers and a 33 exit status;
- `make demo` boots that same desktop in a real QEMU window instead of a
  headless gate: `DEMO_QEMU_EXTRA` plugs a `usb-kbd` and a `usb-mouse` into the
  xHCI controller, `DEMO_DISPLAY` (GTK) opens the window, and the command line
  carries `wb.enable=1 wb.wait=0 wb.hold=<seconds>`, so the desktop stays on
  screen and both input devices are live until the hold expires (Ctrl+Alt+G
  releases the pointer). See [build.md](docs/build.md).

The kernel also detects AVX2, enables XSAVE state and, by default, refuses to
boot without AVX2. Self-tests run at boot, then the kernel prints a boot report
to a 16550 serial port and the framebuffer and exits QEMU deterministically for
automated testing.

Verified in this tree:

```
CrugOS 0.24.0 (phase 6b) -- Amiga-inspired microkernel bring-up
CRUGOS: hello
CRUGOS: version 0.24.0
pmm    : 118520 pages (462 MiB), free 118519, bitmap 131072 B
CRUGOS: pmm ready
cpu    : AuthenticAMD family 15 model 107 stepping 1 nx=yes apic=yes avx2=yes required=yes
vm     : 4 GiB identity map, W^X kernel sections, guard=0x000000000018f000
CRUGOS: vm ready
cpu    : smep=no smap=no tsc_deadline=no
CRUGOS: cpu ready
timer  : 2687896922 Hz (pit), mode=lapic-periodic, 100 Hz tick
CRUGOS: timer ready
CRUGOS: heap ready
CRUGOS: input ready (serial + keyboard)
CRUGOS: usb controller absent
CRUGOS: audio device absent
virtio : net eth0 mac 52:54:00:12:34:56
CRUGOS: net ready (1 device(s))
virtio : blk vda, 32768 sectors, io=0x6000
CRUGOS: virtio ready (1 device(s))
CRUGOS: nvme ready (0 device(s))
ahci   : sda 1032192 sectors, port 0
CRUGOS: ahci ready (1 device(s))
CRUGOS: block ready (2 device(s))
storage: no GPT on sda
gpt    : 1 partition(s), root 'crugroot' LBA 2048..26623
CRUGOS: gpt ready
CRUGOS: root device vda1
CRUGOS: ext4 ready
CRUGOS: journal ready
CRUGOS: ext4 /hello.txt: hello from ext4
CRUGOS: storage ready
sched  : per-class run queues ready, boot thread id 0, idle id 1
CRUGOS: sched ready
CRUGOS: syscall ready
trap   : breakpoint handled at rip=0x0000000000100e2b
CRUGOS: journal committed 1 block(s) in 1 descriptor(s)
CRUGOS: journal selftest ok (1 blocks recovered)
CRUGOS: writable storage ok
CRUGOS: dhcp lease 10.0.2.15/255.255.255.0 gw 10.0.2.2 dns 10.0.2.3
CRUGOS: arp resolved 10.0.2.2 52:55:0a:00:02:02
CRUGOS: icmp echo reply from 10.0.2.2 seq=1 ttl=255
CRUGOS: tcp listen ready (port 8080)
CRUGOS: tcp connect ok (10.0.2.2:52847)
CRUGOS: tcp http ok (53 bytes)
CRUGOS: tcp socket syscall ok
CRUGOS: tcp http served (72 bytes)
CRUGOS: net selftest ok
CRUGOS: gfx ready (1024x768, cursor at 512,384)
CRUGOS: selftest ok
kernel : 0x0000000000100000 size=720 KiB
boot   : cpu=0 acpi_rsdp=0x1f77e000 fdt=0x0 initrd=0x0
memory : 36 regions, usable 474080 KiB (462 MiB)
pmm    : total=118520 free=118462 bitmap=131072 B
fb     : 1024x768 stride=1024 format=BGRX8888 base=0x80000000
cmdline: 28 bytes (redacted)
entropy: ca02033c50b53413
wx     : audit ok, identity map 0..4 GiB, guard unmapped
heap   : total=57344 B used=8976 B slab_pages=12 large_pages=2
sched  : switches=112
boot   : pmm=28552 us cpu=438 us vm=626 us traps=360 us timer=65357 us heap=60 us storage=94487 us sched=354 us syscall=122 us total=190359 us
timer  : 2687896922 Hz (pit), ticks=31, mono=458247353 ns
CRUGOS: phase 6b boot ok
```

This excerpt is the `make test-tcp` boot. The guest is both a TCP client and a
server: it leases `10.0.2.15/24` from QEMU's user-mode (slirp) DHCP server,
resolves the gateway `10.0.2.2` over ARP, echoes ICMP, then fetches an HTTP page
from a host responder and answers a host fetch of its own listener, so the log
shows `tcp listen ready`, `tcp connect ok`, `tcp http ok`, `tcp socket syscall
ok` and `tcp http served`. The fake MAC comes from QEMU; a real NIC uses its
own. `storage: no GPT on sda` is expected: `sda` is the firmware's ESP disk and
the test volume is the virtio disk `vda`. Argon2id dominates the encrypted gates
instead (`m=4096 KiB, t=3`, about 230 ms of storage stage), and the unencrypted
gates such as this one boot in about 190 ms.

The DNS gate (`make test-net-dns`) swaps the HTTP exchange for a name
resolution boot (the guest resolves over TCP, so the TCP lines are absent and
only the resolver lines appear):

```
CRUGOS: dns server 10.0.2.2:60901 (tcp)
CRUGOS: dns resolved crugos.test -> 10.0.2.99 via tcp (ttl 60)
CRUGOS: dns cache hit crugos.test -> 10.0.2.99
CRUGOS: dns socket syscall ok
CRUGOS: dns selftest ok
```

The USB gate (`make test-usb-hotplug`) boots with `usb=off,i8042=off` and a
`qemu-xhci` controller, plugs a keyboard into it over QMP while the guest is
running, types `abc`, and only then lets the boot finish:

```
CRUGOS: input ready (serial only)
CRUGOS: usb controller BAR assigned at 0xffb00000
CRUGOS: usb controller ready (xhci, 8 port(s))
CRUGOS: usb device 0627:0001 on port 5 (speed 3)
CRUGOS: usb keyboard ready (endpoint 0x81, 8 byte reports)
CRUGOS: usb key 'a'
CRUGOS: usb key 'b'
CRUGOS: usb key 'c'
CRUGOS: usb selftest ok (3 key(s))
```

The mouse gate (`make test-usb-mouse`) is the same machine and QMP setup with a
pointer instead of a keyboard, so the guest decodes motion and a button edge:

```
CRUGOS: input ready (serial only)
CRUGOS: usb controller ready (xhci, 8 port(s))
CRUGOS: usb device 0627:0001 on port 5 (speed 3)
CRUGOS: usb mouse ready (endpoint 0x81, 4 byte reports)
CRUGOS: usb mouse move dx=3 dy=0
CRUGOS: usb mouse button 0x01
CRUGOS: usb selftest ok (0 key(s), 2 pointer event(s))
```

The audio gate (`make test-audio`) boots the same `qemu-xhci` machine with a
`usb-audio` function and an `-audiodev none` backend, so no host sound server is
involved, and streams half a second of a 440 Hz tone through the mixer:

```
CRUGOS: usb device 46f4:0002 on port 5 (speed 1)
CRUGOS: usb audio attached (uac1, 2 ch, 16-bit, 48000 Hz, iface 1 alt 1)
CRUGOS: audio device ready (usb uac1, 2 ch, 16-bit, 48000 Hz)
CRUGOS: audio volume unsupported
CRUGOS: audio volume (ch0 -2560/256 dB, ch1 -2560/256 dB, mute 0)
CRUGOS: audio stream started (48000 Hz, 2 ch)
CRUGOS: audio streamed 24144 frame(s) in 498 ms
CRUGOS: audio capture unavailable (no input endpoint)
CRUGOS: audio selftest ok
```

`CRUGOS: audio volume unsupported` and a failing sampling-frequency request are
expected from QEMU's `usb-audio`: its volume handler never accepts a channel
selector, and it only understands class interface *out* requests. The software
mixer is the source of truth, so both are non-fatal. Capture is parsed and
streamed by the driver too, but this QEMU build only exposes playback, which is
why the gate accepts the "capture unavailable" marker.

The graphics gate (`make test-gfx`) uses the same machine and QMP setup to move
the mouse and prove the sprite really moves in the framebuffer:

```
CRUGOS: input ready (serial only)
CRUGOS: gfx ready (1024x768, cursor at 512,384)
CRUGOS: usb controller ready (xhci, 8 port(s))
CRUGOS: usb device 0627:0001 on port 5 (speed 3)
CRUGOS: usb mouse ready (endpoint 0x81, 4 byte reports)
CRUGOS: gfx cursor shown at (512, 384)
CRUGOS: gfx cursor at (572, 424)
CRUGOS: gfx cursor at (547, 414)
CRUGOS: gfx cursor at (647, 344)
CRUGOS: gfx selftest ok (3 move(s), buttons 0x00)
```

Between those markers the gate takes `screendump` images and checks that the
amber sprite centroid moved by `+60,+40`, `-25,-10` and `+100,-70` pixels, so a
cursor that reports coordinates without drawing them fails. The self-test lingers
for 1.5 s after the last move so the final dump cannot race the QEMU exit.

The Workbench gate (`make test-workbench`) uses the same machine and QMP setup,
but drags a window instead of a sprite:

```
CRUGOS: input ready (serial only)
CRUGOS: gfx ready (1024x768, cursor at 512,384)
CRUGOS: wb window 1 opened (80, 90, 360x220)
CRUGOS: wb window 2 opened (480, 300, 320x200)
CRUGOS: wb desktop ready (1024x768, 2 window(s))
CRUGOS: usb mouse ready (endpoint 0x81, 4 byte reports)
CRUGOS: usb mouse move dx=0 dy=-72
CRUGOS: wb cursor at (512, 312)
CRUGOS: usb mouse button 0x01
CRUGOS: wb window 2 drag start (512, 312)
CRUGOS: wb window 2 active
CRUGOS: usb mouse move dx=45 dy=25
CRUGOS: wb cursor at (557, 337)
CRUGOS: wb window 2 drag to (525, 325)
CRUGOS: usb mouse move dx=45 dy=25
CRUGOS: wb cursor at (602, 362)
CRUGOS: wb window 2 drag to (570, 350)
CRUGOS: usb mouse button 0x00
CRUGOS: wb window 2 moved to (570, 350)
CRUGOS: wb selftest ok (1 drag(s))
```

Between those markers the gate checks the `screendump` pixels: the amber title
bar of window 2 sits at `(570, 350)`, the old bar position is idle grey, and the
white frame of window 1 is unchanged. The drag offset is `(32, 12)` (the grab
point inside the title bar), so the window moves less than the pointer.

`make demo` shows the same desktop interactively: it boots with a pre-attached
USB keyboard and mouse into a GTK window and holds the framebuffer with
`wb.hold=`, so the two windows, their title bars and the amber pointer are on
screen instead of being verified by a gate. The keyboard types on the serial
console and the mouse drives the pointer, so a title bar can be dragged by hand;
`build/x86_64/serial.log` records the `CRUGOS: usb mouse move` lines and the
`CRUGOS: wb holding desktop for <seconds> s` line, and the guest still exits
with status 33 when the hold ends. `DEMO_HOLD_SEC=0` plus
`DEMO_DISPLAY=none` turns it back into a fast headless boot.

With no xHCI function in the default machine the driver prints
`CRUGOS: usb controller absent` (as in the excerpt above) and the PS/2 keyboard
stays in charge; the USB gates are the only ones that install the controller and
remove the PS/2 one, so the input can only arrive over USB.

`make iso` turns the ESP tree into `build/x86_64/crugos.iso`: an ISO 9660 image
with an El Torito no-emulation entry, whose FAT16 boot image is also reachable
through the protective MBR's single type-`0xEF` partition, so the file boots
both as a CD-ROM and after a plain `dd` onto a USB stick. The image carries its
own command line (`ISO_CMDLINE`, default `storage=none`), so a stick never
inherits the QEMU `crypt.pass=` token and the kernel prints
`CRUGOS: storage skipped (storage=none)` instead of demanding a root filesystem.
`make check-iso` re-parses the artifact (volume descriptor, El Torito catalog
checksum, FAT16 BPB, MBR entry, an `xorriso -osirrox` extraction verified with
`fsck.vfat`), and `make test-iso` boots the ISO through `usb-storage` on
`qemu-xhci` and again as a CD-ROM, both ending in status 33. `make usb
USB_DEV=/dev/sdX USB_CONFIRM=yes` writes it to real media. See
[iso.md](docs/iso.md).

## Quick start

```
make preflight   # crugc, GNU ld, QEMU, OVMF, python3
make             # show this help (the target list)
make all         # build kernel.elf, BOOTX64.EFI and the ESP tree
make test        # boot under QEMU/OVMF and verify the boot contract
make test-raid   # boot a healthy two-member RAID1 root and check both disks
make test-raid10 # boot a healthy four-member RAID10 root and check all disks
make test-nvme   # boot from an NVMe namespace and check the image
make test-ahci   # boot from a SATA disk and check the image
make test-raid-degraded  # boot RAID1 with one member missing
make test-raid10-degraded  # boot RAID10 with one member per mirror pair
make test-raid-repair  # rebuild a blank RAID1 member at boot, then check
make test-raid10-repair  # rebuild blank RAID10 slots 1 and 3 at boot
make test-journal-csum  # crash after a journal commit, replay it with e2fsck
make test-journal-replay  # crash, then replay the journal in the kernel
make test-crypt  # boot from an AES-256-XTS encrypted root and check it
make test-crypt-recovery  # unlock the second (recovery) keyslot
make test-crypt-wrongkey  # a wrong passphrase must fail closed
make test-crypt-tamper  # a corrupted keyslot MAC must be rejected
make test-crypt-ni  # the same encrypted boot with AES-NI
make test-crypt-prompt  # type the passphrase on the serial console
make test-crypt-prompt-wrong  # three wrong lines must lock the volume
make test-crypt-prompt-timeout  # an unanswered prompt must fail closed
make test-crypt-keyboard  # type the passphrase on the PS/2 keyboard
make test-crypt-keyboard-wrong  # three wrong keyboard lines must lock
make crypt-run   # boot the encrypted disk in a QEMU window and type it
make run         # boot and keep the serial log for inspection
make demo        # boot the Workbench desktop in a real QEMU window
make test-net    # boot with a virtio-net NIC and verify DHCP/ARP/ICMP
make test-tcp    # TCP client/server plus an HTTP exchange against a host peer
make test-net-dns  # resolve a name against a host DNS server over TCP
make test-usb-hotplug  # hotplug a USB keyboard into xHCI and type at it
make test-usb-mouse    # hotplug a USB mouse into xHCI and move the pointer
make test-audio        # stream PCM to a USB Audio Class device through the mixer
make test-gfx          # move a hotplugged USB mouse and verify the framebuffer cursor
make test-workbench    # drag a window on the software-rendered desktop
make test-workbench-menu  # open menus, select items and click gadgets
make iso         # build a bootable hybrid UEFI ISO (build/x86_64/crugos.iso)
make check-iso   # validate the ISO's El Torito, FAT ESP, MBR and file set
make test-iso    # boot that ISO from a USB stick and from a CD-ROM
make usb         # write the ISO to a USB block device (USB_DEV=/dev/sdX)
make test-compat # boot a non-AVX2 build on a non-AVX2 CPU model
make test-host   # host unit tests (string, printf, ELF, bitmap, VM math, handles, CRC32C, AES/XTS/Argon2id/BLAKE2b, crypt slots, line editor, scancodes, USB HID keyboard and mouse, pointer state, IPv4 checksums, DHCP parsing, DNS, audio mixer/tone/ring, framebuffer cursor, intuition windows/menus/gadgets)
make check-disk  # e2fsck + persisted-file checks on the latest test disk
make disk        # generate the GPT/ext4 test disk
make raid-disks  # generate the two RAID1 member disks
make raid10-disks  # generate the four RAID10 member disks
make nvme-disk   # generate the GPT/ext4 image used by test-nvme
make ahci-disk   # generate the GPT/ext4 image used by test-ahci
make crypt-disk  # generate the encrypted test image (mkcrypt + mkdisk)
make mkcrypt     # build the host tool that formats/extracts crypt volumes
make version     # print the release version from VERSION
make options     # print every build/QEMU/storage option and its value
make size        # kernel budget: <1 MiB stretch, <2 MiB hard
make check-efi   # PE32+ structure checks on the loader
make repro       # two builds must be byte-identical
```

Build artifacts land in `build/x86_64/`; `make run` records
`build/x86_64/serial.log`, `debug.log` and `exit-code.txt`. Every knob has a
default in the makefile; `make options` prints the full resolved set. Useful
variables include `MEMORY_MB`, `SMP`, `TIMEOUT_SEC`, `QEMU_CPU`, `QEMU_ACCEL`,
`QEMU_DISPLAY`, `QEMU_EXTRA`, `OVMF_CODE`, `OVMF_VARS`, `REQUIRE_AVX2`,
`EXPECT_AVX2`, `EXPECT_RAID`, `EXPECT_NVME`, `EXPECT_REBUILD`, `EXPECT_AHCI`,
`EXPECT_CRYPT`, `EXPECT_AES`, `EXPECT_NET`, `EXPECT_TCP`, `EXPECT_DNS`,
`EXPECT_USB`, `EXPECT_MOUSE`, `EXPECT_AUDIO`, `EXPECT_GFX`, `EXPECT_WB`,
`EXPECT_MENU`,
`AUDIO_RUN_TIMEOUT_SEC`, `AUDIO_QEMU_EXTRA`, `GFX_WAIT`, `GFX_MOVES`,
`GFX_RUN_TIMEOUT_SEC`, `GFX_QEMU_EXTRA`, `WB_WAIT`, `WB_DRAG`,
`WB_RUN_TIMEOUT_SEC`, `WB_QEMU_EXTRA`, `WB_MENU_WAIT`, `WB_MENU_ACTS`,
`WB_MENU_RUN_TIMEOUT_SEC`, `DEMO_DISPLAY`, `DEMO_HOLD_SEC`,
`DEMO_TIMEOUT_SEC`, `DEMO_CMDLINE`, `DEMO_MACHINE_EXTRA`, `DEMO_QEMU_EXTRA`,
`ISO_IMAGE`, `ISO_STAGE`, `ISO_FAT_IMG`, `ISO_SIZE_MB`, `ISO_VOLID`,
`ISO_CMDLINE`, `ISO_DATE`, `ISO_RUN_TIMEOUT_SEC`, `EXPECT_ISO`, `ISO_IMG`,
`USB_IMG`, `USB_STICK`, `USB_DEV`, `USB_CONFIRM`,
`NET_MODE`, `NET_HOSTFWD`, `TCP_SERVE_SEC`,
`TCP_RUN_TIMEOUT_SEC`, `DNS_NAME`, `DNS_ADDRESS`, `DNS_TTL`,
`DNS_RUN_TIMEOUT_SEC`, `USB_WAIT`, `USB_KEYS`, `USB_RUN_TIMEOUT_SEC`,
`MOUSE_MOVES`, `MOUSE_WAIT`, `MOUSE_RUN_TIMEOUT_SEC`, `MACHINE_EXTRA`,
`NVME_IMG`, `AHCI_IMG`,
`CRYPT_IMG`,
`CRYPT_TAMPER_IMG`, `CRYPT_PASS`, `CRYPT_RECOVERY_PASS`, `CRYPT_M`,
`CRYPT_PROMPT_TIMEOUT_SEC`, `CRYPT_PROMPT_WRONG_PASS`, `CRYPT_RUN_DISPLAY`,
`CRYPT_RUN_TIMEOUT_SEC`, `CMDLINE_EXTRA`,
`SERIAL_TCP`, `MCRYPT`, `HOST_CC` and `MODE`; see [docs/build.md](docs/build.md).

## Repository layout

| Path | Contents |
| --- | --- |
| `boot/efi/` | UEFI loader: file I/O, ELF loader, GOP, memory map, entropy, handoff |
| `include/crug/` | shared kernel/loader headers (types, BootInfo, I/O, string, heap, threads, IPC, syscalls) |
| `kernel/arch/x86_64/` | boot code, linker script, port I/O, serial, i8042 keyboard, framebuffer, CPU/IDT/APIC/timer, context switch |
| `kernel/core/` | kernel entry, console, printf, panic, self-tests, scheduler, handles, MsgPort, syscall dispatcher, boot report |
| `kernel/lib/` | freestanding string, formatting, bitmap and console line-editor routines |
| `kernel/input/` | `input.device`: set-1 scancode decoder, the console byte queue and the pointer state |
| `kernel/audio/` | `audio.device` PCM ring, the per-channel mixer and the tone generator |
| `kernel/net/` | `net.device`, Ethernet/ARP/IPv4/ICMP/UDP, TCP, the DHCP client and the DNS resolver |
| `kernel/gfx/` | `gfx.device` (the pointer-driven framebuffer cursor) and `intuition.device` with the phase 6A desktop and its phase 6B menus, gadgets and event queue |
| `kernel/mm/` | physical allocator, W^X page tables, kernel heap |
| `kernel/block/` | block-device registry, partitions, bounds checks, RAID1/RAID10, boot-time rebuild and the AES-256-XTS `crypt.device` |
| `kernel/fs/` | GPT, writable checksummed ext4, checksummed JBD2 journal and the storage boot policy (including the console passphrase prompt) |
| `kernel/crypto/` | AES, XTS, AES-NI, BLAKE2b and Argon2id (shared with the host tools) |
| `drivers/` | PCI, legacy virtio-blk, NVMe and AHCI/SATA block device drivers, legacy virtio-net, and the xHCI controller with its USB HID keyboard, mouse and Audio Class decoders (audio frames flow into `kernel/audio/`) |
| `scripts/` | QEMU runner, disk/RAID1/RAID10/crypt image builders, the hybrid ISO builder (`mkiso.sh`, `mkfat.py`), boot/PE/disk/RAID/journal/crypt/ISO checks, the serial and keyboard prompt drivers, tamper helper, size and reproducibility gates |
| `tests/host/` | host unit tests for freestanding code |
| `tools/` | host-side development tools (`gen-font.py`, `mkcrypt.c`, `gen-crc32c.py`) |
| `docs/` | [build](docs/build.md), [boot](docs/boot.md), [storage](docs/storage.md), [checksums](docs/checksums.md), [crypto](docs/crypto.md), [input](docs/input.md), [network](docs/network.md),
[NVMe](docs/nvme.md), [AHCI/SATA](docs/ahci.md), [RAID1/RAID10](docs/raid.md), [USB/xHCI](docs/usb.md), [audio](docs/audio.md), [graphics](docs/gfx.md), [Workbench](docs/workbench.md), [bootable ISO](docs/iso.md), [cpu/AVX2](docs/cpu.md), [versioning](docs/versioning.md), [roadmap](docs/roadmap.md) |

## Versioning

The release number lives in [`VERSION`](VERSION) and follows Semantic
Versioning. The build stamps it into the kernel and loader
(`-DCRUG_VERSION_STR`), the kernel prints `CRUGOS: version <version>`, and
`make test` fails if the guest version differs from `VERSION`. Releases are
annotated `vX.Y.Z` Git tags; `BOOTINFO_VERSION` is versioned separately. See
[docs/versioning.md](docs/versioning.md) for the release checklist and ABI
compatibility rules.

## Boot contract

`include/crug/bootinfo.h` defines `boot_info`: magic `0x43524731`, version 1,
the kernel extent, the normalized memory map, framebuffer mode, ACPI RSDP,
optional FDT/initrd, boot CPU id, a 64-byte entropy seed, the loader's TSC, the optional command line (the `crypt.pass=` automation token) and, when that token is absent, an interactive console passphrase prompt (serial line or PS/2 keyboard).
See [docs/boot.md](docs/boot.md) for the full handoff ABI and exit protocol.

## Toolchain notes

- `crugc` 0.2.3 (GCC 16.2.0) is a native x86_64 compiler. There is no
  `x86_64-crugos` cross target; the same host compiler builds the kernel with
  `-fcrug-kernel` (defines `__CRUG_KERNEL__`, disables `go`/`chan`/`select` and
  rejects ownership/borrow syntax).
- The UEFI loader must be compiled with `-fno-ident` and linked with
  `ld -m i386pep --disable-reloc-section`; every firmware entry point uses the
  Microsoft ABI (`ms_abi`). Details and failure modes are in
  [docs/build.md](docs/build.md).
- QEMU uses its `vvfat` driver as the ESP, so no FAT image or `mtools` is
  needed.
- AVX2 is detected and its XSAVE state enabled. The default build panics if the
  CPU lacks AVX2; `REQUIRE_AVX2=0` builds a compatibility kernel. Do not use
  `-cpu max` or `-cpu Haswell` with this OVMF; see [docs/cpu.md](docs/cpu.md).

## Roadmap

Phases 0, 1, 2, 3A and 3B-1 through 3B-10 are done. Phase 3A covered the block
layer, legacy virtio-blk, GPT and read-only EXT4; phase 3B-1 added the writable
EXT4 path and an on-disk consistency gate; phase 3B-2 added JBD2 journal replay
and ordered metadata journaling; phase 3B-3 added a RAID1 root with healthy and
degraded boots; phase 3B-4 added a polled NVMe root path; phase 3B-5 added
RAID10 with healthy four-member and degraded two-member boots; phase 3B-6 added
boot-time rebuild of a blank RAID1/RAID10 member; phase 3B-7 added a polled
AHCI/SATA driver and a SATA-root boot gate; phase 3B-8 added ext4 metadata and
JBD2 journal checksums plus multi-descriptor transactions; phase 3B-9 added an
AES-256-XTS encrypted root with Argon2id keyslots, a recovery passphrase,
loader-supplied passphrase handling and key zeroization; phase 3B-10 added the
masked console passphrase prompt with retries and a per-attempt timeout; phase
4A added a polled PS/2 keyboard, `input.device` and a console read path shared
with the serial line; phase 4B brought up a polled legacy virtio-net driver and
a small IPv4 stack (`net.device`: Ethernet, ARP, IPv4, ICMP echo, UDP) with a
DHCP client and a boot self-test that leases, resolves and pings the gateway;
phase 4C added a minimal TCP transport with a client, a listener, an HTTP
exchange in both directions and six socket syscalls, gated end to end by a host
peer over slirp and a QEMU host forward; phase 4D added a caching DNS stub
resolver with a UDP query, a TCP fallback, CNAME chains, a TTL cache and the
`SYS_RESOLVE` syscall, gated against a hermetic host DNS server; phase 5A added
a polled xHCI host controller and a USB boot keyboard that feeds the same input
queue and can be hotplugged into the running guest; phase 5B widened that
driver to a device table of boot keyboards and mice and gave `input.device` a
pointer state driven by a boot-mouse decoder; phase 5C added `audio.device`, a
per-channel mixer and a USB Audio Class driver that streams isochronous packets
from a PCM ring, gated with a hermetic QEMU audio function; phase 5D added
`gfx.device`, a 12x19 cursor the framebuffer console saves and recomposites
around its text, gated by comparing QEMU screendumps against the guest's own
coordinates; phase 6A added `intuition.device`, a software window manager of
four stacked, draggable windows on that framebuffer, and the gated two-window
desktop bring-up; phase 6B added strip menus with popup selection, body gadgets,
the close gadget and the typed event queue the shell will consume. Remaining
phase 3B work is
online RAID rebuild, hot spare, scrub and a write-intent bitmap, RAID below
encryption, a redundant ESP, TPM/keyfile keyslots and USB storage; phase 4
continues with framebuffer (GOP) console input and virtual terminals, plus a
routing table and richer resolver features on top of the phase 4B-4D stack;
the next step is Workbench userspace: a shell window and console scrollback on
top of the phase 6B desktop. Later phases cover
USB storage, graphics, Workbench userspace, SMP/IOMMU and virtualisation. See
[docs/roadmap.md](docs/roadmap.md) for the full sequence and the known
specification gaps (Crug syntax, cross-architecture compilers and the OVMF
minimum RAM floor).

## License

See [LICENSE](LICENSE).