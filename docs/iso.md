# Bootable media: `make iso`, `check-iso`, `test-iso` and `usb`

CrugOS normally boots from an ESP directory that QEMU exposes as a writable
vvfat disk (`make run`, `make test`). That is fine for development, but it is
not something you can put on a USB stick. `make iso` turns the same ESP tree
into a single hybrid image that boots both as a CD-ROM and after a plain `dd`
onto a USB block device:

```
make iso        # build/x86_64/crugos.iso
make check-iso  # validate the artifact offline
make test-iso   # boot it from a USB stick and from a CD-ROM
make usb        # write it to a real device (USB_DEV=/dev/sdX USB_CONFIRM=yes)
```

The image is a hybrid because the two ways of booting look at different
structures: an El Torito *no-emulation* entry boots the CD path, and an MBR
with a single EFI System Partition entry covers the same FAT image so firmware
that treats the stick as a partitioned disk finds `EFI/BOOT/BOOTX64.EFI`.

## What the image contains

```
ISO 9660 volume CRUGOS (level 3)
  /boot/efi.img        FAT16 file system, the El Torito boot image
  /EFI/BOOT/BOOTX64.EFI  the same files, grafted at the ISO root
  /CRUGOS/KERNEL.ELF
  /CRUGOS/VERSION
  /CRUGOS/CMDLINE.TXT
```

| Element | Value |
| --- | --- |
| Volume identifier | `ISO_VOLID`, default `CRUGOS` |
| El Torito | no emulation, platform EFI, boot image `/boot/efi.img` |
| FAT image | FAT16, 512-byte sectors, 8 sectors per cluster, 2 FATs, 512 root entries, label `CRUGOS` |
| FAT image size | `ISO_SIZE_MB`, default 24 MiB (6133 clusters, 59 used) |
| Partition table | protective MBR with one type-`0xEF` partition that starts at the El Torito image |
| Guest command line | `ISO_CMDLINE`, default `storage=none` |

`ISO_CMDLINE` exists because `make iso` reads the *built* ESP tree, whose
`crugos/cmdline.txt` may carry a QEMU-specific `crypt.pass=` token and a test
disk expectation. [scripts/mkiso.sh](../scripts/mkiso.sh) always rewrites that
file from `ISO_CMDLINE` (which defaults to `storage=none`), so an image that
ships on a stick never contains a test passphrase and never demands a root
filesystem. Pass `ISO_CMDLINE=` to ship an empty command line, or
`ISO_CMDLINE='crypt.pass=...'` to bake a passphrase in (not recommended).

## Boot with no block device

A stick carrying only the image has no ext4 root, so the kernel needs a way to
say "there is nothing to mount here, and that is fine". The
`storage=none` token does exactly that (see [storage.md](storage.md)):
`storage_init()` still probes virtio-blk, NVMe and AHCI, prints their `ready`
lines, then prints

```
CRUGOS: storage skipped (storage=none)
```

and returns success without a root. `storage_selftest()` turns into a soft
skip, so the boot contract still ends with `CRUGOS: selftest ok`. This is not a
weaker gate for normal boots: the disk images used by `make test` never pass
the token, and a machine without a block device *and* without the token still
fails closed with `CRUGOS: no root filesystem`.

## Building the image

`make iso` depends on `make esp`, so the loader, the kernel, `crugos/version`
and `crugos/cmdline.txt` are current before the image is assembled.
[scripts/mkiso.sh](../scripts/mkiso.sh) then:

1. wipes `ISO_STAGE` (`build/x86_64/iso`) and copies the ESP tree aside;
2. rewrites `crugos/cmdline.txt` from `ISO_CMDLINE`;
3. builds the FAT16 boot image with [scripts/mkfat.py](../scripts/mkfat.py);
4. refuses to continue unless `fsck.vfat -n` reports it clean;
5. copies the same files to the ISO root as well, and
6. runs `xorriso -as mkisofs` with the El Torito entry and the hybrid MBR.

```
xorriso -as mkisofs -iso-level 3 \
    -V CRUGOS \
    -e boot/efi.img -no-emul-boot -eltorito-platform efi \
    -part_like_isohybrid \
    -iso_mbr_part_type C12A7328-F81F-11D2-BA4B-00A0C93EC93B \
    --protective-msdos-label \
    -o build/x86_64/crugos.iso build/x86_64/iso/tree
```

### Why not mtools, and why our own FAT writer

Writing a FAT image normally means `mtools` (`mformat` + `mcopy`), which is not
installed here. `scripts/mkfat.py` writes the file system directly instead, and
that has two side benefits:

* the image is deterministic -- entries are emitted in sorted order, every
  timestamp is fixed (`2026-01-01 00:00:00`), unused bytes stay zero, and the
  two FATs are byte-identical, so the same ESP tree always produces the same
  bytes;
* the layout is asserted rather than hoped for: only 8.3 names are accepted
  (a longer name is a hard error, `mkfat: name does not fit 8.3: <name>`), the
  cluster count must land in the FAT16 range (4085..65524), and the caller can
  list (`--list`) or read (`--read`) files back out of an existing image.

`Efi/Boot/BOOTX64.EFI` and `crugos/kernel.elf` both fit 8.3, so the restriction
costs nothing today; it is there to keep a future long name from being silently
truncated into a boot failure.

## Checking the image without booting

`make check-iso` runs [scripts/check-iso.py](../scripts/check-iso.py), which
parses the artifact instead of trusting the recipe:

* size is a multiple of 2048 bytes and at least 2 MiB;
* the ISO 9660 primary volume descriptor at sector 16 has volume id `CRUGOS`;
* a boot record descriptor with `EL TORITO SPECIFICATION` names the catalog
  sector, and the catalog's validation entry has platform `0xEF`, the `0x55AA`
  signature and a zero word checksum;
* the default entry is `0x88` (bootable), media byte 0 (no emulation), and its
  load address points at a FAT16 BPB (jump instruction, `0x55AA`, 512-byte
  sectors, 8 sectors per cluster, `FAT16`, label `CRUGOS`);
* the protective MBR at LBA 0 holds a type-`0xEF` partition entry;
* `xorriso -osirrox` extracts `/boot/efi.img`, its bytes equal the byte range
  the El Torito catalog points at, and `fsck.vfat -n` accepts it;
* `mkfat.py --list` finds `EFI/BOOT/BOOTX64.EFI`, `CRUGOS/KERNEL.ELF`,
  `CRUGOS/VERSION` and `CRUGOS/CMDLINE.TXT` with non-zero sizes, and
  `mkfat.py --read CRUGOS/CMDLINE.TXT` shows `storage=none`.

It prints the geometry it found, so a layout change cannot pass unnoticed:

```
ok: iso hybrid UEFI boot (volume CRUGOS, ESP image at LBA 34, 25165824 bytes,
    4 file(s), catalog sector 33, MBR ESP entry at LBA 1, fsck clean)
```

## Booting the image: `make test-iso`

`make test-iso` is the ISO gate. It `dd`s the ISO onto
`build/x86_64/usb-stick.img` and boots that file twice, headless, with no disk
image and no network:

| Run | How the firmware sees the medium | QEMU arguments |
| --- | --- | --- |
| USB | `usb-storage` on a `qemu-xhci` controller, so OVMF boots it as a USB hard disk | `-device qemu-xhci -device usb-storage,drive=stick` |
| CD | the ISO as an optical drive | `-cdrom build/x86_64/crugos.iso` |

Both runs are checked with `scripts/boot-check.py` in `storage=none` mode
(`--storage=none`): the block/GPT/ext4/journal markers are replaced by the
single `CRUGOS: storage skipped (storage=none)` marker, and every other part of
the boot contract (version, AVX2 policy, input, graphics, self-test, phase
marker, exit status 33) still has to hold. The gate prints
`ok: iso boots from a USB stick (exit 33)` and `ok: iso boots from a CD-ROM (exit 33)`.

```
  FAT   build/x86_64/iso/tree/boot/efi.img (24 MiB, 6133 clusters, 59 used)
  ISO   build/x86_64/crugos.iso (25765888 bytes, FAT16 ESP 24 MiB)
ok: iso hybrid UEFI boot (volume CRUGOS, ... fsck clean)
run-qemu: booting build/x86_64/usb-stick.img ... usb build/x86_64/usb-stick.img, net none
run-qemu: guest exited successfully (isa-debug-exit 0x10 -> status 33)
ok: boot markers found, version 0.30.0 ... storage none QEMU exit status 33
  boot   : pmm=28394 us cpu=390 us vm=635 us traps=315 us timer=65331 us heap=64 us storage=111756 us sched=591 us syscall=132 us total=207612 us
  OK    iso boots from a USB stick (exit 33)
  OK    iso boots from a CD-ROM (exit 33)
```

On the USB leg OVMF boots from the stick and the kernel sees the medium as a
USB device it does not drive:

```
CRUGOS: usb controller BAR assigned at 0xffb00000
CRUGOS: usb controller ready (xhci, 8 port(s))
CRUGOS: usb device 46f4:0001 on port 1 (speed 4)
CRUGOS: usb device class 8 on port 1 (unsupported)
CRUGOS: storage skipped (storage=none)
CRUGOS: selftest ok
CRUGOS: phase 6h boot ok
```

Class 8 is mass storage: the firmware read the FAT image, the kernel's xHCI
driver only implements HID and audio, and that is exactly the intended
division of labour for a boot medium.

## Writing a real USB stick

```
make usb USB_DEV=/dev/sdX USB_CONFIRM=yes
```

`make usb` is the only target that writes outside `build/`, so it is paranoid
on purpose: it refuses without `USB_CONFIRM=yes`, it requires `USB_DEV` to be
an absolute path to an existing *block device*, it refuses a device that is
currently mounted according to `/proc/mounts`, and only then runs
`dd if=build/x86_64/crugos.iso of=$USB_DEV bs=4M conv=fsync status=progress`.
Nothing is unmounted for you, and no partition table is created -- the image
carries its own.

## Limits

* **No GPT.** The plan for this feature asked for a GPT with an ESP entry,
  and the image does not have one: `xorriso` cannot synthesize a GPT without
  the Syslinux `isohybrid` MBR, which is not installed here, and a hand-
  written GPT would collide with the ISO 9660 volume descriptors in the first
  32 KiB of the file. What ships instead is a protective MBR with one
  type-`0xEF` partition (`-part_like_isohybrid`,
  `-iso_mbr_part_type C12A7328-F81F-11D2-BA4B-00A0C93EC93B`), which is what
  both OVMF boots and `check-iso` verify.
* **Unsigned loader.** Secure Boot is out of scope; the stick needs firmware
  with Secure Boot disabled or with the loader's key enrolled.
* **8.3 names only.** See above; the ESP tree is four files.
* **24 MiB by default.** The FAT16 cluster count must stay below 65524, so
  `ISO_SIZE_MB` is capped by `mkfat.py`; 24 MiB uses 59 clusters, and the
  kernel is 224 KiB.
* **No hardware testing.** `make test-iso` boots the artifact in QEMU; writing
  a physical stick and booting real hardware is a manual step (`make usb`).

See [build.md](build.md) for the make targets and options, [boot.md](boot.md)
for the command-line tokens and [storage.md](storage.md) for `storage=none`.
