# Versioning

CrugOS uses [Semantic Versioning 2.0.0](https://semver.org/) with annotated
`vMAJOR.MINOR.PATCH` Git tags. The current release is `v0.31.0`, phase H1:
hardening pass 1, the first pass that treats the boot path as hostile input.
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
