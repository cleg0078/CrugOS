#!/usr/bin/env bash
# mkiso.sh - assemble a bootable UEFI ISO image from an ESP tree.
#
# The image is a hybrid: an El Torito no-emulation CD whose boot image is a
# FAT16 ESP, plus an MBR whose single active EFI System Partition entry covers
# the same bytes, so the file boots as a CD-ROM and after a plain dd onto a USB
# stick.  xorriso -part_like_isohybrid points that MBR entry at the ISO9660
# image instead (byte 512 is zeros, not a FAT boot sector), so the table is
# rewritten from the El Torito catalog below.  The ESP tree is also grafted
# into the ISO9660 file system so the stick stays readable on systems that look
# for /EFI/BOOT.
set -euo pipefail

: "${ESP_DIR:?ESP_DIR is required}"
: "${ISO_IMG:?ISO_IMG is required}"
: "${ISO_STAGE:?ISO_STAGE is required}"
ISO_SIZE_MB="${ISO_SIZE_MB:-24}"
ISO_CMDLINE="${ISO_CMDLINE:-storage=none}"
ISO_VOLID="${ISO_VOLID:-CRUGOS}"
ISO_DATE="${ISO_DATE:-2026010100000000}"
PYTHON="${PYTHON:-python3}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

for tool in xorriso fsck.vfat; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "mkiso: $tool is required" >&2
        exit 1
    fi
done

if [ ! -f "$ESP_DIR/EFI/BOOT/BOOTX64.EFI" ]; then
    echo "mkiso: no ESP tree at $ESP_DIR (run make esp)" >&2
    exit 1
fi

rm -rf "$ISO_STAGE"
mkdir -p "$ISO_STAGE/tree/boot"
cp -a "$ESP_DIR" "$ISO_STAGE/esp"
mkdir -p "$ISO_STAGE/esp/crugos"
# The ISO ships its own command line: never inherit a QEMU disk token.
printf '%s\n' "$ISO_CMDLINE" > "$ISO_STAGE/esp/crugos/cmdline.txt"

FAT_IMG="$ISO_STAGE/tree/boot/efi.img"
"$PYTHON" "$SCRIPT_DIR/mkfat.py" --source "$ISO_STAGE/esp" \
    --output "$FAT_IMG" --size-mb "$ISO_SIZE_MB" --label "$ISO_VOLID"

if ! fsck.vfat -n "$FAT_IMG" >/dev/null 2>&1; then
    echo "mkiso: the FAT ESP image is not a clean FAT16 file system" >&2
    exit 1
fi

# A second copy of the same files, grafted at the ISO root.
cp -a "$ISO_STAGE/esp/." "$ISO_STAGE/tree/"

DATE_ARGS=()
if xorriso -as mkisofs -help 2>&1 | grep -q -- "--set_all_file_dates"; then
    DATE_ARGS=(--set_all_file_dates "$ISO_DATE")
fi

rm -f "$ISO_IMG"
if ! xorriso -as mkisofs -iso-level 3 \
    -V "$ISO_VOLID" \
    -e boot/efi.img -no-emul-boot -eltorito-platform efi \
    -part_like_isohybrid \
    -iso_mbr_part_type C12A7328-F81F-11D2-BA4B-00A0C93EC93B \
    --protective-msdos-label \
    "${DATE_ARGS[@]}" \
    -o "$ISO_IMG" "$ISO_STAGE/tree" >"$ISO_STAGE/xorriso.log" 2>&1; then
    echo "mkiso: xorriso failed:" >&2
    cat "$ISO_STAGE/xorriso.log" >&2
    exit 1
fi

# Rewrite the MBR so its ESP entry covers the El Torito FAT16 image and not the
# ISO9660 image.  A strictly UEFI firmware that boots a dd-ed stick enumerates
# MBR partitions and mounts the ESP; it never reads the CD boot catalog, so the
# entry xorriso writes leaves the stick reported as not bootable.
"$PYTHON" "$SCRIPT_DIR/check-iso.py" --iso "$ISO_IMG" --patch-mbr

SIZE="$(stat -c %s "$ISO_IMG")"
printf '  ISO   %s (%s bytes, FAT16 ESP %s MiB)\n' "$ISO_IMG" "$SIZE" "$ISO_SIZE_MB"
