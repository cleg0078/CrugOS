# Versioning

CrugOS uses [Semantic Versioning 2.0.0](https://semver.org/) with annotated
`vMAJOR.MINOR.PATCH` Git tags. The current release is `v0.25.0`, phase 6C: a
live shell in the Workbench's second window. `include/crug/textgrid.h` +
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
