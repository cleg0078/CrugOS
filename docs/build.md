# Building CrugOS

This document describes the phase 6B build system: how the kernel, the UEFI
loader, the host crypto tool, the test disk images and the bootable hybrid ISO
are built, which invariants the link must preserve, and how to diagnose the
failure modes that are specific to this toolchain.

## Requirements

| Tool | Purpose | Notes |
| --- | --- | --- |
| `crugc` | Crug compiler | 0.2.3 / GCC 16.2.0, host `x86_64-pc-linux-gnu`; `-fcrug-kernel` |
| `ld` (GNU binutils) | loader link | must support `-m i386pep` |
| `qemu-system-x86_64` | boot test | 10.x, TCG works without `/dev/kvm` |
| OVMF | UEFI firmware | `/usr/share/OVMF/OVMF_CODE_4M.fd` + `OVMF_VARS_4M.fd` |
| `python3` | boot/PE checks | standard library only |
| `cc` (`HOST_CC`) | `tools/mkcrypt` and the host unit tests | any host C11 compiler; the crypt tool reuses `kernel/crypto/*` |
| `sgdisk`, `mke2fs`, `debugfs`, `e2fsck`, `dd`, `truncate` | test disks | only for `make disk`, `make raid-disks`, `make raid10-disks` and their checks; `dd` also blanks repair spares; e2fsprogs |
| `xorriso`, `fsck.vfat` | bootable ISO | only for `make iso`/`check-iso`/`test-iso`/`usb`; the FAT image itself is written by `scripts/mkfat.py`, so mtools is not needed |

Run `make preflight` to check all of the above.

## Targets

```
make             # show this target list (same as `make help`)
make all         # kernel.elf, BOOTX64.EFI and the ESP tree
make run         # boot the ESP tree under QEMU + OVMF
make demo        # boot the Workbench desktop in a real QEMU window
make test        # boot, verify markers/version/AVX2, then e2fsck the disk
make test-net    # boot with a slirp virtio-net NIC and verify DHCP/ARP/ICMP
make test-tcp    # TCP client/server and an HTTP exchange, both directions
make test-net-dns  # resolve a name against a host DNS server over TCP
make test-usb-hotplug  # hotplug a USB keyboard into xHCI and type at it
make test-usb-mouse    # hotplug a USB mouse into xHCI and move the pointer
make test-audio        # stream PCM to a USB Audio Class device through the mixer
make test-gfx          # move a hotplugged USB mouse and verify the framebuffer cursor
make test-workbench    # drag a window on the software-rendered desktop
make test-workbench-menu  # open menus, select items and click gadgets
make iso         # build a bootable hybrid UEFI ISO (build/<arch>/crugos.iso)
make check-iso   # validate the ISO's El Torito, FAT ESP, MBR and file set
make test-iso    # boot that ISO from a USB stick and from a CD-ROM
make usb         # write the ISO to a USB block device (USB_DEV=/dev/sdX)
make test-raid   # boot a two-member RAID1 root and check both members
make test-raid10 # boot a four-member RAID10 root and check every member
make test-nvme   # boot from an NVMe namespace and check the image
make test-ahci   # boot from a SATA disk and check the image
make test-raid-degraded  # boot RAID1 with one member missing
make test-raid10-degraded  # boot RAID10 with one member per mirror pair
make test-raid-repair  # rebuild a blank RAID1 member at boot, then check both
make test-raid10-repair  # rebuild two blank RAID10 members, then check all four
make test-journal-csum  # crash after a journal commit, replay it with e2fsck
make test-journal-replay  # crash, then replay the journal in the kernel
make test-crypt  # boot from an AES-256-XTS root, decrypt it and check the image
make test-crypt-recovery  # unlock the recovery keyslot instead of slot 0
make test-crypt-wrongkey  # a wrong passphrase must fail closed (exit 35)
make test-crypt-tamper  # a corrupted keyslot MAC must be rejected
make test-crypt-ni  # the same encrypted boot with AES-NI enabled
make test-crypt-prompt  # type the passphrase on QEMU's serial console and boot
make test-crypt-prompt-wrong  # three wrong console passphrases must lock the volume
make test-crypt-prompt-timeout  # an unanswered prompt must fail closed
make test-crypt-keyboard  # type the passphrase on the PS/2 keyboard and boot
make test-crypt-keyboard-wrong  # three wrong keyboard passphrases must lock the volume
make crypt-run   # boot the encrypted disk in a QEMU window for real typing
make test-compat # boot a REQUIRE_AVX2=0 build on a non-AVX2 CPU model
make test-host   # host tests: string/printf/ELF/bitmap/VM/handles/CRC32C/AES/XTS/Argon2id/BLAKE2b/crypt slots/line editor/input scancodes/USB HID keyboard and mouse descriptors and reports/pointer state/IPv4/TCP/UDP checksums/DHCP parsing/DNS/audio mixer, tone and PCM ring/framebuffer cursor/intuition windows, menus and gadgets
make disk        # build the GPT/ext4 test disk at build/<arch>/disk.img
make raid-disks  # build the two GPT/ext4 RAID1 member disks
make raid10-disks  # build the four GPT/ext4 RAID10 member disks
make nvme-disk   # build the GPT/ext4 image used by test-nvme
make ahci-disk   # build the GPT/ext4 image used by test-ahci
make crypt-disk  # build the AES-256-XTS encrypted test image
make mkcrypt     # build the host crypt tool (format/extract)
make check-disk  # e2fsck and persisted-file checks on the current disk
make size        # kernel size budget (<1 MiB stretch, <2 MiB hard)
make check-efi   # PE32+ structure checks on the loader
make repro       # two independent builds must be byte-identical
make preflight   # toolchain/firmware availability
make version     # print the release version from VERSION
make options     # print every option with its current value
make clean       # remove build/<arch>
make distclean   # remove build/ and host test artifacts
```

Useful variables. Every row can be overridden on the command line; `make
options` prints the complete resolved set (including paths derived from
`BUILD_ROOT`).

| Variable | Default | Meaning |
| --- | --- | --- |
| `ARCH` | `x86_64` | only architecture implemented |
| `MODE` | `release` | `release`, `debug` or `size` |
| `V` | `0` | `V=1` prints full command lines |
| `MEMORY_MB` | `512` | guest RAM; see the firmware floor below |
| `SMP` | `1` | vCPUs (the phase 2 kernel uses the boot CPU only) |
| `TIMEOUT_SEC` | `30` | QEMU wall-clock limit for `make run`/`make test` |
| `QEMU_CPU` | `qemu64,+avx,+avx2` | `-cpu` model; empty means QEMU's default |
| `REQUIRE_AVX2` | `1` | `0` builds a kernel that boots without AVX2 |
| `EXPECT_AVX2` | `yes` | what `make test` requires the guest to report (`no` for compat) |
| `EXPECT_RAID` | `no` | what `make test` requires for RAID: `no`, `healthy` or `degraded` |
| `EXPECT_NVME` | `no` | what `make test` requires for NVMe (`yes` needs `nvme0n1`) |
| `EXPECT_AHCI` | `no` | what `make test` requires for AHCI/SATA (`yes` needs an `sd?` root) |
| `EXPECT_REBUILD` | `no` | number of RAID members the boot must rebuild; `no` also rejects an unexpected rebuild |
| `EXPECT_CRYPT` | `no` | encrypted-root expectation: `no`, `yes`, `wrong` (unlock must fail) or `locked` (the console prompt timed out) |
| `EXPECT_AES` | `any` | AES implementation the boot must report: `software`, `aes-ni` or `any` |
| `EXPECT_NET` | `no` | `yes` requires the `make test-net` DHCP/ARP/ICMP markers (`no` skips them) |
| `EXPECT_TCP` | `no` | `yes` requires the five `CRUGOS: tcp ...` markers written by `make test-tcp` |
| `EXPECT_DNS` | `no` | `yes` requires the five `CRUGOS: dns ...` markers written by `make test-net-dns` |
| `NET_MODE` | empty | `user` attaches a slirp user-mode virtio-net NIC (`-netdev user,id=net0`); empty means no NIC |
| `NET_HOSTFWD` | empty | forwarded port for the slirp NIC, e.g. `tcp:127.0.0.1:<port>-:8080`; the `tcp:` protocol prefix is mandatory |
| `TCP_SERVE_SEC` | `15` | how long the guest listener waits for the gate's connection (`net.serve=`) |
| `TCP_RUN_TIMEOUT_SEC` | `180` | wall-clock budget for `make test-tcp` (the TCP boot is slower than a plain one) |
| `DNS_NAME` | `crugos.test` | name the `make test-net-dns` boot resolves (`net.name=`) |
| `DNS_ADDRESS` | `10.0.2.99` | address the gate's DNS server answers with |
| `DNS_TTL` | `60` | TTL the gate's DNS server puts in its answer |
| `DNS_RUN_TIMEOUT_SEC` | `180` | wall-clock budget for `make test-net-dns` |
| `EXPECT_USB` | `no` | `yes` requires the `CRUGOS: usb ...` keyboard markers written by `make test-usb-hotplug` |
| `USB_WAIT` | `20` | seconds the USB gate boots with `usb.wait=` so the keyboard can be hotplugged late |
| `USB_KEYS` | `abc` | characters `make test-usb-hotplug` types and waits to see decoded |
| `USB_RUN_TIMEOUT_SEC` | `240` | wall-clock budget for `make test-usb-hotplug` |
| `EXPECT_MOUSE` | `no` | `yes` requires the `CRUGOS: usb mouse ...` markers written by `make test-usb-mouse` |
| `MOUSE_MOVES` | `2` | pointer reports `make test-usb-mouse` injects (one establishes the button baseline) |
| `MOUSE_WAIT` | `20` | seconds the mouse gate boots with `usb.mouse=` so the mouse can be hotplugged late |
| `MOUSE_RUN_TIMEOUT_SEC` | `240` | wall-clock budget for `make test-usb-mouse` |
| `EXPECT_AUDIO` | `no` | `yes` requires the `CRUGOS: audio ...` markers written by `make test-audio` |
| `AUDIO_RUN_TIMEOUT_SEC` | `240` | wall-clock budget for `make test-audio` |
| `AUDIO_QEMU_EXTRA` | `-device qemu-xhci,id=xhci -device usb-audio,audiodev=audio0 -audiodev none,id=audio0` | QEMU arguments that add the USB audio function on a hermetic backend |
| `EXPECT_GFX` | `no` | `yes` requires the `CRUGOS: gfx ...` cursor markers written by `make test-gfx` |
| `GFX_WAIT` | `40` | seconds the cursor gate boots with `gfx.wait=` so the mouse can be hotplugged late |
| `GFX_MOVES` | `3` | pointer moves `make test-gfx` injects and verifies in the framebuffer |
| `GFX_RUN_TIMEOUT_SEC` | `240` | wall-clock budget for `make test-gfx` |
| `GFX_QEMU_EXTRA` | `-device qemu-xhci,id=xhci` | QEMU arguments `make test-gfx` boots with (the gate adds its own `-qmp` socket) |
| `EXPECT_WB` | `no` | `yes` requires the `wb desktop ready` / `wb window` / `wb selftest ok` markers written by `make test-workbench` |
| `WB_WAIT` | `30` | seconds the desktop gate boots with `wb.wait=` so the mouse can be hotplugged late |
| `WB_DRAG` | `1` | title-bar drags `make test-workbench` performs and verifies |
| `WB_RUN_TIMEOUT_SEC` | `240` | wall-clock budget for `make test-workbench` |
| `WB_QEMU_EXTRA` | `-device qemu-xhci,id=xhci` | QEMU arguments `make test-workbench` boots with (the gate adds its own `-qmp` socket) |
| `EXPECT_MENU` | `no` | `yes` requires the `wb menu` / `wb gadget` / `wb action` / `wb selftest ok` markers written by `make test-workbench-menu` |
| `WB_MENU_WAIT` | `30` | seconds the menu gate boots with `wb.wait=` so the mouse can be hotplugged late |
| `WB_MENU_ACTS` | `4` | menu, gadget and close actions the menu gate performs and verifies |
| `WB_MENU_RUN_TIMEOUT_SEC` | `240` | wall-clock budget for `make test-workbench-menu` |
| `DEMO_DISPLAY` | `gtk` | QEMU display `make demo` opens (any `-display` value, for example `sdl` or `none`) |
| `DEMO_HOLD_SEC` | `600` | seconds `make demo` keeps the desktop on screen before the guest exits by itself |
| `DEMO_TIMEOUT_SEC` | `900` | wall-clock budget for the whole `make demo` boot |
| `DEMO_CMDLINE` | `wb.enable=1 wb.wait=0 wb.hold=$(DEMO_HOLD_SEC)` | `CMDLINE_EXTRA` the demo boot passes to the kernel |
| `DEMO_MACHINE_EXTRA` | `usb=off,i8042=off` | machine properties, so the demo keyboard and mouse are the only ones |
| `DEMO_QEMU_EXTRA` | `-device qemu-xhci,id=xhci -device usb-kbd,bus=xhci.0 -device usb-mouse,bus=xhci.0` | QEMU arguments that attach an xHCI with a keyboard and a mouse |
| `ISO_IMAGE` | `build/<arch>/crugos.iso` | hybrid UEFI ISO written by `make iso` |
| `ISO_STAGE` | `build/<arch>/iso` | scratch tree for the ISO build (wiped on every run) |
| `ISO_FAT_IMG` | `$(ISO_STAGE)/tree/boot/efi.img` | FAT16 ESP image used as the El Torito boot image |
| `ISO_SIZE_MB` | `24` | size of that FAT image; `mkfat.py` rejects anything outside the FAT16 range |
| `ISO_VOLID` | `CRUGOS` | ISO 9660 volume identifier and FAT label |
| `ISO_CMDLINE` | `storage=none` | guest command line baked into the image (never inherited from the ESP) |
| `ISO_DATE` | `2026010100000000` | fixed timestamp passed to `xorriso --set_all_file_dates` for a reproducible image |
| `ISO_RUN_TIMEOUT_SEC` | `240` | wall-clock budget for each of the two `make test-iso` boots |
| `EXPECT_ISO` | `no` | reserved for gates that check the ISO markers explicitly |
| `ISO_IMG` | empty | when set, `make run` boots this ISO as a CD-ROM instead of the ESP |
| `USB_IMG` | `build/<arch>/usb-stick.img` | the ISO `dd`'ed onto a file, used by the USB leg of `make test-iso` |
| `USB_STICK` | empty | when set, `make run` attaches this image as USB mass storage on an xHCI controller |
| `USB_DEV` | empty | absolute block device `make usb` writes to; must not be mounted |
| `USB_CONFIRM` | `no` | must be `yes` or `make usb` refuses to write |
| `CRASH_IMG` | `$(BUILD)/crash.img` | crash-gate image (journal commit, no checkpoint) |
| `CRASH_REPLAY_IMG` | `$(BUILD)/crash-replay.img` | copy the kernel replays on the second crash-gate boot |
| `CC` | `crugc` | compiler for kernel and loader objects |
| `LD` | `ld` | linker for the PE32+ loader |
| `PYTHON` | `python3` | host scripts (boot/disk/EFI checks) |
| `QEMU` | `qemu-system-x86_64` | emulator binary |
| `OVMF_CODE` | `/usr/share/OVMF/OVMF_CODE_4M.fd` | OVMF firmware code image |
| `OVMF_VARS` | `/usr/share/OVMF/OVMF_VARS_4M.fd` | OVMF variable store (copied per run) |
| `QEMU_ACCEL` | empty | `-accel` backend, e.g. `tcg` or `kvm`; empty lets QEMU choose |
| `QEMU_DISPLAY` | `none` | `-display` value; `none` keeps runs headless |
| `QEMU_EXTRA` | empty | extra whitespace-separated QEMU arguments |
| `MACHINE_EXTRA` | empty | comma-separated q35 properties appended to `-machine q35` (the USB gate uses `usb=off,i8042=off`) |
| `BUILD_ROOT` | `build` | output root; `test-compat` uses `build/compat` |
| `DISK_IMG` | `build/<arch>/disk.img` | raw disk image attached as `vda`; empty boots without virtio |
| `DISK_SIZE_MB` | `16` | size of the generated test disk |
| `RAID_DISK_A/B` | `build/<arch>/raid-a.img`, `raid-b.img` | RAID1 members attached as `vda`/`vdb` |
| `RAID10_DISK_A..D` | `build/<arch>/raid10-a.img`..`raid10-d.img` | RAID10 members attached as `vda`..`vdd` |
| `NVME_IMG` | `build/<arch>/nvme.img` | image attached as an NVMe namespace |
| `NVME_SERIAL` | `CRUGNVME0` | QEMU NVMe serial reported by OVMF |
| `AHCI_IMG` | `build/<arch>/ahci.img` | image attached as a SATA disk on `ide.1` |
| `CRYPT_IMG` | `build/<arch>/crypt.img` | encrypted-root image attached as `vda` |
| `CRYPT_TAMPER_IMG` | `build/<arch>/crypt-tamper.img` | copy of `CRYPT_IMG` with one MAC byte flipped |
| `CRYPT_PASS` | `crugos-test` | passphrase used by `mkcrypt`; when non-empty it is also written to the ESP `crugos/cmdline.txt` (empty forces the console prompt) |
| `CRYPT_RECOVERY_PASS` | `crugos-recovery` | recovery-slot passphrase sealed by `mkcrypt` |
| `CRYPT_M` | `4096` | Argon2id memory cost (KiB) for the test volume |
| `CRYPT_PROMPT_TIMEOUT_SEC` | `2` | per-attempt console prompt timeout used by the timeout gate |
| `CRYPT_PROMPT_WRONG_PASS` | `wrong-password` | passphrase typed three times by the wrong-passphrase gate |
| `CRYPT_RUN_DISPLAY` | `gtk` | `-display` value used by `make crypt-run`; override with `sdl`, `vnc=...` or `none` |
| `CRYPT_RUN_TIMEOUT_SEC` | `600` | QEMU timeout for `make crypt-run`, long enough to type a passphrase |
| `CMDLINE_EXTRA` | empty | extra tokens appended to the ESP `crugos/cmdline.txt`, e.g. `crypt.prompt_timeout=2` |
| `SERIAL_TCP` | empty | when set, QEMU exposes the serial console as `tcp:<value>` instead of a log file (used by the console-prompt gates; the keyboard gates instead pass a `-qmp unix:<sock>` argument through `QEMU_EXTRA`) |
| `MCRYPT` | `build/<arch>/mkcrypt` | host crypt tool (format/extract) |
| `HOST_CC` | `cc` | host compiler for `mkcrypt` and `tests/host` |
| `EXTRA_DISKS` | empty | space-separated images attached after `DISK_IMG` |

## Version stamping

`VERSION` is the source of truth. The makefile reads it once and passes
`-DCRUG_VERSION_STR="<version>"` to every kernel and loader object, so both
report the same number. Direct compiler invocations fall back to `0.0.0-dev`
via `include/crug/version.h`. The ESP tree also gets `crugos/version` as a
plain text file. `make test` compares the kernel's `CRUGOS: version` line with
`VERSION`; a mismatch fails the boot gate. Release procedure and tag rules are
in [versioning.md](versioning.md). Because the version is a compile-time
define, every kernel and loader object also depends on `VERSION`; changing it
forces a full recompile rather than only a relink.

## Kernel link

The kernel is a freestanding ELF64 executable linked at a fixed address:

```
KERNEL_BASE = 0x00100000
```

`kernel/arch/x86_64/linker.ld` lays out `.text`, `.rodata`, `.data` and `.bss` on
4 KiB boundaries and exports:

```
__text_start __text_end   __bss_start __bss_end __kernel_end
```

Link flags that matter:

```
-nostdlib -static -no-pie -Wl,--build-id=none -Wl,-T,kernel/arch/x86_64/linker.ld
```

`--build-id=none` keeps the image reproducible and removes a non-loadable note.
The script discards `.comment`, `.note*`, `.eh_frame`, `.rela*` and dynamic
sections; the kernel must not need runtime relocations. C sources are compiled
with `-fno-pic -fno-pie -fno-builtin -fno-stack-protector -mno-red-zone`, no
SSE/FPU (`-mno-sse -mno-sse2 -mno-mmx -mno-80387`) and no AVX
(`-mno-avx -mno-avx2 -mno-avx512f`). The compiler therefore never emits vector
state save/restore in interrupt-adjacent code before the context-switch path
owns XSAVE state. AVX2 detection and `XCR0` setup are described in
[cpu.md](cpu.md).

## Loader link

The UEFI loader is a PE32+ EFI application, not an ELF. It is linked with GNU
`ld` directly:

```
ld -m i386pep --subsystem 10 -e efi_main \
   --image-base 0x10000000 --disable-reloc-section --no-insert-timestamp
```

Hard-won constraints (each of these was a real OVMF failure):

1. `efi_main` and every firmware function pointer use the Microsoft ABI
   (`__attribute__((ms_abi))`, `EFIAPI`). Mixing SysV and MS ABI raises `#UD`
   on the first indirect firmware call.
2. Compile loader objects with `-fno-ident`. Otherwise `.comment` lands at RVA 0
   and OVMF refuses the image (`Load Error`).
3. Do not use `-ffunction-sections` + `--gc-sections`; the entry point can be
   garbage collected and firmware reports no output.
4. Do not use a custom PE linker script or `--enable-reloc-section`. ELF-derived
   `.reloc` contents are invalid here and OVMF crashes loading the image.
5. No `mtools` is required: QEMU's `vvfat` driver exposes the ESP directory
   directly. The process `TMPDIR` must be writable.

## Firmware memory floor

OVMF 4M needs a substantial amount of RAM before it will boot from QEMU's
`vvfat` ESP. Measured with this QEMU/OVMF pair:

| `MEMORY_MB` | Result |
| --- | --- |
| 16, 32 | QEMU exits 0; no serial output |
| 64, 128 | OVMF reports `No bootable option or device was found` |
| 256 | booted once, then failed the same way on a re-run |
| 512 | reliable; used by all gates |

This is a firmware limit, not a kernel limit: the kernel load image is about
290 KiB. The specification's 16 MiB headless target needs a leaner boot path and
is tracked in [roadmap.md](roadmap.md).

## Test disk

`make disk` runs [scripts/mkdisk.sh](../scripts/mkdisk.sh) and produces a GPT
image with one type-`8300` partition named `crugroot` and an ext4 filesystem
containing `/hello.txt`. The filesystem has 1 KiB blocks, a 1 MiB JBD2 journal
(`-J size=1`), `metadata_csum` enabled and `orphan_file` disabled.
[scripts/journal-csum.py](../scripts/journal-csum.py) then flips the journal to
checksum v3, because mke2fs 1.47 leaves the journal checksum flags clear.
`mkdisk.sh OUTPUT SIZE normal|crash` takes a third argument: `crash` also
creates `/simulate-crash`, which makes the boot self-test commit a
multi-descriptor transaction and exit before checkpointing. It requires
`sgdisk`, `mke2fs`, `debugfs`, `dd`, `truncate` and `python3`. The image is
attached as a legacy virtio-blk device:

```
-drive file=<disk.img>,if=none,id=disk0,format=raw
-device virtio-blk-pci,drive=disk0,disable-modern=on
```

`make run` builds the image if it is missing; `make test` always regenerates it
first. The kernel mounts it read-only during `storage_init`, then the boot self-test
remounts it read-write, creates and mutates files under `/selftest`, unmounts
cleanly and remounts read-only to verify the persisted data. It also queues a
dirty journal block, unmounts without clearing the journal, remounts
read-write and verifies replay. `make test` regenerates the image, boots the
kernel and then runs [scripts/check-disk.py](../scripts/check-disk.py), which
extracts the partition and requires `e2fsck -fn` to be clean (a leftover dirty
journal fails this gate). Because the image uses `metadata_csum` and journal
checksums, that `e2fsck` run also verifies every checksum the kernel wrote.
`make check-disk` runs only that disk check on the existing image.

The journal gates run on their own crash image:

- `make test-journal-csum` boots once (the guest exits after
  `CRUGOS: dirty journal committed (no checkpoint)` and
  `CRUGOS: journal committed 100 block(s) in 2 descriptor(s)`), then runs
  [scripts/check-journal.py](../scripts/check-journal.py): `e2fsck -fy` must
  replay the log, `e2fsck -fn` must be clean and `/selftest/j.bin` must hold
  the committed 100 KiB pattern.
- `make test-journal-replay` boots the crash image, copies it and boots again.
  The second boot must print `CRUGOS: journal replayed 100 block(s) at mount`
  and `CRUGOS: journal replay verified`; `e2fsck -fn` must then be clean.
- `make crash-disk` only builds that image.

The encrypted gates use an image with a type-`8f1c7d2e-...` `crugcrypt`
partition: `make crypt-disk` builds the plaintext ext4/JBD2 image with
`mkdisk.sh ... crypt`, and `mkcrypt format` writes the 16 KiB header, seals
keyslot 0 (`CRYPT_PASS`) and optionally slot 1 (`CRYPT_RECOVERY_PASS`), then
encrypts the 12 MiB data area in place. `make test-crypt` writes
`crypt.pass=$(CRYPT_PASS)` into the ESP command-line file, boots, requires
`CRUGOS: crypt ready` with the requested AES implementation, and then runs
`check-disk.py --crypt-tool mkcrypt --crypt-password ...`, which extracts and
decrypts the partition before `e2fsck -fn` and the file checks.
`make test-crypt-wrongkey` boots with a different passphrase and expects
`crypt unlock failed`, `CRUGOS: no root filesystem` and exit 35;
`make test-crypt-tamper` flips byte 1052776 (the MAC of keyslot 0) with
`scripts/tamper.py` and expects the same fail-closed result. See
[crypto.md](crypto.md) for the header layout, key hierarchy and limits.

The prompt gates run without a `crypt.pass=` token, so the kernel has to ask
the console. [scripts/run-crypt-prompt.py](../scripts/run-crypt-prompt.py)
reserves a free loopback port and starts
`make run SERIAL_TCP=127.0.0.1:<port>`; [scripts/run-qemu.sh](../scripts/run-qemu.sh)
then replaces its file-backed serial argument with
`-serial tcp:<addr>,server=on,wait=on`, so QEMU waits for the driver before
booting. The driver tees every byte into the normal serial log, counts
`passphrase:` prompts and types one answer per prompt. `make test-crypt-prompt`
sends `CRYPT_PASS` once and then checks the disk with `mkcrypt extract`;
`make test-crypt-prompt-wrong` sends `CRYPT_PROMPT_WRONG_PASS` three times and
expects `crypt locked (too many attempts)`; `make test-crypt-prompt-timeout`
appends `crypt.prompt_timeout=$(CRYPT_PROMPT_TIMEOUT_SEC)` (2 s by default) to
the command line, sends nothing, and expects `crypt locked (no passphrase)`.
Both failing gates require exit status 35, and every prompt gate fails if the
passphrase text appears in the serial log.

The keyboard gates keep the serial line write-only (no `SERIAL_TCP`) and inject
keystrokes through QEMU's QMP monitor instead:
[scripts/run-crypt-keyboard.py](../scripts/run-crypt-keyboard.py) starts
`make run QEMU_EXTRA=-qmp unix:<sock>,server=on,wait=off`, waits for the serial
log to contain `passphrase: `, then sends one QMP `send-key` event per character
(`{"type": "qcode", "data": "<key name>"}`, 40 ms hold and 40 ms gap) and
finally `ret`. `make test-crypt-keyboard` types `CRYPT_PASS` and then checks the
decrypted image; `make test-crypt-keyboard-wrong` types
`CRYPT_PROMPT_WRONG_PASS` once per prompt, requires three prompts,
`crypt locked (too many attempts)` and exit status 35, and asserts that the
secret never appears in the serial log. The path needs the guest to print
`CRUGOS: input ready (serial + keyboard)`; see [input.md](input.md).

`make test-net` needs no extra image: it boots the normal GPT/ext4 disk with
`NET_MODE=user`, so QEMU attaches a legacy virtio-net NIC (`-netdev user,id=net0`
+ `-device virtio-net-pci,netdev=net0,disable-modern=on,romfile=`) behind a
slirp stack that provides DHCP, DNS and a gateway. The guest must print
`virtio : net eth0 mac ...`, `CRUGOS: net ready (1 device(s))`, a
`CRUGOS: dhcp lease 10.0.2.15/255.255.255.0 gw 10.0.2.2 dns 10.0.2.3` line, an
`CRUGOS: arp resolved 10.0.2.2 ...` line, `CRUGOS: icmp echo reply from 10.0.2.2`
and `CRUGOS: net selftest ok`; `check-disk.py` then runs on the same disk as in
`make test`. See [network.md](network.md).

`make test-tcp` goes further: `scripts/run-net-tcp.py` starts an HTTP responder
on a free `127.0.0.1` port (the guest reaches it as `10.0.2.2`), passes
`net.tcp=<port> net.serve=<seconds>` through `CMDLINE_EXTRA`, and forwards a
host port to the guest's port 8080 with `NET_HOSTFWD=tcp:127.0.0.1:<port>-:8080`
so the script can fetch the guest listener. The gate requires at least two guest
client connections, a successful fetch of the guest's `HTTP/1.0 200 OK`, the
five `CRUGOS: tcp ...` markers (`EXPECT_TCP=yes`) and a clean `e2fsck`. The
boot exits 33 on success and the driver prints
`ok: tcp client/server http (N echo connection(s), M byte(s) fetched from the
guest, exit 33)`.

`make test-net-dns` adds the resolver: `scripts/run-net-dns.py` binds a TCP DNS
responder on a free `127.0.0.1` port (the guest reaches it as `10.0.2.2`; slirp
routes TCP there but not UDP), passes
`net.dns=10.0.2.2:<port> net.dns-tcp=1 net.name=<host>` through
`CMDLINE_EXTRA`, and requires the five `CRUGOS: dns ...` markers
(`EXPECT_DNS=yes`), a `resolved <host> -> <address> via tcp` line, at least one
query arriving at the server and a clean `e2fsck`. The driver prints
`ok: dns resolve over tcp (N query(s), <host> -> <address>, exit 33)`.

`make test-usb-hotplug` proves the phase 5A keyboard hotplug path:
[scripts/run-usb-hotplug.py](../scripts/run-usb-hotplug.py) boots the normal disk
with `MACHINE_EXTRA=usb=off,i8042=off` (so the machine has no built-in USB
controller and no PS/2 port), adds `-device qemu-xhci,id=xhci` through
`QEMU_EXTRA` plus a QMP socket, and passes `usb.wait=$(USB_WAIT)
usb.keys=<len>` through `CMDLINE_EXTRA`. It waits for
`CRUGOS: usb controller ready`, plugs `usb-kbd` into `xhci.0` with `device_add`,
waits for `CRUGOS: usb keyboard ready`, then sends one QMP `send-key` per
character and waits for the matching `CRUGOS: usb key '<c>'` line (falling back
to `input-send-event` addressed at `/machine/peripheral/kbd0` if the console
routes keystrokes elsewhere). The gate requires exit status 33,
`CRUGOS: usb selftest ok`, the five `usb` markers (`EXPECT_USB=yes`) and a clean
`e2fsck`; the driver prints
`ok: usb hotplug keyboard (N key(s) <keys>, port hotplug, exit 33)`.
`make test-usb-mouse` proves the phase 5B pointer path with the same machine
setup: [scripts/run-usb-mouse.py](../scripts/run-usb-mouse.py) plugs `usb-mouse`
into `xhci.0`, passes `usb.mouse=$(MOUSE_WAIT) usb.moves=$(MOUSE_MOVES)` through
`CMDLINE_EXTRA`, and injects relative motion with QMP `input-send-event`. One
QMP call produces exactly one HID report and the first report only sets the
button baseline, so the script moves, waits for `CRUGOS: usb mouse move`, and
only then presses the left button and waits for `CRUGOS: usb mouse button`. The
event is broadcast rather than addressed at the device, because QEMU resolves a
named `device` through its graphics console table and a USB device is not
found there. The gate requires exit status 33, `CRUGOS: usb selftest ok`, the
mouse markers (`EXPECT_MOUSE=yes`) and a clean `e2fsck`; the driver prints
`ok: usb hotplug mouse (N move(s), 1 button(s), port hotplug, exit 33)`.
See [usb.md](usb.md).

`make test-audio` proves the phase 5C audio path with the same machine setup plus
`AUDIO_QEMU_EXTRA`, which adds `-device qemu-xhci`, `-device usb-audio` and an
`-audiodev none,id=audio0` backend, so the gate needs no host sound server and is
hermetic. The boot carries no extra token: the driver attaches when the
enumerated configuration has an interface of class `0x01`, selects alternate
setting 1, keeps 32 isochronous packets in flight and fills them from the PCM
ring; `audio_selftest()` streams `audio.seconds=` (0.5 s) of a 440 Hz tone and
requires real pacing. The gate demands exit status 33,
`CRUGOS: audio selftest ok`, a `audio streamed N frame(s) in M ms` line, the
`audio` markers (`EXPECT_AUDIO=yes`), either captured frames or the
`CRUGOS: audio capture unavailable (no input endpoint)` marker (this QEMU build
exposes playback only) and a clean `e2fsck`; the boot-check prints
`ok: boot markers found, version 0.21.0 ... audio yes ...`.

`make test-gfx` proves the phase 5D graphics path with the same machine setup as
the USB gates (`usb=off,i8042=off`) plus `GFX_QEMU_EXTRA`, which adds
`-device qemu-xhci`; [scripts/run-gfx-cursor.py](../scripts/run-gfx-cursor.py)
adds its own `-qmp unix:<tmp>/qmp.sock` argument, boots with
`gfx.wait=$(GFX_WAIT) gfx.moves=$(GFX_MOVES)` through `CMDLINE_EXTRA`, and
hotplugs `usb-mouse`. For every move it takes a QEMU `screendump` before and
after, parses the P6 PPM and checks that the amber sprite centroid
(`0xffd040`) moved by the same delta the guest reported, within 2.0 pixels. The
gate requires exit status 33, `CRUGOS: gfx selftest ok`, the `gfx` markers
(`EXPECT_GFX=yes`) and a clean `e2fsck`; it prints
`ok: gfx cursor (3 move(s) verified in the framebuffer, exit 33)`. See
[gfx.md](gfx.md).

`make test-workbench` proves the phase 6A desktop with the same machine setup
(`usb=off,i8042=off`) plus `WB_QEMU_EXTRA`;
[scripts/run-workbench.py](../scripts/run-workbench.py) boots with
`wb.enable=1 wb.wait=$(WB_WAIT) wb.drag=$(WB_DRAG)` through `CMDLINE_EXTRA`, so
the framebuffer console is taken over by the desktop and two demo windows are
opened. It hotplugs `usb-mouse`, moves the pointer onto the title bar of window
2, presses the left button, injects two `(+45, +25)` motion events and
releases. The grab offset `(32, 12)` means the window follows with the pointer
minus that offset. The gate then reads a `screendump` and requires the active
title bar amber at the new origin, the old bar position idle grey and the frame
of window 1 still white, which also pins the RGB packing of the framebuffer
primitives. It requires exit status 33, `CRUGOS: wb selftest ok`, the `wb`
markers (`EXPECT_WB=yes`) and a clean `e2fsck`; it prints
`ok: workbench title-bar drag (1 drag(s), window 2 at (570, 350), exit 33)`.
See [workbench.md](workbench.md).

`make test-workbench-menu` proves the phase 6B interaction layer on the same
machine setup, with [scripts/run-workbench-menu.py](../scripts/run-workbench-menu.py)
in place of the drag driver. It boots with
`wb.enable=1 wb.wait=$(WB_MENU_WAIT) wb.menu=1 wb.acts=$(WB_MENU_ACTS)`, so the
desktop also waits for the programmed number of menu and gadget actions. The
script clicks window 2's `Shell` strip, takes a `screendump` to prove the popup
row under the pointer is highlighted amber while the next row keeps the menu
grey, selects `New`, clicks the `Run` gadget of window 1, closes window 2 with
its close gadget (verified by two pixels of a second `screendump`: desktop
background where the window was, white frame of window 1), then opens window
1's `Workbench` strip and selects `Quit`. It requires exit status 33,
`CRUGOS: wb selftest ok (N drag(s), M action(s))` with at least one action, the
`menu` markers (`EXPECT_MENU=yes`) and a clean `e2fsck`; it prints
`ok: workbench menu and gadget actions (4 action(s), exit 33)`. Every pointer
move is issued in steps of at most 100 pixels, because QEMU splits a larger
relative motion into 8-bit HID reports and the guest drops some of them.

## Interactive demo

`make demo` is not a gate but a way to watch the system: it boots
`DEMO_QEMU_EXTRA` (an xHCI controller with a `usb-kbd` and a `usb-mouse`
already plugged in) into a real `DEMO_DISPLAY` window (GTK by default) instead
of the headless gates. The command line carries
`wb.enable=1 wb.wait=0 wb.hold=$(DEMO_HOLD_SEC)`:

- `wb.enable=1` hands the framebuffer to the phase 6B desktop and opens the two
  demo windows.
- `wb.wait=0` turns the desktop self-test into a no-op, so the demo does not
  require a title-bar drag before the gate marker; the interactive desktop is
  the point, not the self-test.
- `wb.hold=<seconds>` keeps the guest alive after `CRUGOS: phase 6b boot ok`
  (`wb_hold()`), polling input once per timer tick, so the window stays on
  screen until the deadline or until you close it.

Inside the window the USB keyboard and the USB mouse are live: pointer motion
prints `CRUGOS: usb mouse move dx=.. dy=..` on the serial console, a left
press on a title bar starts a drag, and `Ctrl+Alt+G` releases the pointer grab
so you can leave the window. The guest then exits through `isa-debug-exit`
with status 33, exactly like the gates, and `build/x86_64/serial.log` plus
`build/x86_64/exit-code.txt` keep the record. Passing `DEMO_DISPLAY=none`
turns the same target into a headless demo.

## Bootable ISO

`make iso` assembles `build/<arch>/crugos.iso`, a hybrid image that boots as
a CD-ROM (El Torito, no emulation) and after a plain `dd` onto a USB stick (an
MBR with one type-`0xEF` ESP partition covering the same FAT image). The ESP
travels as a FAT16 image built by `scripts/mkfat.py` -- this host has no
mtools, so the file system is written directly and deterministically -- and
`scripts/mkiso.sh` drives `xorriso -as mkisofs` with the El Torito entry,
`-part_like_isohybrid` and `--protective-msdos-label`.

The image ships its own command line: `mkiso.sh` always rewrites
`crugos/cmdline.txt` from `ISO_CMDLINE` (default `storage=none`), so a stick
never inherits the QEMU `crypt.pass=` token. `storage=none` is a kernel token
that lets `storage_init()` probe every transport and then print
`CRUGOS: storage skipped (storage=none)` instead of demanding a root
filesystem.

`make check-iso` re-parses the artifact offline: ISO 9660 primary volume
descriptor, El Torito boot record and catalog checksum, the FAT16 BPB at the
catalog's load address, the protective MBR's ESP entry, an `xorriso -osirrox`
extraction of `/boot/efi.img` byte-compared against the catalog range,
`fsck.vfat -n`, and the four files with their command line. `make test-iso` is
the gate: it `dd`s the ISO onto `USB_IMG`, boots that file through
`usb-storage` on `qemu-xhci`, boots the ISO itself as a CD-ROM, and runs
`scripts/boot-check.py --storage=none` on both serial logs, which swaps the
storage/GPT/ext4/journal markers for the single skip marker. `make usb` writes
the image to a real device and needs `USB_DEV=/dev/sdX USB_CONFIRM=yes`; it
refuses a device that is mounted.

See [iso.md](iso.md) for the on-disk layout, why the image ends up with an MBR
instead of a GPT, and the reproducibility rules.

The supported ext4 subset, write path, checksum layout, journal format and
failure modes are documented in [storage.md](storage.md) and
[checksums.md](checksums.md).

`make test-nvme` regenerates `build/<arch>/nvme.img` with `mkdisk.sh`, boots
with only `NVME_IMG` attached to QEMU's `nvme` device, requires
`CRUGOS: nvme ready` plus `nvme0n1`, and then runs `check-disk.py` on the image.
The driver maps the 64-bit BAR above 4 GiB, disables any controller state left
by OVMF, creates queues and identifies namespace 1; see [nvme.md](nvme.md).

`make test-ahci` regenerates `build/<arch>/ahci.img` and attaches it to the
q35 onboard AHCI controller as `ide.1` (`-device ide-hd,drive=ahci0,bus=ide.1`),
so the firmware's ESP disk stays on port 0. The gate requires
`CRUGOS: ahci ready`, an `ahci   : sd? <n> sectors` line, a
`CRUGOS: root device sd?` line and then `check-disk.py` on the image. See
[ahci.md](ahci.md) for the BAR mapping and port link-timeout details.

`make raid-disks` runs [scripts/mkraid.sh](../scripts/mkraid.sh) and
[scripts/raid-meta.py](../scripts/raid-meta.py) to build two 16 MiB GPT disks
with one type-`fd00` `crugraid` partition each, identical ext4 data and
checksummed RAID1 metadata in the last two sectors. `make test-raid` boots
with both images and requires a healthy array; `make test-raid-degraded` boots
with only `RAID_DISK_A` and requires a degraded boot.
[scripts/check-raid.py](../scripts/check-raid.py) validates both metadata
copies and geometry, extracts the data region, runs `e2fsck -fn`, checks the
persisted files, and after a healthy run compares the two data regions byte for
byte. The metadata format, assembly rules and limits are in
[raid.md](raid.md).

`make raid10-disks` runs [scripts/mkraid10.sh](../scripts/mkraid10.sh) with
the same `raid-meta.py` helper to build four 16 MiB members with slots 0..3,
level 10 and a 128-sector chunk. `make test-raid10` attaches all four images
and requires `raid healthy (4/4 members)`; `make test-raid10-degraded` attaches
only `RAID10_DISK_A` and `RAID10_DISK_C` (slots 0 and 2, one member per mirror
pair) and requires `raid degraded (2/4 members)`.
[scripts/check-raid10.py](../scripts/check-raid10.py) validates the metadata,
unique slots and pair coverage, rebuilds the logical array from one member per
pair, runs `e2fsck -fn` and checks the persisted files on every member.

`make test-raid-repair` builds the two RAID1 members and then zeroes the whole
`crugraid` partition of `RAID_DISK_B` with
[scripts/raid-blank.sh](../scripts/raid-blank.sh), keeping its GPT so the disk
still looks like a replacement member. The boot gate requires
`CRUGOS: raid rebuilt (1 member(s))` and a healthy array, and
`check-raid.py` then validates the freshly written metadata on both members.
`make test-raid10-repair` does the same for slots 1 and 3 and requires
`raid rebuilt (2 member(s))`; all four members are checked afterwards. Because
the rebuilt metadata and data are written by the kernel, these gates also cover
the metadata-write and flush paths.

## Reproducibility

`make repro` builds `kernel.elf` and `BOOTX64.EFI` twice under different
`BUILD_ROOT`s and compares them byte-for-byte. Any timestamp, path or hash
randomisation that leaks into the output will fail this target.

## Troubleshooting

- **`Load Error` from OVMF**: inspect `.comment`/first section alignment and
  confirm `-fno-ident` and `--disable-reloc-section`.
- **`!!!! Find image based on IP(...)`**: invalid or stripped relocation data;
  rebuild the loader with the exact link line above.
- **No serial output at all**: OVMF may have printed only to the framebuffer;
  check `build/<arch>/debug.log` (port `0xE9`) and re-run with `-display none`
  omitted.
- **`ExitBootServices failed`**: the memory map changed between snapshot and
  call; the loader retries three times, re-reading the map each time.
- **QEMU resets in a loop / no output after the handoff**: a page-table bug.
  Re-run with `-d int,cpu_reset -D qemu-int.log` and look for the first
  exception (`v=0e` with `e=0011` is an NX fault on instruction fetch, a common
  symptom of setting `NX` on a page-directory entry that points at a page
  table).
- **`#PF` in the kernel with a present+instruction-fetch error**: check that
  `.text` is mapped executable and that every page-table-referencing PDE has
  `NX` clear.
- **No boot at low `MEMORY_MB`**: see the firmware memory floor above; use
  `MEMORY_MB=512`.
- **`virtio : queue setup failed`**: the disk was attached as a modern-only
  device or without `disable-modern=on`; use the QEMU flags above.
- **`ahci   : BAR setup failed`**: the 32-bit BAR mask must set the upper 32
  bits (`mask |= 0xFFFFFFFF00000000ull`) before `~mask + 1`; a BAR inside the
  identity map must be reprotected with `vm_protect_page()` instead of
  `vm_map_page()`. See [ahci.md](ahci.md).
- **SATA boot spends seconds in `ahci_init`**: an empty port was polled with
  the full 1000 ms command timeout; empty ports use `AHCI_LINK_TIMEOUT_MS`
  (20 ms) instead.
- **NVMe timeout or `controller not ready`**: check CSTS at `0x1C` and AQA at
  `0x24`; a controller left enabled by OVMF must be disabled and RDY polled
  before the queues are programmed. Create SQ status `0x8201` means the
  Create CQ/SQ dwords are reversed (queue id low 16, size high 16). See
  [nvme.md](nvme.md).
- **`raid   : metadata invalid`**: a member's last two sectors hold a non-zero
  record that disagrees on magic, CRC, level, chunk, geometry, UUID, generation
  or slot. Fully blank metadata is a cold spare and is rebuilt instead.
  Regenerate with `make raid-disks` or `make raid10-disks`; inspect with
  `scripts/check-raid.py` or `scripts/check-raid10.py`.
- **`raid   : mirror pair N has no member`**: a RAID10 array lost both members
  of one pair, so there is no rebuild source. Regenerate the images with
  `make raid10-disks`.
- **`raid   : rebuild of <name> failed`**: the copy from the mirror failed; the
  slot stays missing and the array boots degraded. The serial log shows the
  failing read or write; check the disk image and the virtio/NVMe/AHCI path.
- **Unexpected rebuild in a plain gate**: `boot-check.py` fails any non-repair
  test that prints `CRUGOS: raid rebuilt`. Blank metadata is only expected after
  `make raid-repair-disks`/`raid10-repair-disks`; regenerate with
  `make raid-disks`/`raid10-disks`.
- **Healthy test boots degraded**: a virtio device did not appear or its
  metadata was rejected. Check the serial `block ready (N device(s))` line and
  QEMU's extra `-drive` pairs; the test scripts pass `EXTRA_DISKS`. A RAID10
  degraded gate is expected to show `2/4`.
- **`make test-raid` or `make test-raid10` fails e2fsck but the array
  booted**: members diverged or chunks were assembled from the wrong pair.
  `check-raid.py`/`check-raid10.py` report the mismatch; this indicates a
  mirror/stripe write-path bug, not a filesystem bug.
- **`storage: writable test failed`**: the in-kernel write path failed. The
  serial log names the step; `make check-disk` shows whether the on-disk state
  or the file contents are wrong. A bitmap-offset bug shows up as
  `Multiply-claimed block(s)` in `e2fsck`.
- **`e2fsck` reports bitmap differences or wrong link counts**: block bitmap bits
  are relative to the group's first block (`s_first_data_block + group *
  blocks_per_group`), not absolute block numbers. Check the allocation code in
  `kernel/fs/ext4.c`.
- **`storage: ext4 mount failed`**: the image has an unsupported feature or a
  dirty journal on a read-only mount. Extract the partition with `dd` and
  inspect it with `dumpe2fs -h`; `make disk` regenerates a known-good image.
- **`storage: journal selftest failed`**: replay did not restore the queued
  block. Check the descriptor/commit sequence and `s_start` handling in
  `kernel/fs/journal.c`; a read-only mount of a dirty journal is refused by
  design.
- **`e2fsck` reports a dirty journal or needs recovery**: the kernel did not
  checkpoint/clear `s_start` before unmount. The journal superblock is
  big-endian; inspect bytes 24..31 at the journal inode's first block.
- **`e2fsck` reports a checksum error on a directory or inode**: the tail or
  inode checksum was not recomputed after a modify-in-place update. The
  directory tail chains the inode generation; see
  [checksums.md](checksums.md) and `ext4_dir_set_tail()`.
- **`e2fsck` says "Journal superblock is corrupt"**: `s_checksum_type` at
  journal-superblock byte 0x50 is a single byte that must be 4, and the
  checksum over all 1024 bytes must be seeded with `0xFFFFFFFF` (not the UUID
  seed).
- **`make test-journal-csum` fails to find the descriptor line**: the crash
  transaction must queue at least 100 blocks; check `storage_crash_test()` in
  `kernel/fs/storage.c` and that the image was built with `mkdisk.sh ... crash`.
- **`make test-journal-replay` finds no replay**: the second boot did not have
  to recover, which means the first boot checkpointed. Ensure
  `journal_test_skip_checkpoint()` is active and that
  `ext4_test_mark_dirty()` ran before the commit (the superblock must carry
  `needs_recovery`, incompat bit 0x4).
- **`CRUGOS: crypt unlock failed` on `make test-crypt`**: the passphrase in the
  ESP `crugos/cmdline.txt` differs from the one sealed into the image. The
  makefile writes `crypt.pass=$(CRYPT_PASS)` when it builds the ESP and the
  variable is non-empty; rebuild with the same `CRYPT_PASS` used by
  `crypt-disk`. An empty `CRYPT_PASS` removes the file and the kernel prompts
  on the serial console instead.
- **No prompt in `make test-crypt-prompt`**: a `crypt.pass=` token is still on
  the ESP. Run with `CRYPT_PASS=` (the gate does) or delete
  `build/<arch>/esp/crugos/cmdline.txt`.
- **The prompt times out before an answer arrives**: `crypt.prompt_timeout`
  defaults to 60 s per attempt; the timeout gate lowers it to
  `CRYPT_PROMPT_TIMEOUT_SEC` (2 s). Raise that variable when debugging by hand.
- **`prompt: serial socket did not open`**: `make run` failed before QEMU started
  listening, or another process holds the port. The driver prints the full
  `make` command; run it manually to see the QEMU error. Do not pass
  `-serial` through `QEMU_EXTRA` while `SERIAL_TCP` is set: the run script
  owns that argument.
- **A passphrase appears in the serial log**: this must never happen. The boot
  prints only the `*` mask and the driver asserts the typed secret is absent;
  the prompt gates fail otherwise.
- **`CRUGOS: input ready (serial only)`**: `keyboard_init` did not find an i8042
  controller (the status port read `0xFF`) or it did not accept the
  configuration byte. The gates rely on the q35 PS/2 keyboard; without it only
  the serial line can type the passphrase.
- **Keyboard gate: `QMP error: CommandNotFound` for `sendkey`**: the QMP command
  is `send-key` and its `keys` argument is an array of KeyValue objects, not a
  string. See `scripts/run-crypt-keyboard.py`.
- **Keyboard gate waits forever at the prompt**: the serial line must stay
  write-only, otherwise `serial_try_getc()` consumes the input path first. Do not
  set `SERIAL_TCP` for these gates and pass `CRYPT_PASS=` so no `crypt.pass=`
  token is written to the ESP.
- **Keyboard gate types into nothing**: QEMU's `-display none` still owns a PS/2
  keyboard on q35, but the QMP socket must be up before the prompt appears; the
  driver waits for both the socket and the `passphrase: ` marker.
- **Tamper gate boots successfully**: the flipped byte must land in the
  partition, not in the image's GPT area. The gate uses offset
  `1052776 = 2048*512 + 0x1000 + 0x68` (keyslot 0 MAC); `scripts/tamper.py`
  prints the byte it flipped, and `mkcrypt extract` failing is the expected
  proof.
- **`crypt aes: software` in `make test-crypt-ni`**: the QEMU CPU model must
  include `+aes`; the target passes `QEMU_CPU=qemu64,+aes,+avx,+avx2`, and the
  guest log must report `crypt aes: aes-ni`.
- **Encrypted boot is much slower than the plain gates**: Argon2id with
  `m=4096, t=3` costs roughly 200 ms under TCG. Reduce `CRYPT_M` for faster
  local iterations, but keep the release gate at the default.
- **`mkcrypt: ... short read` / key mismatch**: `mkcrypt format` expects a
  plaintext filesystem image no larger than the data area
  (`volume_sectors - 32` sectors); regenerate with `make crypt-disk`.
- **No `virtio : net` line in a network gate**: `NET_MODE` was not `user`, so
  QEMU attached no NIC. `make test-net` passes it; for a manual run use
  `make run NET_MODE=user`. Without a NIC the kernel prints
  `CRUGOS: net selftest skipped (no network device)` and still boots.
- **`CRUGOS: net selftest failed (dhcp)`**: the slirp DHCP exchange did not
  complete. Check that the NIC line and `net ready (1 device(s))` appear before
  the failure, that `-netdev user,id=net0` is present, and that no other QEMU
  user-mode network is bound to the same options; the client retries three
  DISCOVER/OFFER/REQUEST/ACK rounds.
- **`CRUGOS: net selftest failed (tcp connect|tcp http|tcp serve)`**: the
  guest could not reach the gate's responder. Check that `net.tcp=<port>` is on
  the ESP command line (`CMDLINE_EXTRA`), that the responder is still listening
  (it runs for the gate's duration), and that `QEMU_EXTRA` did not replace the
  slirp `-netdev`. A `tcp http served` failure means the host fetch never
  arrived: verify `NET_HOSTFWD` uses the `tcp:` prefix and that port 8080 is not
  bound by another guest process.
- **`Invalid host forwarding rule ... (Bad protocol name)`**: QEMU requires the
  protocol in `hostfwd`, e.g. `tcp:127.0.0.1:8081-:8080`, not
  `127.0.0.1:8081-:8080`.
- **`net selftest failed (tcp socket)`**: the syscall leg failed even though the
  kernel-level client worked, so the handle table or `SYS_SOCKET_*` numbers are
  wrong; check `include/crug/syscall.h` and `kernel/core/syscall.c`.
- **`net selftest failed (arp)` or `(icmp)`**: the lease was not `10.0.2.15/24`
  with gateway `10.0.2.2`, or the ARP reply never arrived. Slirp answers both
  for the default `-netdev user` configuration; a custom `QEMU_EXTRA` subnet or
  `-netdev` must provide its own gateway, and the self-test pings whatever DHCP
  reports as the router.
- **QEMU exits before OVMF starts**: verify the firmware paths and that
  `OVMF_VARS` is writable (the run script copies it into `build/`).
- **Guest still reports the old version after a bump**: ensure `VERSION` is a
  prerequisite of the object files, not just of the link. The makefile has
  `$(KERNEL_OBJECTS): VERSION` and `$(LOADER_OBJECTS): VERSION` for this
  reason; a stale build tree otherwise keeps the old string.
