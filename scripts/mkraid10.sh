#!/usr/bin/env bash
# Build four GPT member disks for a CrugOS RAID10 test array.
set -euo pipefail

out_a=${1:?usage: mkraid10.sh DISK_A DISK_B DISK_C DISK_D}
out_b=${2:?usage: mkraid10.sh DISK_A DISK_B DISK_C DISK_D}
out_c=${3:?usage: mkraid10.sh DISK_A DISK_B DISK_C DISK_D}
out_d=${4:?usage: mkraid10.sh DISK_A DISK_B DISK_C DISK_D}
size_mb=16
part_sectors=24576
data_sectors=$((part_sectors - 2))
chunk_sectors=128

for tool in truncate sgdisk mke2fs debugfs dd python3; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "mkraid10: missing $tool" >&2
        exit 1
    fi
done

disks=("$out_a" "$out_b" "$out_c" "$out_d")
out_dir=$(dirname "$out_a")
mkdir -p "$out_dir" "$(dirname "$out_b")" "$(dirname "$out_c")" "$(dirname "$out_d")"
tmp=$(mktemp -d "$out_dir/.mkraid10.XXXXXX")
trap 'rm -rf "$tmp"' EXIT

for disk in "${disks[@]}"; do
    truncate -s "${size_mb}M" "$disk"
    sgdisk --clear \
        --new=1:2048:+"$part_sectors" \
        --typecode=1:fd00 \
        --change-name=1:crugraid \
        "$disk" >/dev/null
done

truncate -s $((data_sectors * 512)) "$tmp/data.img"
mke2fs -q -t ext4 -F -b 1024 -I 256 -J size=1 \
    -O has_journal,metadata_csum,^64bit,^flex_bg,^dir_index,^resize_inode,^sparse_super,^large_file,^orphan_file \
    "$tmp/data.img"
printf 'hello from ext4\n' > "$tmp/hello.txt"
debugfs -w -R "write $tmp/hello.txt /hello.txt" "$tmp/data.img" >/dev/null

for disk in "${disks[@]}"; do
    dd if="$tmp/data.img" of="$disk" bs=512 seek=2048 conv=notrunc status=none
done

array_uuid=$(python3 -c 'import os; print(os.urandom(16).hex())')
for slot in 0 1 2 3; do
    python3 "$(dirname "$0")/raid-meta.py" "${disks[$slot]}" 2048 "$part_sectors" \
        "$slot" 4 "$array_uuid" 1 10 "$chunk_sectors"
done

echo "mkraid10: ${disks[*]} (16 MiB, RAID10, chunk ${chunk_sectors} sectors, array $array_uuid)"
