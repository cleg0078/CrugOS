#!/usr/bin/env python3
"""Check a CrugOS hybrid UEFI ISO without booting it.

Validates the ISO 9660 primary volume descriptor, the El Torito UEFI boot
catalog, the protective MBR, the embedded FAT16 EFI system partition and the
cmdline that partition ships.

usage: check-iso.py [--iso PATH] [--mkfat PATH]
"""

import argparse
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

SECTOR = 2048
FAT_SECTOR = 512
ESP_PARTITION_TYPE = 0xEF
FAT_JUMP = bytes.fromhex("eb3c90")
BOOT_SIGNATURE = bytes.fromhex("55aa")
EL_TORITO_ID = b"EL TORITO SPECIFICATION"


def le16(buf, off):
    return int.from_bytes(buf[off:off + 2], "little")


def le32(buf, off):
    return int.from_bytes(buf[off:off + 4], "little")


def fail(message):
    print("FAIL: " + message)
    return 1


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--iso", default="build/x86_64/crugos.iso")
    parser.add_argument("--mkfat", default=None)
    args = parser.parse_args()

    iso = Path(args.iso)
    mkfat = Path(args.mkfat) if args.mkfat else Path(__file__).with_name("mkfat.py")
    if not iso.exists():
        return fail("missing iso " + str(iso))
    if not mkfat.exists():
        return fail("missing mkfat helper " + str(mkfat))

    data = iso.read_bytes()
    if len(data) % SECTOR != 0:
        return fail("iso size is not a multiple of 2048")
    if len(data) < 2 * 1024 * 1024:
        return fail("iso is too small")

    pvd = data[16 * SECTOR:17 * SECTOR]
    if pvd[0] != 1 or pvd[1:6] != b"CD001":
        return fail("no ISO 9660 primary volume descriptor at sector 16")
    volume_id = pvd[40:72].decode("ascii", "replace").strip()
    if volume_id != "CRUGOS":
        return fail("unexpected volume id " + repr(volume_id))

    catalog_lba = None
    for index in range(0, 32):
        vd = data[(16 + index) * SECTOR:(17 + index) * SECTOR]
        if vd[1:6] != b"CD001":
            return fail("bad ISO 9660 descriptor at sector " + str(16 + index))
        if vd[0] == 0:
            if vd[7:7 + len(EL_TORITO_ID)] != EL_TORITO_ID:
                return fail("boot record is not an El Torito boot record")
            catalog_lba = le32(vd, 0x47)
            break
        if vd[0] == 255:
            return fail("no El Torito boot record volume descriptor")
    if catalog_lba is None:
        return fail("no El Torito boot record volume descriptor")

    catalog = data[catalog_lba * SECTOR:(catalog_lba + 1) * SECTOR]
    if len(catalog) < 64:
        return fail("truncated El Torito boot catalog")
    if catalog[0] != 1:
        return fail("invalid El Torito validation entry header id")
    if catalog[1] != ESP_PARTITION_TYPE:
        return fail("the El Torito validation entry is not for the EFI platform")
    if catalog[30:32] != BOOT_SIGNATURE:
        return fail("the El Torito validation entry has no 0x55AA signature")
    checksum = 0
    for off in range(0, 32, 2):
        checksum += le16(catalog, off)
    if checksum & 0xFFFF != 0:
        return fail("the El Torito validation entry checksum is invalid")
    entry = catalog[32:64]
    if entry[0] != 0x88:
        return fail("boot catalog default entry is not bootable")
    if entry[1] != 0:
        return fail("boot catalog entry uses an emulation mode, expected no-emulation")
    load_rba = le32(entry, 8)
    if load_rba <= 0 or load_rba * SECTOR >= len(data):
        return fail("boot catalog load address " + str(load_rba) + " is outside the iso")

    fat = data[load_rba * SECTOR:]
    bpb = fat[:FAT_SECTOR]
    if len(bpb) < FAT_SECTOR:
        return fail("truncated FAT boot sector at LBA " + str(load_rba))
    if bpb[0:3] != FAT_JUMP:
        return fail("the El Torito boot image does not start with a FAT jump")
    if bpb[510:512] != BOOT_SIGNATURE:
        return fail("the El Torito boot image has no 0x55AA signature")
    bytes_per_sector = le16(bpb, 11)
    sectors_per_cluster = bpb[13]
    reserved = le16(bpb, 14)
    fat_copies = bpb[16]
    root_entries = le16(bpb, 17)
    media = bpb[21]
    total_sectors = le16(bpb, 19) or le32(bpb, 32)
    if bytes_per_sector != 512:
        return fail("FAT sector size is not 512")
    if sectors_per_cluster != 8:
        return fail("FAT cluster size is not 8 sectors")
    if reserved != 1 or fat_copies != 2:
        return fail("unexpected FAT reserved sector or FAT copy count")
    if root_entries != 512:
        return fail("unexpected FAT root directory entries")
    if media != 0xF8:
        return fail("unexpected FAT media byte")
    if bpb[54:62] != b"FAT16   ":
        return fail("the ESP image is not FAT16")
    fat_bytes = total_sectors * bytes_per_sector
    if fat_bytes <= 0 or len(fat) < fat_bytes:
        return fail("the El Torito boot image is truncated")
    fat = fat[:fat_bytes]
    label = bpb[43:54].decode("ascii", "replace").rstrip()
    if label != "CRUGOS":
        return fail("unexpected FAT volume label " + repr(label))

    mbr = data[0:512]
    if mbr[510:512] != BOOT_SIGNATURE:
        return fail("the iso has no MBR signature")
    esp_partition = None
    for index in range(4):
        entry_part = mbr[446 + index * 16:462 + index * 16]
        if entry_part[4] == ESP_PARTITION_TYPE:
            esp_partition = (le32(entry_part, 8), le32(entry_part, 12))
            break
    if esp_partition is None:
        return fail("the protective MBR has no EFI system partition entry")

    work = Path(tempfile.mkdtemp(prefix="crugos-check-iso."))
    try:
        extracted = work / "efi.img"
        done = subprocess.run(["xorriso", "-osirrox", "on", "-indev", str(iso),
                               "-extract", "/boot/efi.img", str(extracted)],
                              stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        if done.returncode != 0:
            return fail("xorriso could not extract /boot/efi.img: " +
                        done.stderr.decode(errors="replace").strip())
        if extracted.read_bytes() != fat:
            return fail("/boot/efi.img does not match the El Torito boot image")
        if shutil.which("fsck.vfat"):
            check = subprocess.run(["fsck.vfat", "-n", str(extracted)],
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            if check.returncode != 0:
                return fail("fsck.vfat rejected the ESP image: " +
                            check.stdout.decode(errors="replace").strip())
        listing = subprocess.run([sys.executable, str(mkfat), "--list", str(extracted)],
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if listing.returncode != 0:
            return fail("mkfat.py could not list the ESP image: " +
                        listing.stdout.decode(errors="replace").strip())
        entries = {}
        for line in listing.stdout.decode(errors="replace").splitlines():
            parts = line.split(chr(9))
            if len(parts) == 2:
                entries[parts[0]] = parts[1]
        required = ("EFI", "EFI/BOOT", "EFI/BOOT/BOOTX64.EFI",
                    "CRUGOS", "CRUGOS/KERNEL.ELF", "CRUGOS/VERSION",
                    "CRUGOS/CMDLINE.TXT")
        for name in required:
            if name not in entries:
                return fail("the ESP image is missing " + name)
            size = entries[name]
            if size != "DIR" and int(size) <= 0:
                return fail("the ESP image entry " + name + " is empty")
        files = [name for name, size in entries.items() if size != "DIR"]
        cmdline = subprocess.run([sys.executable, str(mkfat), "--read",
                                  "CRUGOS/CMDLINE.TXT", "--image", str(extracted)],
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if cmdline.returncode != 0:
            return fail("mkfat.py could not read the ESP cmdline: " +
                        cmdline.stdout.decode(errors="replace").strip())
        text = cmdline.stdout.decode(errors="replace").strip()
        if "storage=none" not in text:
            return fail("the ISO cmdline does not boot without storage: " + repr(text))
    finally:
        shutil.rmtree(work, ignore_errors=True)

    print("ok: iso hybrid UEFI boot (volume " + volume_id + ", ESP image at LBA " +
          str(load_rba) + ", " + str(fat_bytes) + " bytes, " + str(len(files)) +
          " file(s), catalog sector " + str(catalog_lba) + ", MBR ESP entry at LBA " +
          str(esp_partition[0]) + ", fsck clean)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
