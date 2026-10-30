# CrugOS

CrugOS is a small 64-bit operating system written in Crug,
a C dialect with Rust-style ownership and Go-style concurrency. It targets the
feel of AmigaOS: a compact monolithic kernel, message-passing userspace, a
Workbench shell and applications that are self-contained bundles.

The design contract lives in
[specs/CrugOS_complete_kernel_spec_v3.md](specs/CrugOS_complete_kernel_spec_v3.md).

## Status: phase 8A virtio-gpu scanout (`v0.46.0`)

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
inside a window body, the close gadget and a typed event queue. Phase 6C
turns the second window into a live shell -- a console text grid with
scrollback, a line editor with command history, and a command set that lists,
reads and writes the mounted ext4 root -- on top of that desktop. Phase 6D
makes every window resizable from a 12x12 corner grip and gives the keyboard
a navigation role: Tab cycles the focus, the arrow keys move the active window
and Enter opens its menu for the arrows to drive. Phase 6E puts desktop objects
under the windows and a depth gadget in every title bar: a 64x48 icon is
selected by a press and the `System:` volume opens a drawer on a double click,
while the depth box sends its window to the bottom of the stacking order.
Phase 6F makes those icons interactive: a press grabs and drags a tile, a
send-to-front gadget joined the depth gadget in every title bar, and the icon
table takes the keyboard, and phase 6G turns the `System:` drawer into a
scrolling list: the arrow keys move the viewport, `r` re-reads the volume, a
scrollbar tracks the position and a resize reflows the rows. Phase 6H makes
those rows selectable and the scrollbar draggable through a generic Intuition
body-input hook that hands a window's own presses, motions and releases to a
handler. Release `v0.23.1`
made that tree bootable media: `make iso` writes a hybrid UEFI ISO
that boots from a USB stick or a CD-ROM, with no disk image and no test
tokens baked in. Phase H1 hardened the boot path against crafted images, phase
H2 hardened the kernel heap with address-keyed page cookies, validated headers
and a read-only integrity checker, and phase H3 gives every fault the same
bounded report, classifies it for a kernel or a user frame, and delivers the
four stack-destroying vectors on IST stacks. Phase H4 takes the storage mount
path to the host: it mutates real disk images and mounts every mutant in a
forked child, 22000 cases per gate. Phase U1 crosses into ring 3: a
seven-entry GDT, a fixed 512 GiB to 1 TiB user window, an ELF loader that maps
`/bin` programs with W^X permissions, a copy boundary on every syscall pointer,
and a trap path that kills only the faulting user thread. Phase U2 gives every
ring-3 thread its own PML4: the window is no longer a shared range in the
kernel's tables, two programs run at once with the same virtual address pointing
at different frames, and `make test-user2` is the 49th gate. Phase U3 gives ring
3 the filesystem: `SYS_OPEN`, `SYS_READ`, `SYS_WRITE`, `SYS_CLOSE` and
`SYS_LISTDIR`, a per-thread descriptor table, an `argc`/`argv` block on the
initial stack, and `/bin/sh`, which reads `/etc/sh.rc` from the disk image and
writes `/shout.txt` back; `make test-user3` is the 50th gate. Phase U4 adds
spawn, exec and wait: a ring-3 program starts another program image as a
thread and waits for its exit code, and `SYS_EXEC` replaces the calling image
in place while keeping its tid; `make test-user4` is the 51st gate. Phase U5
adds fork: `SYS_FORK` copies the caller's whole user window into a fresh address
space and resumes the child immediately after the `int 0x80`, so the child sees
its own pages rather than the parent's, and `make test-user5` is the 52nd gate.
Phase U6 makes that fork copy-on-write: the parent's writable pages are shared
with the child read-only and copied on the first store, so a fork of 300 pages
costs a handful of frames, and `make test-user6` is the 53rd gate. Phase 6I
gives the Workbench three retained screens with a strip in the menu bar,
per-screen windows and desktop icons, and a window move between screens;
`make test-workbench-screens` is the 54th gate. Phase 6J adds the Commodities
Exchange: a fixed registry of four global hotkey utilities (`Volume up`,
`Volume down`, `Mute` and `Clock`), an `Exchange` manager window whose rows
toggle them, and function keys decoded from both the PS/2 and the USB HID
keyboard, with `make test-workbench-cx` as the 55th gate. Phase 7A adds
application bundles: `/Applications/<Name>.app` directories with a bounded
`manifest.toml`, a boot-time registry that validates them, a Workbench window
that launches one and a shell builtin that lists, inspects and runs them, with
`make test-app-bundles` as the 56th gate. Phase 7B adds capabilities and
signed packages: every ring-3 thread carries a capability mask, a policy file
in `/System` decides what a bundle may hold, a `.crpkg` container carries an
Ed25519 signature over a SHA-512 digest that a trust list must recognise, and
`make test-pkg`, `make test-caps` and `make test-workbench-pkg` are gates 57 to
59. Phase 7C gives the installer a lifecycle and closes the capability hole:
a newer package updates an installed one through a stage-and-swap with a
rollback backup, a package can be removed again, `SYS_SPAWN` and `SYS_EXEC`
now require `app.launch` and hand the child the caller's mask, and the shell
gained a `pkg` builtin. Phase 8A gives the kernel a display device: a
virtio-gpu-pci function on the modern virtio-pci transport creates one 2D
resource with contiguous backing pages, attaches it as the scanout, and
presents the desktop as damage rectangles, while the firmware framebuffer
remains the fallback; `make test-gpu` is the 60th gate.

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
- phase 6C puts a live shell in window 2: `kernel/lib/textgrid.c` keeps a
  96-line scrollback ring of 38-column lines and draws the visible eight rows
  with an inverse-video cursor; `kernel/shell/shell.c` is a line editor with
  Ctrl-U (clear), Ctrl-C (cancel) and Ctrl-P/Ctrl-N history, and its command
  table -- `help`, `ver`, `mem`, `uptime`, `ps`, `ls`, `cat`, `echo`, `write`,
  `sync`, `cls` -- runs against a `struct shell_io`, which
  `kernel/shell/shell_ops.c` binds to the mounted ext4 root, the heap counters
  and the scheduler; `wb.shell=1 wb.sh=<n>` install the painter on window 2 and
  wait for n typed commands;
- `make test-workbench-shell` hotplugs a `usb-kbd`, types `help`, `ls /`, `ver`
  and `echo hi` over QMP, requires one `CRUGOS: sh run '<line>'` line per
  command and the version string in the `ver` output, then compares two
  `screendump`s of the shell body: at least 200 pixels must change and the body
  must carry text-coloured pixels. See [workbench.md](docs/workbench.md);
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
- `make demo` boots the whole Workbench in a real QEMU window instead of a
  headless gate: `DEMO_QEMU_EXTRA` plugs a `usb-kbd` and a `usb-mouse` into the
  xHCI controller, `DEMO_DISPLAY` (GTK) opens the window, and the command line
  carries every desktop feature at once -- `gpu.enable=1 gpu.size=1024x768
  wb.enable=1 wb.wait=0 wb.desk=1 wb.dr=1 wb.dsel=1 wb.screens=1 wb.cx=1
  wb.shell=1 wb.fill=$(DEMO_FILL_FILES) wb.hold=<seconds>` -- so the desktop
  object and its drawer, both screens, the Commodities Exchange and a live Shell
  are on screen together until the hold expires (Ctrl+Alt+G releases the
  pointer). The window is the virtio-gpu 2D scanout, not firmware memory:
  `DEMO_GPU_QEMU_EXTRA` adds `-vga none -device virtio-gpu-pci,id=gpu0`, so the
  boot log starts with `CRUGOS: virtio-gpu ready (1024x768, 768 backing
  page(s), resource 1, modern pci)` and `CRUGOS: gpu scanout ok`; `DEMO_GPU=0`
  drops both and boots the software framebuffer instead. See
  [build.md](docs/build.md).

Phase 6D makes the windows resizable and gives the keyboard work of its own.

- every window carries a 12x12 `INTUITION_RESIZE_SIZE` grip in its inner
  bottom-right corner, at `(x + w - INTUITION_BORDER - 12 - 2, y + h -
  INTUITION_BORDER - 12 - 2)`; dragging it resizes the window live, clamped to
  the 120x80 minimum (`INTUITION_MIN_WIDTH`/`INTUITION_MIN_HEIGHT`) and to the
  screen, and a motion that would leave the screen is refused instead of
  clamped, so the size simply does not change. The release prints
  `CRUGOS: wb window %d resized to %ux%u`, increments
  `intuition_resize_count()`, counts one action and queues an
  `INTUITION_EVENT_RESIZE` (type 6) whose `struct intuition_event` carries the
  new `u32 w` / `u32 h`; a press and release without motion still counts;
- `intuition_key_event(u8)` consumes decoded key bytes: Tab cycles the focus
  through the stacking order, raising the window, activating it, queueing an
  `ACTIVATE` event and printing `CRUGOS: wb window %d focused (Tab)`; the four
  arrows move the active window by `INTUITION_KEY_STEP` (8) pixels, clamped to
  the screen; Enter opens the active window's first screen-top menu as a popup
  with item 0 highlighted, Down/Up move the highlight, Enter selects the
  highlighted item and Escape closes the popup;
- `include/crug/input.h` gains `INPUT_KEY_UP` 0x90, `INPUT_KEY_DOWN` 0x91,
  `INPUT_KEY_LEFT` 0x92 and `INPUT_KEY_RIGHT` 0x93; the PS/2 set-1 decoder
  pushes them for the E0 make codes 0x48/0x50/0x4B/0x4D and ignores E0 break
  codes and E0 modifier releases, and the USB HID decoder maps usage page 0x07
  usages 0x52/0x51/0x4F/0x50 (UpArrow, DownArrow, RightArrow, LeftArrow);
- `wb.keys=1` installs a keys mode that prints
  `CRUGOS: wb keys ready (window %d active)` after the desktop ready line
  (`wb.shell=1` wins if both are given, because the shell owns the keyboard);
  `wb.acts=` now counts resizes too, and `wb_drain_events()` prints
  `CRUGOS: wb action N resize window W (WxH)` for a `RESIZE` event;
- `make test-workbench-keys` plays the two halves in one boot: it hotplugs a
  `usb-mouse` and drags the Shell window's grip from (790,490) to (730,430), so
  the window shrinks from 320x200 to 260x140 and the old corner becomes desktop
  again, then hotplugs a `usb-kbd` and sends Tab, Right, Right, Enter, Down and
  Enter to focus window 1, move it to (96,90) and select the `Info` menu item.
  It requires a 33 exit status and expects `EXPECT_KEYS=yes` from
  `boot-check.py`; see [workbench.md](docs/workbench.md).

Phase 6E adds desktop objects and the depth gadget.

- `include/crug/intuition.h` + `kernel/gfx/intuition.c` gain a desktop object
  table of eight 64x48 tiles (`INTUITION_MAX_ICONS`,
  `INTUITION_ICON_WIDTH`/`HEIGHT`, `INTUITION_ICON_LABEL_GAP` 4), drawn on the
  desktop background under every window with the label under the tile and the
  kinds `INTUITION_ICON_VOLUME` (0), `_DRAWER` (1) and `_FILE` (2);
  `intuition_add_icon(x, y, label, kind)` clamps the tile so all 64x48 pixels
  stay on screen and fails without a framebuffer or a label;
- a press inside a tile selects it -- amber frame
  (`INTUITION_ICON_SELECTED_BORDER_RGB` `0x00FFB000`) and amber label -- queues
  `INTUITION_EVENT_ICON` (type 7) with the index and the tile origin, counts one
  action and prints `CRUGOS: wb icon '<label>' selected`. A press on the empty
  desktop clears the selection and a window drawn over a tile wins the press;
  the palette is `INTUITION_ICON_RGB` `0x00C0C0C0`, `INTUITION_ICON_INNER_RGB`
  `0x00081018` and `INTUITION_ICON_TEXT_RGB` `0x00E0E0E0`;
- the depth gadget is a 12x12 box (`INTUITION_DEPTH_SIZE`) four pixels left of
  the close box in every title bar, so a title bar is now a drag area, a depth
  box and a close box. A press sends that window to the bottom, makes the new
  top window active (its title bar turns amber), queues
  `INTUITION_EVENT_DEPTH` (type 8) followed by an `ACTIVATE` for the new top
  window, counts one action and prints `CRUGOS: wb window N sent to back`. A
  press on the depth box of the window already at the bottom is a no-op: no
  event, no count, no redraw;
- `intuition_set_paint(id, fn, ctx)` installs a painter for one window body,
  called with the body rectangle instead of the static body text, and gadgets
  are still drawn after it; `intuition_redraw_window(id)` repaints one window
  after its content changed. The drawer uses both;
- `kernel/gfx/workbench.c` reads `wb.desk=<0|1>`: with `wb.desk=1` it creates
  one volume object labelled `System:` at (16,240) and prints
  `CRUGOS: wb icon ready ('System:', volume)` after the desktop/shell/keys
  markers. A double click on the object within `WB_DOUBLE_CLICK_NS`
  (600000000 ns) opens a normal Intuition window titled `System:` at (560,60)
  360x260 whose painter lists the root directory entries from the mounted ext4
  root, one name per line, at most 10 lines, clipped to the body width, and
  prints `CRUGOS: wb drawer 'System:' opened (N entries)`; an empty or unmounted
  root prints `no volume`. `wb.desk=1` counts as a reporting mode, so
  `wb.acts=N` is honoured; the boot self-test accepts window drags OR
  menu/gadget/close/resize actions OR shell commands OR these desktop actions
  and prints `CRUGOS: wb desk selftest ok (N drawer(s))` before the usual
  `CRUGOS: wb selftest ok (N drag(s), M action(s))`;
- `make test-workbench-desk` is the desktop gate: it builds the ESP and disk,
  boots with `wb.enable=1 wb.wait=<wait> wb.desk=1 wb.acts=<n>`, and checks the
  idle icon, the amber selection, the drawer with five root entries, its amber
  title bar and dark body, and the depth gadget sending it back. See
  [workbench.md](docs/workbench.md).

Phase 6F makes the desktop objects interactive.

- `include/crug/intuition.h` + `kernel/gfx/intuition.c` gain
  a 4-pixel `INTUITION_ICON_DRAG_MIN` dead zone, `INTUITION_EVENT_ICON_MOVE` (type 9) and
  `INTUITION_EVENT_FRONT` (type 10), the counters `intuition_icon_moves()` and
  `intuition_front_count()` and `intuition_icon_key(u8)`. A press on a tile
  selects and grabs it (the grab offset is the cursor minus the tile origin); a
  pointer motion outside the dead zone moves the tile to the cursor minus that
  offset, clamped to
  `[0, screen width - 64]` x `[0, screen height - 48]`, and the release queues
  `ICON_MOVE` with the object index and the new origin, counts one icon move and
  one action and prints `CRUGOS: wb icon '<label>' dropped at (x, y)`; the tile
  stays selected, so its amber frame travels with it;
- the send-to-front gadget is a 12x12 box (`INTUITION_FRONT_SIZE`) at
  `close_box_x - 32`, twelve pixels left of the depth box: a press raises that
  window, makes it active, queues `INTUITION_EVENT_FRONT`, counts one front and
  one action and prints `CRUGOS: wb window N sent to front`. Pressing the gadget
  of the frontmost window is a no-op, with no event and no count. Every window
  repaint is now five frames (window, close box, depth box, front box, resize
  grip);
- keyboard icon focus: `kernel/gfx/workbench.c` reads `wb.ikeys=<0|1>` (parsed
  before `wb.desk=`, and still ignored when `wb.shell=1`). Tab, Right and Down
  move the selection forward and Left and Up move it backwards, both wrapping;
  Enter selects the first object when nothing is selected and on the next press
  activates it (the `System:` volume opens its drawer); Escape clears the
  selection. A selection change prints `CRUGOS: wb icon '<label>' focused`, and
  icon mode prints `CRUGOS: wb icons ready (N object(s), keyboard focus)` and
  `CRUGOS: wb icons selftest ok (N move(s), M front(s))`;
- `make test-workbench-icons` runs `scripts/run-workbench-icons.py`: it types
  Enter twice to focus and activate the `System:` volume, drags its tile from
  (16,240) to (168,376) and proves the move with a `screendump` (amber frame at
  (168,376), window body at (176,384), desktop colour at the old origin), then
  clicks window 1, presses its depth box at (410,102) to send it back (the title
  at (90,96) turns idle grey) and its front gadget at (394,102) to raise it
  again (the title turns amber). It requires exit status 33, prints `ok:
  workbench icon dragging, send-to-front and keyboard focus (exit 33)` and
  expects `EXPECT_ICONS=yes` from `boot-check.py`; see
  [workbench.md](docs/workbench.md);
- `make test-host` reports 3054 checks, with the new `test_intuition_icon_drag`,
  `test_intuition_front` and `test_intuition_icon_keys` groups and a 75-frame
  window-open assertion because one repaint now paints five frames;
- the kernel file is 230056 B (224 KiB) and the load image 749 KiB, against the
  1024 KiB stretch and 2048 KiB hard budgets, in a release of 41 gates.

Phase 6G makes the `System:` drawer a scrolling list.

- `kernel/gfx/workbench.c`: the drawer window body is a viewport over the root
  listing. The row count is `(h - 22) / 20 + 1` for a body `h` above 22 pixels
  and 0 below that, so the 360x260 drawer (236-pixel body) shows 11 rows and
  the same drawer resized to 460x360 (336-pixel body) shows 16, on a 20-pixel
  row pitch for 8x16 glyphs. Arrow Up and Arrow Down scroll one entry at a
  time, clamped to `[0, entries - rows]` so the list never scrolls past either
  end; `r` or `R` re-reads the directory listing (a file created after the
  drawer opened appears) and clamps the top row again. A 4-pixel scrollbar
  track sits at `x + w - 8` inside the body, with a thumb
  `max(12, track_height * rows / entries)` pixels tall whose offset follows the
  top entry, so the drawer shows both position and extent;
- `wb.dr=1` takes the arrow keys and `r` for the drawer while `wb.desk=1`
  still opens the desktop, and prints
  `CRUGOS: wb drawer mode (arrow keys scroll, r refreshes)`;
  `wb.fill=<0..24>` creates `/f00` .. `/f(N-1)` in the root once before the
  drawer opens (a missing or read-only volume skips it) and prints
  `CRUGOS: wb fill N file(s)`, because the scroll, refresh and reflow gate
  needs a listing longer than one screen. The drawer prints
  `CRUGOS: wb drawer ready (N entries, N row(s))`, a scroll prints
  `CRUGOS: wb drawer scrolled (top N of M)`, a refresh prints
  `CRUGOS: wb drawer refreshed (N entries)` and the drawer selftest line
  reports the scroll and refresh counts;
- the kernel mounts the ext4 root read-only at boot, so the fill and the
  shell's `write` command now open a write window: `storage_remount_rw()` and
  `storage_remount_ro()` (declared in `include/crug/storage.h`) umount and
  remount the root around a short write and print
  `CRUGOS: storage remounted read-write` and
  `CRUGOS: storage remounted read-only`. The shell's `write` command
  previously failed silently against the read-only boot mount and now works;
  `sync` returns success while the volume is read-only because every write
  already closed its remount;
- `make test-workbench-drawer` is the 42nd gate:
  `scripts/run-workbench-drawer.py` fills the root, double-clicks the
  `System:` volume icon, reads 29 entries (five root entries plus 24 filled
  files) and 11 rows, requires at least 25 entries and exactly 11 rows, presses
  Down three times and proves the move with more than 200 changed pixels in the
  drawer body, drags the resize grip from
  (904,304) to (1004,404) so the drawer grows to 460x360 and reflows to 16
  rows, proves the growth at (830,400) -- desktop (16,32,48) before, window
  body (8,16,24) after -- with at least 200 body-text (224,224,224) pixels
  inside the widened body, refreshes with `r`, and expects exit status 33 and
  a clean `e2fsck`. `boot-check.py` gained the `EXPECT_DRAWER` positional
  and its `ok` line reports `drawer yes`;
- `make test-host` reports 3075 checks, with the new `test_intuition_paint`
  group covering the body painter, `intuition_redraw_window()` and the resize
  reflow;
- docs: README, workbench, boot, build, roadmap, versioning, gfx and iso
  describe the scrolling drawer, the two tokens, the remount helpers and the
  gate.

Phase 6H makes the drawer rows selectable and its scrollbar draggable.

- `include/crug/intuition.h`: a generic body-input hook.
  `INTUITION_BODY_PRESS` (0), `INTUITION_BODY_MOVE` (1) and
  `INTUITION_BODY_RELEASE` (2) arrive in body-relative coordinates through
  `int intuition_set_body_input(int id, void (*fn)(int id, u32 kind, i32 x, i32 y, void *ctx), void *ctx)`;
  a NULL `fn` clears the handler and an unknown id returns -1.
  `kernel/gfx/intuition.c` gives the handler the press: with one installed it
  records an internal body window, forwards PRESS, repaints and returns before
  any title-bar drag, resize or gadget branch, so a body press never steers the
  window; while the button is held every pointer motion is forwarded as MOVE,
  and the release is forwarded as RELEASE before the icon, resize, gadget and
  drag branches run. `win_draw_body()` and the new `body_rect()` helper share
  one body rectangle, `x+BORDER, y+BORDER+TITLE_HEIGHT, w-2*BORDER,
  h-2*BORDER-TITLE_HEIGHT`;
- `kernel/gfx/workbench.c`: `wb.dsel=1` points that hook at the open drawer.
  The drawer is opened at (560,60,360x260), a 356x236 body over 29 entries (five
  root entries plus 24 from `wb.fill=`), 11 visible rows on a 20-pixel row
  pitch and a maximum top row of 18. A click at body `y` selects index
  `top + (y - 6) / 20` and paints an amber band
  (`INTUITION_MENU_HIGHLIGHT_RGB` `0x00FFB000`) with the text in the
  window-background colour, printing
  `CRUGOS: wb drawer selected '<name>' (index N of M)`. The 4-pixel scrollbar
  track (`WB_TRACK_W`) sits at body-local `x = body width - 8`, `y + 6`,
  `h - 12` tall; the thumb is `max(12, track * rows / entries)` pixels high
  and its offset follows the first visible row. Pressing the track jumps the
  view and grabs the thumb at the press point, dragging follows the pointer, and
  the release prints the final top row;
- select mode prints `CRUGOS: wb select mode (click a row, drag the scrollbar)`
  right after `CRUGOS: wb desktop ready`, the drawer prints
  `CRUGOS: wb drawer scrollbar grabbed` and
  `CRUGOS: wb drawer scrollbar to (top N of M)`, the self-test ends with
  `CRUGOS: wb select selftest ok (2 select(s), 1 scrollbar drag(s))` (or
  `CRUGOS: wb select timeout (N/M)` when the clicks do not arrive) and the boot
  marker is now `CRUGOS: phase 6h boot ok`. The phase 6G drawer markers
  (ready, scrolled, refreshed) are unchanged;
- `make test-workbench-select` is the 43rd gate:
  `scripts/run-workbench-select.py` boots
  `wb.enable=1 wb.desk=1 wb.dr=1 wb.dsel=1 wb.fill=24 wb.acts=3`, hotplugs a
  `usb-mouse` and double-clicks the `System:` desktop icon. A screendump must
  show row 0 in the window-background colour (8,16,24); a click on row 2 at
  (600,136) must paint the amber (255,176,0) band at (890,136) while row 0 stays
  dark, and the guest reports `2 of 29`; pressing the thumb at (911,100) and
  dragging to (911,280) makes the guest report top row 18 of 29 and clears row
  2's band, after which a click on the new row 0 at (600,96) -- index 18 --
  paints its band amber. The gate waits for the selftest line and requires exit
  status 33 and a clean `e2fsck`, expects `EXPECT_SELECT=yes`
  (`boot-check.py` prints `select yes`) and prints
  `ok: workbench drawer selection and scrollbar dragging (exit 33)`;
- `tests/host/test_intuition.c` gained `test_intuition_body_input`: it prints
  `- intuition body input` and covers the body-relative PRESS and MOVE
  coordinates (48,78 then 68,108 in a window at 100,100,300,300), no forwarding
  after the release, title-bar presses and the close gadget never reaching the
  handler, no stale interaction after a window closes, NULL clearing the handler
  and `intuition_set_body_input(99, ...) == -1`; `make test-host` reports 3104
  checks;
- docs: README, workbench, boot, build, roadmap, versioning, gfx and iso
  describe the selectable rows, the draggable scrollbar, the body-input hook,
  the `wb.dsel=` token and the gate.

Phase H1 is the first hardening pass: the boot path now validates the bytes an
attacker can put there.

- `include/crug/harden.h` and `kernel/lib/harden.c`: seven pure validators,
  each host-tested in `tests/host/test_harden.c`. `harden_range_ok()` bounds
  a register-supplied buffer against the 4 GiB identity map (`len == 0` is
  fine, `addr == 0` and `len > limit` are rejected, otherwise
  `addr <= limit - len` so the addition cannot wrap);
  `harden_payload_ok()` rejects a NULL payload with a non-zero length;
  `harden_extent_header_ok()` checks magic `0xF30A`, entries, depth and the
  real node size (60 bytes for the inline root in `i_block`,
  `block_size - 12 - csum` for a leaf); `harden_ext4_geometry_ok()` requires
  a power-of-two block size of at least 1024, non-zero counts and
  `s_blocks_per_group` / `s_inodes_per_group` at most `block_size * 8`;
  `harden_nvme_doorbell_ok()` keeps a doorbell offset inside BAR0;
  `harden_kdf_cost_ok()` bounds the Argon2id cost tuple; and
  `harden_volume_ok()` keeps a crypt volume inside its parent partition;
- `kernel/core/syscall.c`: `SYS_PORT_SEND` validates the `rdx`/`r10` buffer
  and `SYS_PORT_RECV` validates the same buffer plus a non-zero `rsi` as an
  8-byte writable range, both against `HARDEN_IDENTITY_LIMIT` and both
  returning the new `SYS_EFAULT` (`-14`); `msgport_send()` rejects
  `data == 0` with `len > 0`, closing the uninitialised-heap leak. The
  self-test prints `CRUGOS: syscall selftest failed (pointer gate)` on
  failure;
- `kernel/fs/ext4.c`: the superblock geometry is checked before the
  `metadata_csum` checksum (`CRUGOS: ext4 mount rejected (geometry)`) and the
  root inline extent after `read_inode()` (`CRUGOS: ext4 mount rejected (root
  extent)`), so a crafted image has to be self-consistent to reach the
  validator; `extent_map()`/`extent_add()` validate every header against the
  bytes the node owns, and `free_block()` re-checks the bit index;
- `drivers/block/nvme.c`: a `CAP.DSTRD` that puts a doorbell past
  `bar_size` is refused at probe (`nvme   : doorbell stride outside BAR0`)
  instead of writing up to ~192 KiB past the mapping;
- `kernel/block/crypt.c`: unlock enforces an Argon2id policy of
  1024..65536 KiB, `t` 2..8, lanes 1..16 and `m >= 8 * lanes` on top of the
  format ceiling (1 GiB / `t` 64, `mkcrypt` unchanged), and a header whose
  volume exceeds its partition is refused; both refusals wipe the 16 KiB
  header copy;
- `kernel/lib/string.c`: `secure_zero()` wipes through volatile stores so the
  compiler cannot elide the wipe; the crypt header/bounce/scratch/master paths
  and `storage_wipe()` use it, and `boot/efi/main.c` zeroes the loader's
  `cmdline.txt` pool copy so `crypt.pass=` does not survive in USABLE memory;
- `make test-hardening` is the 44th gate: `scripts/run-hardening.py` patches
  one field per case (geometry-bpg, geometry-ipg, root-extent, kdf-high,
  kdf-low, volume-huge), boots the crafted image with
  `HARDEN_TIMEOUT_SEC=120` and requires the rejection marker and exit 35 --
  never a panic -- while the control boots to exit 33;
- `make test-host` reports 3200 checks; the kernel is 239840 B (234 KiB) on
  disk and the load image 778018 B (759 KiB); all 44 gates pass.

Phase H2 hardens the kernel heap itself: every structure the allocator walks is
validated before it is trusted, and the kernel can be asked whether the heap is
still consistent.

- `kernel/mm/heap.c` and `include/crug/heap.h`: every slab page and every large
  block header starts with a `u64` cookie keyed by the page address
  (`HEAP_COOKIE_KEY 0x435255474f5348ull` XOR the page XOR `SLAB_COOKIE_SALT`
  `0x534c4142ull` or `LARGE_COOKIE_SALT` `0x4c415247ull`), so a header forged at
  the wrong address is rejected;
- `struct slab_header` keeps its 64-byte footprint but is now checked field by
  field: the cookie, the class index, the object counts, the used bitmap (no bit
  past `objects`), every free-list entry (inside the page and unallocated), the
  listed count against `free_count` and a cycle cap of `objects`. `kmalloc`
  panics with `heap: corrupt slab header` or `heap: corrupt slab free list`
  before following a bad pointer;
- large blocks form a doubly linked list (`struct large_header` with `cookie`,
  `size`, `pages`, `next`, `prev`) whose links and fields are validated;
  `large_contains()` detects a pointer inside a block, so `free()` of an
  interior large pointer panics with `heap: free of interior large pointer`;
- `heap_check()` walks every slab class and the large list and returns one of
  nine `HEAP_CHECK_*` codes (`heap_check_name()` prints `ok`, `slab cookie`, ...),
  cross-checking `g_slab_pages`, `g_large_pages` and `g_large_bytes` against the
  live structures; the walk masks interrupts with
  `cpu_irq_save()`/`cpu_irq_restore()`;
- `heap_stress(ops)` churns 55% allocations and 45% frees over up to 64 live
  objects of 1..8000 bytes, tags and verifies every object, checks the heap
  every 64 operations and ends with an exact `heap_used_bytes()` accounting
  check (capped by `HEAP_STRESS_MAX` 200000); `heap_guard()` plants a bad
  free-list link, a zeroed slab cookie and a zeroed large-block cookie, proves
  `heap_check()` reports codes 5, 1 and 6, then repairs them;
- `heap_cmdline()` parses `heap.stress=<1..200000>` and `heap.guard=1` right
  after `heap_init()` in `kernel/core/main.c`; `heap_selftest()` prints
  `CRUGOS: heap check ok (6 slab(s), 8 object(s), 1 large block(s))` and also
  requires a freed 16-byte object to be reused at the same address;
- `include/crug/cpu.h` gains a `CRUG_HOST_TEST` branch (`cpu_irq_save()` returns
  `(1ull << 9)`, restore/disable are no-ops) so `kernel/mm/heap.c` compiles into
  `tests/host`; `tests/host/test_heap.c` supplies a fake 2048-page 8 MiB `.bss`
  arena with its own bitmap and mirrors the two kernel structs to corrupt real
  metadata, covering every check code, the large-block free path,
  `heap_stress(2000)`, `heap_guard()` and every `heap_check_name()` string. The
  `pmm_alloc_pages`/`pmm_free_pages` and `kzalloc` stubs moved out of
  `test_argon2.c` and `test_handle.c` into the new test, and `make test-host`
  reports 3235 checks;
- `make test-heap` is the 45th gate: it boots
  `heap.stress=$(HEAP_STRESS_OPS) heap.guard=1` under a 240-second budget,
  requires the stress, guard and check markers and a clean `e2fsck`; the kernel
  is 244264 B (238 KiB) on disk and the load image 783106 B (764 KiB); all 45
  gates pass.

Phase H3 replaces the per-vector panic one-liner with a trap policy: one report
shape for every fault, an explicit kernel-versus-user classification, and four
IST stacks so the handler still runs when the fault destroyed the stack.

- `include/crug/trap.h` keeps the `trap_frame` layout and adds
  `TRAP_IST_DF`/`TRAP_IST_NMI`/`TRAP_IST_MC`/`TRAP_IST_DB` (1 to 4),
  `enum trap_class { TRAP_HANDLED, TRAP_USER, TRAP_FATAL }`,
  `trap_classify(vector, cs)` and `trap_ist_for(vector)`;
- `kernel/lib/trapfmt.c` is the pure formatter (`trap_format()`, no
  `kprintf`, no serial, no framebuffer): it writes at most 1024 bytes as
  `CRUGOS: trap report <name> vector=<dec> error=0x<hex> ist=<dec> from=<kernel|user>`,
  the saved `rip`/`cs`/`rflags`/`rsp`/`ss` line, a `#PF`-only fault line
  (`present`/`write`/`user`/`reserved`/`instruction`, then `cr2`) and two
  register lines, all lowercase and unpadded;
- `kernel/arch/x86_64/idt.c`: `page_fault_panic()` is gone. Every vector
  except the timer (32), the syscall gate (128) and `#BP` (still handled and
  counted in `g_int3_seen`) builds a report, prints it and classifies:
  `#BP` is handled in both rings, `#OF` is handled only for a user frame, the
  user-killable set {0, 1, 6, 11, 12, 13, 14, 16, 17, 19, 21} is `TRAP_USER`
  when `cs & 3 == 3` and fatal otherwise, and everything else is fatal; a
  `TRAP_USER` fault panics as `panic("user-mode <name>")` until ring 3 exists;
- `kernel/arch/x86_64/linker.ld`/`cpu.c`: `__df_stack` is 16 KiB (was 8 KiB)
  and `__nmi_stack`/`__mc_stack`/`__db_stack` are 8 KiB each, all in `.bss`
  after the boot stack (+32 KiB of bss) and loaded into `ist1`..`ist4`;
  `rsp0` still points at the boot stack because the kernel never returns to
  CPL3 yet;
- `kernel/core/trap_test.c`: `trap.test=<recipe>` (de, ud, gp, pf, df, nmi,
  mc, db) injects exactly one fault as the last action before
  `arch_exit_qemu(0x10)`, so a control boot exits 33 and a hostile one 35;
  `trap_cmdline()` runs right after `heap_cmdline()`;
- `make test-trap` is the 46th gate: `scripts/run-trap.py` boots the control
  plus eight recipes from per-case disks under `build/x86_64/trap/` and
  requires the report vector/IST fragments, `CRUGOS PANIC: <name>` and
  `CRUGOS: panic` with exit 35, while `TRAP_TIMEOUT_SEC` (60) bounds each boot;
- `tests/host/test_trap.c` asserts every report shape, the classification
  matrix and the IST routing on the host; `make test-host` reports 3400
  checks, the kernel is 253144 B (247 KiB) on disk and the load image 821986 B
  (802 KiB), and all 46 gates pass.

Phase H4 attacks the mount path from the host instead of from QEMU: the kernel
image, the `BootInfo` v1 ABI and every boot path are unchanged, and the release
adds a fuzzer, its own link line and one more gate.

- `tests/host/fuzz_mount.c` (about 890 lines) links the real `kernel/fs/ext4.c`,
  `kernel/fs/gpt.c`, `kernel/fs/journal.c`, `kernel/block/block.c`,
  `kernel/block/crypt_meta.c`, `kernel/mm/heap.c`, the `kernel/lib/*` helpers and
  the `kernel/crypto/*` primitives against host stubs for `kprintf()` (silent
  unless `--verbose-kernel`), `panic()` (exit 97) and the `pmm_*` allocator (a
  32 MiB static arena), and serves the image through a bounds-checked in-memory
  block device; it builds as a separate `tests/host/build/fuzz-mount` binary
  because it installs its own stubs and must not share the unit-test link line;
- it finds the `crugroot` partition with the real `gpt_parse()` -- callers test
  `< 0`, because the function returns the partition count -- and weights six
  mutation regions: superblock 22, group descriptors 14, inode table 16, root
  directory block 16, a 64 KiB metadata window 17 and the whole partition 15;
- each case forks, applies one to four mutations from eight kinds (one-byte XOR,
  `0xFF` fill, zero fill, `u16`/`u32`/`u64` write, a 16-byte splice, a 16-byte
  random fill) with edge-weighted values, runs `gpt_parse()` -> `ext4_mount()`
  (25% read-write) -> lookup/list/read -> the write path (create, write,
  truncate, fsync, rename, unlink, mkdir/rmdir) -> `ext4_umount()`, and encodes
  how far it got in its exit code (0 rejected, 1 mounted, 2 walked a path,
  3 wrote);
- a signal, the panic code 97 or an exit code above 3 is a finding: the plan is
  printed and the mutant image dumped under `build/x86_64/fuzz/`, and
  `--selftest` forks a child that dereferences null so a green gate proves the
  detector works; a pristine-image control mount runs before the first case;
- the crypt leg mutates the 256-byte `struct crug_crypt_header` and calls
  `crypt_meta_validate()`/`crypt_meta_scratch_bytes()` -- the on-disk header
  parser, not the Argon2id unlock;
- `make test-fuzz` is the 47th gate: it rebuilds `disk.img` and `crypt.img`,
  runs the self-test and then 20000 ext4 cases plus 2000 header cases with
  `FUZZ_SEED` 1337 and `FUZZ_TIMEOUT_SEC` 10, in about 23 seconds and without a
  QEMU boot, so it prints no kernel version line; it reports about 10580 cases
  mounted, 10535 that walked a path, 2594 that wrote and 0 crashes, four seeds
  agree, and [docs/fuzzing.md](docs/fuzzing.md) records both that result and the
  fault classes the harness cannot reach;
- `make test-host` still reports 3400 checks, the kernel is still 253144 B
  (247 KiB) on disk and 821986 B (802 KiB) loaded, and all 47 gates pass.

Phase U1 adds the first ring-3 program: a seven-entry GDT with a DPL3 code/data
pair, a fixed user window in the address space, an ELF loader that maps `/bin`
programs with W^X permissions, a copy boundary on every syscall pointer and a
fault path that kills only the offending user thread.

- `include/crug/segment.h`/`kernel/arch/x86_64/cpu.c` grow the GDT from five to
  seven entries: kernel code/data `0x08`/`0x10`, the 64-bit TSS at `0x18`, and
  ring-3 code/data `0x2B`/`0x33` (`SEG_USER_CS`/`SEG_USER_DS`); the TSS keeps the
  phase H3 IST stacks, and `rsp0` is no longer the boot stack:
  `cpu_set_kernel_stack()` is its only writer and `schedule()` installs the
  incoming thread's kernel stack top, so a trap from CPL3 lands on a valid stack;
- `include/crug/vm.h` defines the user window at `USER_BASE` 512 GiB to
  `USER_TOP` 1 TiB (PML4 slot 1), outside the supervisor-only identity map, and
  `kernel/mm/vm.c` adds `vm_user_map_page`/`vm_user_map_pages`/
  `vm_user_unmap_pages`, which create every intermediate table with `VM_USER`,
  strip `VM_GLOBAL` and take W^X from the caller (`VM_NX` on data pages);
- `kernel/exec/elf_exec.c` reads `/bin/<name>` with `ext4_read_file` (512 KiB
  cap), parses it with the same `boot/efi/elf.c` the UEFI loader uses, reserves
  each `PT_LOAD` with the right permissions, copies the bytes through
  `pmm_phys_to_virt` so SMAP cannot trip the loader, maps a four-page stack just
  below `USER_STACK_TOP` with an unmapped page under it, and hands the result to
  `thread_create_user()`; empty segments are ignored and the entry point and
  stack pointer go through `harden_user_entry_ok`;
- `kernel/mm/uaccess.c` is the only way the kernel touches user memory: a pure
  `harden_user_range_ok` window check, a page-table walk that reports
  `UACCESS_EFAULT` instead of taking a kernel `#PF`, and `stac`/`clac` around the
  actual copy when CPUID exposes SMAP;
- `kernel/core/syscall.c` serves both rings: `from_user()` tests `frame->cs & 3`,
  CPL0 callers keep the identity-map path so the in-kernel selftests are
  unchanged, and CPL3 callers go through the bounce-buffer copies. `SYS_WRITE`
  (16) writes fd 1/2 from up to 4096 bytes in 256-byte chunks, `SYS_MAX` is 17,
  `SYS_EBUSY` (-16) is new, and `SYS_EXIT` from CPL3 ends only that thread;
- `kernel/arch/x86_64/user.S`/`kernel/core/thread.c`: a `THREAD_FLAG_USER` thread
  carries `user_rip`/`user_rsp` and up to 8 mapped regions; `thread_create_user()`
  refuses a second live user thread with `-EBUSY`, `thread_destroy()` unmaps the
  recorded pages, and `thread_user_start` builds the `iretq` frame (`SS 0x33`,
  user `RSP`, `RFLAGS 0x202`, `CS 0x2B`, entry `RIP`);
- `kernel/arch/x86_64/idt.c`: a fault from CPL3 prints the report and
  `CRUGOS: user fault <name> at rip=... err=... tid N`, then
  `CRUGOS: user fault killed tid N code -<vector>` and kills only that thread via
  `thread_kill_current()`; user `#DF`/`#NMI`/`#MC` and every CPL0 fault still
  panic, and `TRAP_USER` is no longer a "user-mode <name>" panic;
- `user/` holds three freestanding programs (`hello`, `badptr`, `fault`) linked
  by `user/user.ld` at `0x8000010000` with separate R+X text and R+W data
  segments; `user/lib.h` is inline `int $0x80` wrappers and `make user` builds
  them into `build/x86_64/user/`; `scripts/mkdisk.sh` reads `USER_DIR` from the
  environment and writes them into `/bin`;
- `kernel/core/user_test.c` parses `user.test=1` from the boot command line and
  runs the three programs in sequence, and `make test-user` is the 48th gate: it
  rebuilds the disk with the programs, boots with the token under
  `USER_RUN_TIMEOUT_SEC` (240), and passes boot-check the new final `EXPECT_USER`
  argument, which fails any gate that prints a user marker without it;
- `tests/host/test_uaccess.c` and `test_exec.c` cover the window, segment and
  entry validators, `uaccess_check` and the ELF segment policy; `make test-host`
  reports 3470 checks, the kernel is 262760 B (256 KiB) on disk and 831006 B
  (811 KiB) loaded, and all 48 gates pass.

Phase U2 gives every ring-3 thread its own page-table root: the 512 GiB window
is no longer a shared range in the kernel's tables, two programs run at once with
the same virtual address pointing at different frames, and `make test-user2` is
the 49th gate.

- `include/crug/vm.h` adds `struct vm_space` (`pml4`, `phys`, `user_pages`) and
  the `vm_space_create`/`vm_space_destroy`/`vm_space_map_user_pages`/
  `vm_space_unmap_user_pages`/`vm_space_translate`/`vm_space_activate` API plus
  `vm_space_set_kernel_root()` and `vm_kernel_root_phys()`;
  `kernel/mm/vm_space.c` is a separate, host-testable unit that needs only
  `pmm_alloc_page`/`pmm_free_page` and the identity translations, never CR3 or
  the LAPIC, and `vm_space_activate()` is the only CR3 writer (in
  `kernel/mm/vm.c`), skipping the write when the root is unchanged;
- every space copies slot 0 from the kernel root, so the shared supervisor
  identity map (kernel image, stacks, MMIO) is present in all of them, and keeps
  slot 1 for the 512 GiB to 1 TiB window; mapping upgrades each intermediate
  entry to `PRESENT|WRITE|USER`, clears `VM_GLOBAL`, forces `VM_USER`, keeps the
  caller's W^X flags, validates alignment/window/absent-leaf and rolls back
  exactly the pages of a failed call, and `vm_space_destroy` frees every leaf,
  PT, PD, PDPT and the root page;
- `kernel/mm/vm.c` keeps the active root in a static that `pte_for` and
  `vm_translate` follow, so kernel-side uaccess always walks the running space;
- `struct thread` carries a `vm_space *space` instead of the U1
  `thread_user_map[8]` region list; `schedule()` installs the kernel stack top
  and then calls `vm_space_activate(next->space)`, `thread_destroy()` destroys
  the space, and `thread_create_user(name, entry, user_rsp, space,
  sched_class)` no longer refuses a second user thread -- the U1 one-program rule
  is gone;
- `kernel/exec/elf_exec.c` owns one space per exec: segment reservations and the
  four-page stack at `USER_STACK_TOP` go into it, the unmapped guard page under
  the stack stays, and the space is handed to the new thread. While landing U2,
  `vm_init` started publishing the root immediately after allocating it, because
  the section mapper walks the root and the old order page-faulted before the
  IDT existed;
- `user/iso.c` and its linker script `user/iso.ld` pin a writable page at
  `0x0000008000030000`; two instances write `0x15500000 | (tid << 8)`, yield 300
  times and re-check their own word, and `kernel/core/user_test.c` under
  `user.test=2` proves the same virtual address maps to different physical
  frames (the log prints `frames 0x28000 vs 0x38000`) before joining both;
- `make test-user2` is the 49th gate: it rebuilds the disk with all four
  programs, boots with `user.test=2`, and passes boot-check the new final
  `EXPECT_USERS` argument (argc guard now 3..29) with
  `USER2_RUN_TIMEOUT_SEC` (240), so the boot-check line reports
  `user yes isolated yes`; `make test-user` (gate 48) still runs the three U1
  programs under `user.test=1`;
- `tests/host/test_vm_space.c` covers slot-0 inheritance, the window validation
  table, raw leaf flags, distinct frames for the same VA, partial rollback and
  full reclaim, plus an allocation-failure path; `host_pmm_free_pages_count()`
  in `test_heap.c` makes a later leak visible. `make test-host` reports 3554
  checks, the kernel is 263056 B (256 KiB) on disk and 832574 B (813 KiB)
  loaded, and all 49 gates pass.

Phase U3 puts a real shell at CPL3: four file syscalls reach the mounted ext4
root, the kernel passes `argc`/`argv` on the initial stack, and `/bin/sh` reads
`/etc/sh.rc` from the disk image and writes `/shout.txt` back, which makes
`make test-user3` the 50th gate.

- `include/crug/syscall.h` gains `SYS_OPEN` (17), `SYS_READ` (18), `SYS_CLOSE`
  (19) and `SYS_LISTDIR` (20), `SYS_MAX` becomes 21 and `SYS_ENOENT` (-2) is the
  new missing-path error; `SYS_WRITE` (16) now writes to an open file when
  `fd >= 3` and keeps the console behaviour for 1 and 2;
- `include/crug/ufile.h`/`kernel/fs/ufile.c` hold the per-thread descriptor
  table: 8 slots starting at fd 3, whole-file read snapshots and a 64 KiB write
  buffer, a 64-byte path cap, and `thread_destroy()` calling `ufile_close_all()`;
  a writable close creates/rewrites/truncates the file, fsyncs it and brackets
  the flush with `storage_remount_rw()`/`storage_remount_ro()`, which is why the
  log shows `storage remounted read-write` then `read-only`;
- `kernel/exec/argv.c` builds the initial `argc`/`argv`/`envp` block in a 4 KiB
  area at the top of the user stack (16 args, 2048 bytes of strings, 16-byte
  aligned); `elf_exec_run_args(path, argc, argv, out)` starts the thread with
  `rsp` at `argc`, `user/start.S` loads the three values and `elf_exec_run()`
  stays the `argc == 0` wrapper;
- `user/sh.c`, `user/sh.ld` and `scripts/mkdisk.sh` add `/bin/sh` and
  `/etc/sh.rc`: the shell runs `ver`, `echo`, `cat`, `ls`, `write` and `exit`
  with static buffers (no user heap yet), reads `argv[1]` or `/etc/sh.rc`, and
  the shipped script writes `hello-from-user-shell` (22 bytes with the newline);
- `kernel/core/user_test.c` understands `user.test=3`: the U1 trio, the U2
  isolation pair and then the shell, expecting exit 7; `make test-user3` is the
  50th gate and sets `EXPECT_USER=yes`, `EXPECT_USERS=yes` and the new
  `EXPECT_USHELL=yes` (`argv[29]`, guard now 3..30) under
  `SHELL_RUN_TIMEOUT_SEC` (240), and runs
  `check-disk --expect-shell-write`, which asserts `/shout.txt` contains
  `hello-from-user-shell\n` after a clean umount and `e2fsck`;
- `make test-host` reports 3663 checks, the kernel is 267976 B (261 KiB) on disk
  and 842046 B (822 KiB) loaded (`.text` 190790 B, `.rodata` 28608 B, `.bss`
  622592 B), and all 50 gates pass.

The user part of that gate log:

```
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
```

Phase U4 gives ring 3 a process model: three syscalls spawn, wait for and
replace program images, so a user program can run another one and read its
exit code instead of the kernel doing it. `make test-user4` is the 51st gate.

- `include/crug/syscall.h` gains `SYS_SPAWN` (21), `SYS_WAIT` (22) and
  `SYS_EXEC` (23): `SYS_MAX` becomes 24 and `SYS_ESRCH` (-3) is the new
  missing-child error;
- `elf_spawn_user(path, argc, argv, parent_id, out)` in
  `kernel/exec/elf_exec.c` starts a new program image as a new ring-3 thread
  and returns its tid; the new `parent_id` field in `struct thread` records
  the caller, `elf_exec_run()` becomes the `argc == 0` wrapper and
  `elf_exec_run_args()` calls it with parent id 0 for kernel-owned programs;
- `SYS_WAIT(tid)` blocks until that child exits and returns its code. The new
  `thread_find(id)` walks a global all-threads list that keeps zombies, so a
  wait arriving after the exit still sees the code, and monotonic ids mean a
  stale tid never matches a recycled thread. A wait is accepted only when
  `child->parent_id` is the caller and `child != caller`, otherwise it
  returns `SYS_ESRCH`; `thread_join_code()` reaps the thread;
- `SYS_EXEC(path, argc, argv)` replaces the calling image in place:
  `elf_exec_in_place()` loads the ELF into a fresh `vm_space`, points the
  thread's `space`/`user_rip`/`user_rsp` at it, activates the new space
  before destroying the old one (CR3 still points at it) and returns the new
  entry, so the syscall layer rewrites the saved `rip`/`rsp` and `rax` and
  the `iretq` lands in the new image. The thread keeps its tid and the log
  prints `CRUGOS: user exec /bin/child (tid 14)`; a CPL0 caller gets
  `SYS_EINVAL`;
- `syscall_argv()` in `kernel/core/syscall.c` copies arguments for all three
  calls: CPL3 callers through `uaccess_check`/`uaccess_user_read`/
  `uaccess_user_str` into `g_argv_buf[EXEC_ARGS_STR_MAX]`, CPL0 callers
  through `harden_range_ok` and `memcpy`, both ending in
  `exec_args_ok(argc, str_bytes)`, which rejects more than 16 arguments or
  2048 string bytes;
- `user/spawn.c` and `user/child.c` become `/bin/spawn` and `/bin/child`:
  spawn starts its child with `from-spawn`, prints the tid, waits for exit 5,
  then execs the child with `from-exec`; the child prints its tid, argc and
  the second argument, which is how the log proves both paths;
- `user.test=4` runs the U1 trio, the U2 pair, the shell and then
  `user_spawn()`, which joins `/bin/spawn` and requires exit 5;
- `make test-user4` is the 51st gate: it rebuilds the disk with all seven
  programs, boots with `user.test=4`, passes boot-check the new
  `EXPECT_SPAWN` argument (`argv[30]`, guard now 3..31) with
  `SPAWN_RUN_TIMEOUT_SEC` (240), so the boot-check line ends
  `ushell yes spawn yes`, and runs the plain check-disk pass (no
  `--expect-shell-write`; that proof belongs to gate 50);
- `tests/host/test_argv.c` gained `exec_args_ok` cases. `make test-host`
  reports 3670 checks, the kernel is 276408 B (269 KiB) on disk and 844414 B
  (824 KiB) loaded (`.text` 192902 B, `.rodata` 28864 B, `.bss` 622592 B),
  and all 51 gates pass.

The user part of the gate 51 log (the `...` stands for the U1-U3 markers, which
are unchanged from gate 50):

```
CRUGOS: user selftest start
...
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
CRUGOS: user selftest ok (3 program(s), 1 fault(s))
```

Phase U5 adds the other half of the process model: a ring-3 program can fork
itself, and the child gets its own copy of every user page instead of sharing
the parent's. `make test-user5` is the 52nd gate.

- `include/crug/syscall.h` gains `SYS_FORK` (24), so `SYS_MAX` becomes 25. The
  call returns the child's tid in the parent and 0 in the child, and the child's
  first user instruction is the one after the parent's `int 0x80`;
- `vm_space_clone()` in `kernel/mm/vm_space.c` builds the child's PML4: slot 0
  inherits the shared supervisor identity map, and every present slot-1 leaf
  gets a fresh frame with the parent's `PRESENT`/`WRITE`/`NX` bits and a copy of
  the page. A failure destroys the half-built space, and a parent above
  `VM_SPACE_FORK_MAX_PAGES` (256 pages) is refused rather than partly cloned;
- the child is started by a trampoline, not `thread_entry`:
  `thread_create_forked()` keeps a copy of the parent's trap frame with
  `rax = 0`, and `kernel/arch/x86_64/user.S:thread_fork_enter` parks it
  `THREAD_FORK_FRAME_GAP` (1024) bytes below the child's kernel stack top and
  jumps to the `isr_return` label that `isr.S` now exports before its
  `popq %r15`, so the child returns to CPL3 through the normal interrupt
  epilogue;
- the child's descriptor table starts empty (`ufile_table_init`): a forked
  descriptor cannot be shared safely, and an empty table means a buffered file
  write is never flushed twice;
- `user/fork.c` is the ring-3 probe: the child writes `0x2222` into a volatile
  word and exits 3; the parent waits for the tid, checks exit 3 and must then
  still read its own `0x1111`, which proves the pages were copied and not
  aliased;
- `user.test=5` runs the U1 trio, the U2 isolation pair, the U3 shell, the U4
  spawn/exec probe and then `user_fork()`, which joins `/bin/fork` and requires
  exit 3;
- `make test-user5` is the 52nd gate: it rebuilds the disk with all eight
  programs, boots with `user.test=5`, passes boot-check `EXPECT_USER`,
  `EXPECT_USERS`, `EXPECT_USHELL`, `EXPECT_SPAWN` and the new `EXPECT_FORK`
  argument (`argv[31]`, guard now 3..33) with `FORK_RUN_TIMEOUT_SEC` (240), so
  the boot-check line ends `spawn yes fork yes`, and runs the plain check-disk
  pass;
- `make test-host` reports 3723 checks, the kernel is 276616 B (270 KiB) on disk
  and 846206 B (826 KiB) loaded (`.text` 194310 B, `.rodata` 29248 B, `.bss`
  622592 B), and all 52 gates pass.

Phase U6 turns that eager copy into copy-on-write: a fork now shares the
parent's writable frames read-only with the child and copies a page only when
one side stores to it, so the 300-page probe forks for the price of a fork
frame. `make test-user6` is the 53rd gate.

- `include/crug/cow.h` and `kernel/mm/cow.c` add a per-frame reference table:
  `COW_TABLE_SLOTS` (4096) open-addressing slots with tombstones, keyed by
  `(pa >> 12) * 2654435761` folded with `x ^= x >> 16`. `cow_share(pa)` opens a
  share (first call sets two references) and returns -1 when the table is full,
  `cow_refs(pa)` reports zero for an untracked frame, `cow_release(pa)` returns
  1 when the caller may free the frame and 0 when another space still maps it,
  and `cow_reset()`/`cow_tracked()` reset and count the table;
- `include/crug/vm.h` adds the `VM_COW` software PTE bit (`1ull << 9`, part of
  `VM_PTE_FLAG_MASK`), raises `VM_SPACE_FORK_MAX_PAGES` from 256 to 4096,
  makes `vm_space_clone()` non-const and adds `vm_space_cow_fault()` (0 = nothing
  to resolve, 1 = the frame was copied, 2 = the caller was the sole owner and
  the leaf was repaired in place) plus `vm_active_space()`;
- `kernel/mm/vm_space.c` shares every writable or already-`VM_COW` leaf in the
  clone: the child leaf gains `VM_COW`, the parent leaf loses `VM_WRITE`, and a
  read-only text leaf is shared as a plain read-only mapping - it never takes
  `VM_COW` and never faults, but it is still entered in the refcount table, so
  the frame is freed only when the last space that maps it is destroyed. Every
  present leaf calls `cow_share()`, and a table-full (-1) destroys the
  half-built child and fails the fork with `ENOMEM` instead of falling back to
  an eager copy; `vm_space_unmap_pages()` and `vm_space_destroy()` free a frame
  only when `cow_release()` says it is the last reference;
- `kernel/mm/vm.c` tracks the active space beside the active page table, so the
  fault path and the copy-out path can find the space a user pointer belongs to;
- `kernel/mm/uaccess.c` resolves `VM_COW` leaves in `uaccess_pages_writable()`
  (a `vm_space_cow_fault()` plus `vm_flush_page()`) before a kernel copy-out
  writes through a user pointer;
- `kernel/arch/x86_64/idt.c` handles the user page fault before the ordinary
  report: vector 14 with the U/S and W/R bits set and `trap_classify()` =
  `TRAP_USER` calls `vm_space_cow_fault()` on the current thread's space, and a
  positive answer flushes the page and returns, printing
  `CRUGOS: user cow fault tid N va=... (copy)` or `(sole owner)`;
- `SYS_FORK` flushes the parent's TLB after the child exists (`vm_flush_all()`)
  and prints `CRUGOS: user fork tid N -> M (K page(s) shared, COW)`;
- `user/fork.c` now covers two data pages: the child writes `0x2222` and
  `0x5555`, the parent reaps exit 3 and still reads `0x1111` (4369) and
  `0x3333` (13107), then stores `0x4444` (17476) into the first page - the child
  is gone, so that store takes the sole-owner path - and `user/forkbig.c` maps
  300 pages (1.2 MiB) of `.bss`, sets a sentinel, forks, and requires the child's
  writes to `0x5A` to leave the parent's copy untouched;
- `user.test=6` adds the large-fork probe after the two-page one, and
  `scripts/mkdisk.sh` installs `/bin/forkbig` (nine programs);
- `make test-user6` is the 53rd gate: it rebuilds the disk with all nine
  programs, boots with `user.test=6` under `FORKBIG_RUN_TIMEOUT_SEC` (300),
  passes boot-check the new `EXPECT_FORKBIG` argument (`argv[32]`, argument
  guard now `len(sys.argv) < 3 or > 33`) together with the five earlier
  expectations, so the boot line ends `fork yes forkbig yes`, and runs the plain
  check-disk pass;
- `make test-host` reports 7893 checks (the COW table, the rewritten
  `test_vm_space_clone` and the new `test_vm_space_cow`), the kernel is
  277040 B (270 KiB) on disk and 913790 B (892 KiB) loaded (`.text` 196038 B,
  `.rodata` 29568 B, `.bss` 688128 B), and all 53 gates pass.

Phase 6I gives Intuition full screens, so the desktop is no longer one surface:

- `include/crug/intuition.h` + `kernel/gfx/intuition.c` keep up to
  `INTUITION_MAX_SCREENS` (3) screens, each with its own name, background
  colour, window list and desktop icons; only the front screen is painted and
  the others keep all their state. `intuition_add_screen(name, bg_rgb)` returns
  the new index or -1 (no framebuffer, a NULL name, or three screens already),
  `intuition_set_screen(index)` selects the target screen for later
  `intuition_open`/icons, `intuition_screen_switch(index)` brings a screen to
  the front, and `intuition_move_window_screen(id, index)` re-parents a window;
- a switch drops any pending menu, drag, resize, gadget, body or icon
  interaction, activates the top window of the new screen, counts one action,
  queues `INTUITION_EVENT_SCREEN` (window 0, item = screen index) and prints
  `CRUGOS: wb screen N front (K window(s))`; a move re-parents the window, drops
  the interactions it owned, hands activation to the next window of the front
  screen, counts one move, queues `INTUITION_EVENT_SCREEN` (window = id,
  item = target screen) and prints `CRUGOS: wb window N moved to screen M`;
- the screen strip sits at the right end of the menu bar, one box per screen
  with the front box highlighted: box width is `strlen(name) * 8 + 12`,
  `strip_x` is the screen width minus the strip width minus 6, and a press on
  a box that is not in front switches screens while a press on the front
  screen's own box does nothing;
- `kernel/gfx/workbench.c` reads `wb.screens=1` (default 0, the plain
  single-screen desktop): it adds a `Tools` screen with background `0x00203010`
  (RGB 32,48,16), opens a 340x200 `Tools` window at (160,140) with the `Tools`
  `{Move, Close}` and `Workbench` `{About, Info, Quit}` menus and a
  `Send to Workbench` gadget at body-relative (12,60,160,22), returns to
  screen 0 and prints `CRUGOS: wb screens ready (N screen(s), front 0)`; a press
  on that gadget moves the Tools window to screen 0 and the Workbench body
  reads `phase 6j`;
- `scripts/boot-check.py` gains the phase marker and a new `--expect-screen=yes`
  flag (a flag, not a positional argument, so the argument order and the argc
  range are unchanged) which requires the screens markers, the switch, the
  move and the `wb action N screen N front` lines;
- `make test-workbench-screens` is the 54th gate:
  `scripts/run-workbench-screens.py` boots with
  `wb.enable=1 wb.wait=30 wb.screens=1 wb.acts=4`, clicks the Tools box at
  (992,9) and checks the Tools background, presses the gadget at (254,233),
  clicks the Workbench box at (924,9) and checks the Workbench background, the
  moved window's amber title bar and its body;
- `tests/host/test_intuition.c` gains `test_intuition_screens` (the three-screen
  limit, the target screen, only the front screen painted, a strip press on a
  covered screen, window moves that queue one event and re-hand activation,
  out-of-range indexes and a drag dropped by a switch), and `make test-host`
  reports 7961 checks;
- `make size` reports a 282032 B (275 KiB) kernel file and an 916294 B (894 KiB)
  load image (`.text` 197958 B, `.rodata` 30144 B, `.bss` 688128 B), and all
  54 gates pass.

Phase 6J gives the Workbench a Commodities Exchange, a small table of global
hotkey utilities with a manager window:

- `include/crug/commodity.h` + `kernel/gfx/commodities.c` are a pure,
  framebuffer-free registry: `COMMODITY_MAX` 4, `COMMODITY_NAME_MAX` 16,
  `COMMODITY_HEADER_H` 24, `COMMODITY_ROW_TOP` 28, `COMMODITY_ROW_H` 26 and
  `COMMODITY_ROW_X` 6, with `struct commodity` holding `name`, `hotkey`,
  `enabled`, `fires` and `blocked`;
- the API is `commodity_reset()`, `commodity_register(name, hotkey)` (the new
  index, or -1 for a bad name, a hotkey outside F1..F12 or a duplicate name or
  hotkey), `commodity_toggle()` (the new state or -1),
  `commodity_set_enabled()`, `commodity_hotkey(key, &fired)` (the owning index
  or -1; `fired` is 1 when the action ran and 0 when it was blocked),
  `commodity_count()`, `commodity_at()`, `commodity_index_of_hotkey()`,
  `commodity_fires()`/`commodity_blocked()`, `commodity_enabled_count()`,
  `commodity_key_name()` (`"F1"`..`"F12"`) and `commodity_row_at()`;
- `include/crug/input.h` adds `INPUT_KEY_F1` `0x94u`..`INPUT_KEY_F12`
  `0x9Fu` above the arrows: PS/2 set-1 make codes `0x3B`..`0x44` map to F1..F10,
  `0x57` to F11 and `0x58` to F12 (break codes yield nothing and Shift/Ctrl do
  not change the byte), and USB HID boot usages `0x3A`..`0x45` map to F1..F12
  (F13 is ignored);
- `kernel/gfx/workbench.c` registers `Volume up` F2, `Volume down` F3, `Mute`
  F4 and `Clock` F6 and opens an `Exchange` window at (560,120), size 344x196,
  last so it starts on top: rows are 26 pixels high from 28 pixels below the body
  top, the name sits at `x+6`, the key name at `x+w-96` and `[on]`/`[off]`
  at `x+w-44` in amber when on, and a row click toggles the commodity;
- a disabled commodity's key is refused, bumps its `blocked` count and prints
  `CRUGOS: wb commodity NAME blocked (hotkey off)` without running; an enabled
  one bumps `fires` and runs, printing
  `CRUGOS: wb action N commodity NAME fired (...)`. Volume up/down move the
  mixer by `WB_CX_VOLUME_STEP` 600 within 0..`AUDIO_VOLUME_MIN` -7680, Mute
  toggles the mixer mute and Clock prints the monotonic uptime; a key no
  commodity owns stays with the desktop;
- `wb.cx=<0|1>` (only 0 disables, and it needs `wb.enable=1`) enables the
  exchange and prints `CRUGOS: wb cx ready (N commodity(ies))`; the self-test
  waits for two row toggles and two fires and ends with
  `CRUGOS: wb cx selftest ok (2 toggle(s), 2 fire(s))`;
- `make test-workbench-cx` is the 55th gate: `scripts/run-workbench-cx.py`
  boots with `wb.enable=1 wb.wait=30 wb.cx=1 wb.acts=4`, hot-plugs a
  `usb-mouse` and a `usb-kbd` over QMP, clicks the Mute row at (600,235),
  presses F4 while the commodity is off and again when it is on, presses F6 for
  the Clock, and takes a `screendump` whose Exchange body (700,250) must be
  (8,16,24) and whose active title bar (600,130) must be (255,176,0);
  `scripts/boot-check.py` gains `--expect-cx=yes` (a flag, not a positional
  argument, so the argument order and the argc range are unchanged), and the run
  ends with a clean `e2fsck`;
- `tests/host/test_commodity.c` covers the registry (registration limits,
  duplicate name and hotkey, toggle, blocked and fired counters, the row hit
  test, the key names) and `test_input.c`/`test_usb.c` cover both function-key
  mappings, so `make test-host` reports 8102 checks; `make size` reports a
  283088 B (276 KiB) kernel file and a 919594 B (898 KiB) load image (`.text`
  200518 B, `.rodata` 30880 B, `.data` 68 B, `.bss` 688128 B), and all 55
  gates pass.

Phase 7A gives the system application bundles: directories a user copies onto
the disk without rebuilding the kernel.

- `/Applications/<Name>.app/manifest.toml` plus
  `/Applications/<Name>.app/bin/x86_64/<program>` make one bundle; the payload is
  an ordinary ring-3 ELF and the manifest is a bounded TOML subset
  (`APP_MANIFEST_MAX` 2048 bytes) with `[app]` (`id`, `name`, `version` as three
  numeric parts, optional `min_os`), `[exec]` (`prog`, relative, no `..`),
  `[tooltypes]` (up to eight uppercase `KEY = "value"` entries) and `[icon]`
  (`rgb = "RRGGBB"`);
- `kernel/app/manifest.c` is the pure parser and `kernel/app/app.c` the registry:
  `APP_MAX` 8 bundles scanned once by `app_init()` after the filesystem mounts and
  before the scheduler starts, sorted by id, with duplicate ids and names refused;
  a bundle either registers or prints `CRUGOS: app rejected <dir> (<reason>)` with
  one of nineteen stable reasons (`no-manifest`, `bad-entry`, `bad-version`,
  `bad-prog`, `bad-rgb`, `too-many-tooltypes`, `dup-id`, `registry-full`, ...),
  and the boot line is
  `CRUGOS: app registry ready (N app(s), M rejected)`;
- `app_launch()` checks that the payload resolves and calls `elf_spawn_user()`
  with the bundle directory as `argv[0]`, so a bundle program can find its own
  files;
- `wb.app=1` adds an `Applications:` drawer icon at (16,300) and opens a 340 by
  240 window at (560,320) on a double click; one row per bundle, 20 pixels apart,
  name left and version right, an amber band on the selected row, and a second
  press within `WB_DOUBLE_CLICK_NS` 600 ms launches it. One launch runs at a time
  (`CRUGOS: wb app busy (tid N)`), and the desktop reaps the thread and prints
  `CRUGOS: wb app reaped tid N exit E`;
- the ring-3 shell gains `app list`, `app info <id>` and `app run <id>`, backed by
  `SYS_APP_COUNT` 25, `SYS_APP_INFO` 26 and `SYS_APP_SPAWN` 27 (`SYS_MAX` 28):
  `SYS_APP_INFO` copies a 512-byte `id|name|version|prog|RRGGBB|KEY=value;...`
  record through the uaccess boundary and `SYS_APP_SPAWN` starts a registered
  bundle and returns its tid;
- `mkdisk.sh` installs `Hello.app` (tool type `GREET="world"`, amber icon) and
  `Orbit.app` (`MODE="demo"`, blue icon) plus a deliberately broken
  `Broken.app` the registry rejects, and `user.test=7` runs
  `/bin/sh /etc/app.rc` (`app list`, `app info orbit`, `app run orbit`, `exit 9`)
  so the gate proves the shell path as well as the desktop path;
- `make test-app-bundles` is the 56th gate in two legs: leg one boots headless
  with `wb.enable=1 wb.wait=30 wb.app=1 wb.acts=1`, double-clicks the icon at
  (48,324) and the first row at (600,358), checks the window pixels and the
  self-test count, and passes `--expect-app=yes` to `boot-check.py` (a flag, so
  the argument order is unchanged); leg two boots `user.test=7` under
  `APP_RUN_TIMEOUT_SEC` 300 and checks the shell markers; both end with a clean
  `e2fsck`;
- `tests/host/test_app.c` parses the shipped manifests, rejects the broken one and
  walks the reason table, and `make test-host` reports 8216 checks;
- `make size` reports a 296848 B (289 KiB) kernel file and a 942394 B (920 KiB)
  load image (`.text` 209926 B, `.rodata` 31968 B, `.data` 84 B, `.bss` 700416 B),
  and all 56 gates pass.

See [docs/apps.md](docs/apps.md) for the manifest grammar, the reason table and
the registry API.

Phase 7B gives the system capabilities and signed packages: a program can do
only what its capability mask allows, and the code it runs arrives as a signed,
verifiable package instead of a copied directory.

- `include/crug/caps.h` defines the six bits a mask can hold: `CAP_FS_READ`
  0x01, `CAP_FS_WRITE` 0x02, `CAP_APP_LAUNCH` 0x04, `CAP_NET` 0x08,
  `CAP_AUDIO` 0x10 and `CAP_PKG_INSTALL` 0x20, plus `CAP_ALL` 0x3F and
  `CAP_NONE`; `kernel/lib/caps.c` parses, formats and intersects them;
- a ring-3 thread inherits its parent's mask, and a bundle launch narrows it to
  the intersection of the manifest's `[caps] require` line and the policy. The
  policy is `/System/perms.conf` (`default <caps>` plus one line per app id, at
  most eight rules) and the boot prints
  `CRUGOS: perms policy ready (default app.launch,fs.read, N rule(s))`;
- `kernel/core/syscall.c` checks the mask on every gated call: `SYS_OPEN` needs
  `fs.read` (`fs.write` for a write mode), `SYS_LISTDIR` `fs.read`,
  `SYS_APP_SPAWN` `app.launch`, `SYS_PKG_INSTALL` `pkg.install` and the socket
  and resolver calls `CAP_NET`. A refusal is `SYS_EPERM` (-1); a caller at CPL0
  always passes, so the in-kernel self-tests keep working;
- two calls let a program inspect its own rights instead of running into the
  wall: `SYS_CAP_QUERY` 28 returns the mask and `SYS_CAP_CHECK` 29 answers 1 or
  0 for a mask argument;
- a `.crpkg` package is a 512-byte header (`PKG_MAGIC` `CRUGPKG1`, id, name,
  version, publisher, capability mask, icon colour, the 32-byte public key at
  offset 0xE8 and the 64-byte Ed25519 signature at 0x110) followed by a
  container of up to 32 files (96 KiB each, `PKG_MAX_BYTES` 128 KiB). The
  signature covers the SHA-512 digest of header and payload, so
  `kernel/crypto/sha512.c` and `kernel/crypto/ed25519.c` are the first crypto
  the kernel performs for the user's benefit rather than for disk encryption;
- `kernel/app/pkg.c` verifies the signature against
  `/System/trusted/keys.conf` (`pub <64 hex> <publisher>`), stages the files
  under `/Applications/.stage/<id>`, renames the tree to `<id>.app`, writes a
  record to `/System/pkgdb/<id>.txt` and registers the bundle, then prints
  `CRUGOS: pkg installed <id> <version> (N file(s), M cap(s))`. A refusal prints
  `CRUGOS: pkg rejected <path> (<reason>)` with one of twenty-one stable reasons
  (`untrusted-key`, `digest`, `signature`, `already-installed`,
  `too-many-files`, ...), and the boot line is
  `CRUGOS: package registry ready (N package(s))`;
- `make mkpkg` builds `tools/mkpkg.c` and `make packages` signs the test
  bundles. `PKG_SEED` and `PKG_BAD_SEED` are two fixed Ed25519 seeds, so the
  images are byte-identical run to run and the trust list holds only the first
  key, which is what makes the untrusted refusal reproducible;
- `wb.pkg=1` adds a `Packages` window at (40,330), 420 by 200, with one row per
  package and a one-click install. The row shows the id and the version, turns
  amber with `installed` once the package lands, and the desktop counts the
  installs in its self-test;
- the gates are `make test-pkg` (57), `make test-caps` (58) and
  `make test-workbench-pkg` (59). The first boots `pkg.test=1` and proves an
  install, a duplicate refusal and an untrusted-key refusal; the second boots
  `pkg.test=2` and runs `/bin/captest` with `fs.read` only, so its write and its
  install both return `SYS_EPERM` and it exits 5; the third installs the same
  package from the desktop and checks the window pixels;
- `tests/host/test_pkg.c` covers the SHA-512 and Ed25519 vectors, the mask
  helpers, the policy parser, the trust list, the header layout and the payload
  walk, so `make test-host` reports 8403 checks; `make size` reports a 329016 B
  (321 KiB) kernel file and a 980414 B (957 KiB) load image (`.text` 236070 B,
  `.rodata` 35648 B, `.data` 88 B, `.bss` 708608 B), and all 59 gates pass.

Phase 7C gives those packages a lifecycle:

- `pkg_install()` compares the header version with the record in
  `/System/pkgdb/<id>.txt` before it stages anything: the same version is
  refused as `already-installed`, an older one as `downgrade`, and a newer
  one takes the update path;
- an update stages the new tree, renames the installed bundle to
  `/Applications/.stage-<id>.old`, renames the stage into place, rewrites the
  record and rescans; every failure restores the backup, and the backup is
  deleted only after the record is on disk. The success line is
  `CRUGOS: pkg updated <id> <old> -> <new> (N file(s), M cap(s))`;
- `pkg_remove(id)` remounts writable, deletes `/Applications/<id>.app`
  recursively, unlinks the pkgdb record, fsyncs and rescans, then prints
  `CRUGOS: pkg removed <id> (N package(s))`; `SYS_PKG_REMOVE` 33 is the only
  removal path and needs `pkg.install` (`SYS_MAX` is now 34);
- `SYS_SPAWN` and `SYS_EXEC` are gated on `app.launch` and
  `elf_spawn_user_caps()` passes the caller's mask to the child, so a narrowed
  program can no longer recover `CAP_ALL` by starting `/bin/sh`;
- the ring-3 shell gained `pkg list`, `pkg info <id>`, `pkg install <path>`
  and `pkg remove <id>`, and `/bin/captest` now also proves that
  `pkg.remove`, spawn and exec are denied;
- the desktop window became a toggle: the body press installs when the id is
  absent and removes it when present, paints `installed` or `removed`, and
  the selftest counts two installs and one remove;
- gate 57 now walks install, update, downgrade, remove and reinstall;
  `scripts/mkpkgtest.sh` builds the 2.0.0 package from the same seed. The gate
  count stays at 59, and `make test-host` is 8408 checks after the new
  `test_pkg.c` cases; `make size` reports a 333224 B (325 KiB) kernel file and a
  984062 B (960 KiB) load image (`.text` 239078 B, `.rodata` 36288 B, `.data`
  88 B, `.bss` 708608 B).

See [docs/packages.md](docs/packages.md) for the capability matrix, the policy
and manifest grammar, the container layout, the update and removal pipeline and
the refusal table.

Phase 8A reaches the 2D half of the specification's GPU phase:

- `drivers/gpu/virtio_gpu.c` binds PCI device 0x1050 over the modern virtio-pci
transport, detects the legacy 0x1010 id only to report it unusable, configures
control queue 0 from three allocated pages and drives six 2D commands
synchronously with a two-second deadline;
- `gpu_init()` creates resource 1 as `B8G8R8X8_UNORM` with
`ceil(w * h * 4 / 4096)` contiguous backing pages (1000 at 1280x800), attaches
it, makes it the scanout and prints `CRUGOS: virtio-gpu ready ...` and
`CRUGOS: gpu scanout ok ...`;
- the mode comes from `gpu.size=WxH`, else the host display info, else the
first of 1024x768, 800x600 and 640x480 that the device accepts, capped at
4096x2160 and 8192 backing pages;
- `fb_attach_linear()` installs the resource as the framebuffer with a present
callback; `fb_store()` accumulates a damage bounding box and `fb_present()`
transfers that one rectangle and flushes it, while `fb_present_begin()` and
`fb_present_end()` batch a whole desktop repaint into a single transfer and
the console flushes per line rather than per glyph;
- a missing device, `gpu.enable=0` or any driver failure prints one reason line
and keeps booting on the firmware framebuffer, so all 59 earlier gates boot
exactly as before; `gpu.test=1` makes the self-test announce the skip instead
of staying silent;
- `make test-gpu` boots without any VGA adapter (`-vga none -device
virtio-gpu-pci,id=gpu0`), so the resource is the machine's only display, and
asserts four pixel colours plus amber and frame colour counts from a QMP
screendump of `gpu0` and the self-test's transfer and flush counters;
- there is no 3D yet: no virglrenderer, no Vulkan, no command stream and no
accelerated triangle, which is what a future phase 8B is for;
- `tests/host/test_gpu.c` covers the pure helpers (command encoding, response
classes, rectangle clipping, backing-page arithmetic, display pick and damage
merge), so `make test-host` reports 8488 checks; `make size` reports a 347208 B
(339 KiB) kernel file and a 994626 B (971 KiB) load image (`.text` 248230 B,
`.rodata` 37696 B, `.data` 92 B, `.bss` 708608 B), and all 60 gates pass.

See [docs/gpu.md](docs/gpu.md) for the transport, the resource, the present
model and the failure lines.

The kernel also detects AVX2, enables XSAVE state and, by default, refuses to
boot without AVX2. Self-tests run at boot, then the kernel prints a boot report
to a 16550 serial port and the framebuffer and exits QEMU deterministically for
automated testing.

Verified in this tree:

```
CrugOS 0.46.0 (phase 8a) -- Amiga-inspired microkernel bring-up
CRUGOS: hello
CRUGOS: version 0.46.0
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
CRUGOS: heap check ok (6 slab(s), 8 object(s), 1 large block(s))
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
kernel : 0x0000000000100000 size=339 KiB
boot   : cpu=0 acpi_rsdp=0x1f77e000 fdt=0x0 initrd=0x0
memory : 36 regions, usable 474080 KiB (462 MiB)
pmm    : total=118520 free=118462 bitmap=131072 B
fb     : 1024x768 stride=1024 format=BGRX8888 base=0x80000000
cmdline: 28 bytes (redacted)
entropy: ca02033c50b53413
wx     : audit ok, identity map 0..4 GiB, guard unmapped
heap   : total=57344 B used=8976 B slab_pages=12 large_pages=2
sched  : switches=112
boot   : pmm=27601 us cpu=408 us vm=624 us traps=333 us timer=65376 us heap=507 us storage=95213 us sched=5952 us syscall=107 us total=196126 us
timer  : 2687896922 Hz (pit), ticks=31, mono=458247353 ns
CRUGOS: phase 8a boot ok
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

`make demo` shows the whole Workbench interactively: it boots with a
pre-attached USB keyboard and mouse into a GTK window and holds the framebuffer
with `wb.hold=`. The window is the virtio-gpu 2D scanout: `DEMO_GPU=1` (the
default) selects `DEMO_GPU_QEMU_EXTRA` (`-vga none -device
virtio-gpu-pci,id=gpu0`), which leaves no firmware framebuffer at all, and
`DEMO_GPU_SIZE=1024x768` becomes `gpu.size=` on the command line, so the log
starts with `CRUGOS: virtio-gpu ready (1024x768, 768 backing page(s), resource
1, modern pci)` and `CRUGOS: gpu scanout ok (1024x768, 1 transfer(s), 1
flush(es))`. Set `DEMO_GPU=0` to drop the device and the `gpu.enable=`/`gpu.size=`
tokens and boot the firmware framebuffer. The default `DEMO_CMDLINE` switches
every desktop feature on at once --
`wb.desk=1` for the `System:` object, `wb.dr=1 wb.dsel=1` for its drawer (filled
with `DEMO_FILL_FILES` files the first time it opens), `wb.screens=1` for the
`Workbench`/`Tools` screen strip, `wb.cx=1` for the Commodities Exchange and
`wb.shell=1` for a live Shell window -- so double-clicking the object, dragging
rows, switching screens, pressing `F2`/`F3`/`F4`/`F6` and typing at the prompt
all work without a gate. Because the Shell owns the keyboard, the drawer's
arrow-key navigation is inert; its rows and scrollbar are driven by the mouse.
`build/x86_64/serial.log` records the ready markers (`wb icon ready`,
`wb drawer mode`, `wb select mode`, `wb screens ready`, `wb cx ready`,
`wb shell ready`), the `CRUGOS: usb mouse move` lines and the
`CRUGOS: wb holding desktop for <seconds> s` line, and the guest still exits
with status 33 when the hold ends. `DEMO_HOLD_SEC=0` plus
`DEMO_DISPLAY=none` turns it back into a fast headless boot, and
`DEMO_FILL_FILES=0` leaves the volume untouched.

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
make test-workbench-shell  # type commands into the Workbench shell window
make test-workbench-keys   # resize a window by its grip and drive it from the keyboard
make test-workbench-desk   # select desktop objects and send a window to the back
make test-workbench-icons  # drag a desktop icon, raise a window by its front gadget and drive the icons from the keyboard
make test-workbench-drawer  # scroll, refresh and resize the System: drawer
make test-workbench-select  # click drawer rows and drag its scrollbar
make test-workbench-screens  # switch screens and move a window between them
make test-workbench-cx  # drive commodity hotkeys and toggle rows in the Exchange manager
make test-app-bundles  # launch a .app bundle from the desktop and from the shell
make test-pkg    # install, update and remove a signed package and refuse a duplicate and an untrusted key
make test-caps   # launch the installed bundle and prove its capability wall
make test-workbench-pkg  # install, remove and reinstall a signed package from the Workbench
make test-gpu    # boot on a virtio-gpu scanout and verify the GPU-backed desktop
make test-hardening  # craft hostile superblock/inode/crypt images and boot them
make test-heap   # corrupt the kernel heap and prove the integrity checker catches it
make test-trap   # inject eight crafted faults and check the report, IST and panic
make test-fuzz   # mutate a real disk image and fuzz the mount path
make test-user   # boot with user.test=1 and run three ring-3 programs
make test-user2  # boot with user.test=2, run the three programs and the isolation pair
make test-user3  # boot with user.test=3 and run the scripted ring-3 shell
make test-user4  # spawn and exec ring-3 programs and wait for their exit codes
make test-user5  # fork a ring-3 program and prove the child gets its own pages
make test-user6  # fork copy-on-write and prove 300 shared pages are isolated
make iso         # build a bootable hybrid UEFI ISO (build/x86_64/crugos.iso)
make check-iso   # validate the ISO's El Torito, FAT ESP, MBR and file set
make test-iso    # boot that ISO from a USB stick and from a CD-ROM
make usb         # write the ISO to a USB block device (USB_DEV=/dev/sdX)
make test-compat # boot a non-AVX2 build on a non-AVX2 CPU model
make test-host   # host unit tests (string, printf, ELF, bitmap, VM math, handles, CRC32C, AES/XTS/Argon2id/BLAKE2b, crypt slots, line editor, scancodes, USB HID keyboard and mouse, pointer state, IPv4 checksums, DHCP parsing, DNS, audio mixer/tone/ring, framebuffer cursor, intuition windows/menus/gadgets, resize grips, keyboard focus and keys, icon dragging, the send-to-front gadget and keyboard icon focus, the body input handler, the text grid and the shell, the user-window and uaccess validators, the per-space page-table tests, the ELF exec mapper, the argv predicates, the fork clone, the copy-on-write table, the screen model, the commodity registry, the app manifest parser, the capability mask and permission policy, the package container with its Ed25519 signatures, and the version comparison an update and a removal depend on)
make check-disk  # e2fsck + persisted-file checks on the latest test disk
make user        # build the ring-3 programs and .app payloads the disk image carries
make packages    # sign the test bundles the installer gates install
make mkpkg       # build the host package signer (tools/mkpkg.c)
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
`EXPECT_MENU`, `EXPECT_SHELL`, `EXPECT_KEYS`, `EXPECT_DESK`, `EXPECT_ICONS`,
`EXPECT_DRAWER`, `EXPECT_SELECT`, `EXPECT_HEAP`, `EXPECT_USER`, `EXPECT_USERS`,
`EXPECT_USHELL`, `EXPECT_SPAWN`, `EXPECT_FORK`, `EXPECT_FORKBIG`,
`AUDIO_RUN_TIMEOUT_SEC`, `AUDIO_QEMU_EXTRA`, `GFX_WAIT`, `GFX_MOVES`,
`GFX_RUN_TIMEOUT_SEC`, `GFX_QEMU_EXTRA`, `WB_WAIT`, `WB_DRAG`,
`WB_RUN_TIMEOUT_SEC`, `WB_QEMU_EXTRA`, `WB_MENU_WAIT`, `WB_MENU_ACTS`,
`WB_MENU_RUN_TIMEOUT_SEC`, `WB_SHELL_WAIT`, `WB_SHELL_CMDS`,
`WB_SHELL_RUN_TIMEOUT_SEC`, `WB_KEYS_WAIT`, `WB_KEYS_ACTS`,
`WB_KEYS_RUN_TIMEOUT_SEC`, `WB_DESK_WAIT`, `WB_DESK_ACTS`,
`WB_DESK_RUN_TIMEOUT_SEC`, `WB_ICONS_WAIT`, `WB_ICONS_ACTS`,
`WB_ICONS_RUN_TIMEOUT_SEC`, `WB_DRAWER_WAIT`, `WB_DRAWER_ACTS`, `WB_DRAWER_FILL`,
`WB_DRAWER_RUN_TIMEOUT_SEC`, `WB_SELECT_WAIT`, `WB_SELECT_ACTS`,
`WB_SELECT_FILL`, `WB_SELECT_RUN_TIMEOUT_SEC`, `WB_SCREEN_WAIT`,
`WB_SCREEN_ACTS`, `WB_SCREEN_RUN_TIMEOUT_SEC`, `WB_CX_WAIT`, `WB_CX_ACTS`,
`WB_CX_RUN_TIMEOUT_SEC`, `WB_APP_WAIT`, `WB_APP_ACTS`,
`WB_APP_RUN_TIMEOUT_SEC`, `WB_PKG_WAIT`, `WB_PKG_RUN_TIMEOUT_SEC`,
`GPU_WB_WAIT`, `GPU_RUN_TIMEOUT_SEC`, `GPU_QEMU_EXTRA`,
`MKPKG`, `PKG_DATA_DIR`, `PKG_SEED`, `PKG_BAD_SEED`, `PKG_RUN_TIMEOUT_SEC`,
`CAPS_RUN_TIMEOUT_SEC`, `HARDEN_TIMEOUT_SEC`,
`HEAP_STRESS_OPS`, `HEAP_RUN_TIMEOUT_SEC`, `TRAP_TIMEOUT_SEC`, `FUZZ_CASES`,
`FUZZ_CRYPT_CASES`, `FUZZ_SEED`, `FUZZ_TIMEOUT_SEC`, `USER_RUN_TIMEOUT_SEC`,
`USER2_RUN_TIMEOUT_SEC`, `SHELL_RUN_TIMEOUT_SEC`, `SPAWN_RUN_TIMEOUT_SEC`,
`FORK_RUN_TIMEOUT_SEC`, `FORKBIG_RUN_TIMEOUT_SEC`, `APP_RUN_TIMEOUT_SEC`,
`DEMO_DISPLAY`, `DEMO_HOLD_SEC`, `DEMO_FILL_FILES`,
`DEMO_TIMEOUT_SEC`, `DEMO_CMDLINE`, `DEMO_MACHINE_EXTRA`, `DEMO_QEMU_EXTRA`,
`DEMO_GPU`, `DEMO_GPU_SIZE`, `DEMO_GPU_QEMU_EXTRA`,
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
| `kernel/exec/` | ELF loader for ring-3 programs: segment policy, W^X user mappings and the user stack |
| `kernel/gpu/` | the pure virtio-gpu command, response and damage helpers shared with the host tests |
| `kernel/lib/` | freestanding string, formatting, bitmap, console line-editor and text-grid routines, the capability mask and the permission policy |
| `kernel/input/` | `input.device`: set-1 scancode decoder, the console byte queue and the pointer state |
| `kernel/audio/` | `audio.device` PCM ring, the per-channel mixer and the tone generator |
| `kernel/net/` | `net.device`, Ethernet/ARP/IPv4/ICMP/UDP, TCP, the DHCP client and the DNS resolver |
| `kernel/gfx/` | `gfx.device` (the pointer-driven framebuffer cursor) and `intuition.device` with the phase 6A desktop, its phase 6B menus, gadgets and event queue, and the phase 6C body painter the shell window uses, the phase 6D resize grips and keyboard navigation, the phase 6E desktop objects, depth gadget and drawer, the phase 6F icon dragging, send-to-front gadget and keyboard icon focus, the phase 6G scrolling drawer with its scrollbar, refresh and reflow, and the phase 6H drawer row selection with its draggable scrollbar and body-input hook |
| `kernel/app/` | the application registry, the bounded manifest parser for `.app` bundles and the signed `.crpkg` container |
| `kernel/shell/` | the Workbench shell: line editor, history, the command table and the `shell_io` binding to the mounted ext4 root |
| `kernel/mm/` | physical allocator, W^X page tables, the user-window mapper and uaccess copy boundary, copy-on-write frame references, kernel heap |
| `kernel/block/` | block-device registry, partitions, bounds checks, RAID1/RAID10, boot-time rebuild and the AES-256-XTS `crypt.device` |
| `kernel/fs/` | GPT, writable checksummed ext4, checksummed JBD2 journal and the storage boot policy (including the console passphrase prompt) |
| `kernel/crypto/` | AES, XTS, AES-NI, BLAKE2b, Argon2id, SHA-512 and Ed25519 (shared with the host tools) |
| `drivers/` | PCI, legacy virtio-blk, the virtio-gpu 2D driver, NVMe and AHCI/SATA block device drivers, legacy virtio-net, and the xHCI controller with its USB HID keyboard, mouse and Audio Class decoders (audio frames flow into `kernel/audio/`) |
| `scripts/` | QEMU runner, disk/RAID1/RAID10/crypt image builders, the hybrid ISO builder (`mkiso.sh`, `mkfat.py`), boot/PE/disk/RAID/journal/crypt/ISO checks, the serial and keyboard prompt drivers, tamper helper, size and reproducibility gates |
| `tests/host/` | host unit tests for freestanding code and the mount-path fuzzer (`fuzz_mount.c`) |
| `tools/` | host-side development tools (`gen-font.py`, `mkcrypt.c`, `gen-crc32c.py`) |
| `user/` | ring-3 programs and their freestanding runtime (`user.ld`, `start.S`, `lib.h`) |
| `docs/` | [build](docs/build.md), [boot](docs/boot.md), [storage](docs/storage.md), [checksums](docs/checksums.md), [crypto](docs/crypto.md), [input](docs/input.md), [network](docs/network.md),
[NVMe](docs/nvme.md), [AHCI/SATA](docs/ahci.md), [RAID1/RAID10](docs/raid.md), [USB/xHCI](docs/usb.md), [audio](docs/audio.md), [graphics](docs/gfx.md), [virtio-gpu](docs/gpu.md), [Workbench](docs/workbench.md), [application bundles](docs/apps.md), [signed packages](docs/packages.md), [bootable ISO](docs/iso.md), [kernel heap](docs/heap.md), [trap policy](docs/traps.md), [mount-path fuzzing](docs/fuzzing.md), [ring 3](docs/userspace.md), [cpu/AVX2](docs/cpu.md), [versioning](docs/versioning.md), [roadmap](docs/roadmap.md) |

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
the close gadget and the typed event queue; phase 6C put a live shell with
console scrollback in the second window and wired the keyboard to it; phase 6D
made the windows resizable from a corner grip and taught the keyboard Tab
focus, 8-pixel arrow moves and a menu popup; phase 6E put desktop objects under
the windows, added the depth gadget and painted a drawer from the root
directory; phase 6F made those icons draggable, added the send-to-front gadget
and gave the icon table keyboard focus; phase 6G made that drawer scroll with a
scrollbar, refresh with `r` and reflow on resize, and added the read-write
remount helpers the fill and the shell's `write` command need; phase 6H gave a
window body its own input handler, which the drawer uses to select rows on a
click and to drag its scrollbar thumb. Remaining
phase 3B work is
online RAID rebuild, hot spare, scrub and a write-intent bitmap, RAID below
encryption, a redundant ESP, TPM/keyfile keyslots and USB storage; phase 4
continues with framebuffer (GOP) console input and virtual terminals, plus a
routing table and richer resolver features on top of the phase 4B-4D stack;
the next step is the rest of Workbench userspace: keyboard row navigation and
Enter activation from the drawer, multi-select, renaming an icon, sub-menus, a
gadget toolkit and, in phase 7, application bundles (spec section 35), on top of
the phase 6H desktop (window resizing, the depth and front gadgets, icon
dragging, a scrolling drawer and its selectable rows and draggable scrollbar are
done). Later
phases cover USB storage, graphics, Workbench userspace, SMP/IOMMU and
virtualisation. See
[docs/roadmap.md](docs/roadmap.md) for the full sequence and the known
specification gaps (Crug syntax, cross-architecture compilers and the OVMF
minimum RAM floor).

## License

See [LICENSE](LICENSE).