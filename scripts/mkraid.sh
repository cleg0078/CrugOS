#!/usr/bin/env bash
# Build two GPT member disks for a CrugOS RAID1 test array.
set -euo pipefail

out_a=${1:?usage: mkraid.sh DISK_A DISK_B}
out_b=${2:?usage: mkraid.sh DISK_A DISK_B}
size_mb=16
part_sectors=24576
data_sectors=$((part_sectors - 2))

for tool in truncate sgdisk mke2fs debugfs dd python3; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "mkraid: missing $tool" >&2
        exit 1
    fi
done

out_dir=$(dirname "$out_a")
mkdir -p "$out_dir" "$(dirname "$out_b")"
tmp=$(mktemp -d "$out_dir/.mkraid.XXXXXX")
trap 'rm -rf "$tmp"' EXIT

for disk in "$out_a" "$out_b"; do
    truncate -s "${size_mb}M" "$disk"
    sgdisk --clear \
        --new=1:2048:+"$part_sectors" \
        --typecode=1:fd00 \
        --change-name=1:crugraid \
        "$disk" >/dev/null
done

truncate -s $((data_sectors * 512)) "$tmp/data.img"
mke2fs -q -t ext4 -F -b 1024 -I 256 -J size=1 \
    -O has_journal,^metadata_csum,^64bit,^flex_bg,^dir_index,^resize_inode,^sparse_super,^large_file,^orphan_file \
    "$tmp/data.img"
printf 'hello from ext4\n' > "$tmp/hello.txt"
debugfs -w -R "write $tmp/hello.txt /hello.txt" "$tmp/data.img" >/dev/null

for disk in "$out_a" "$out_b"; do
    dd if="$tmp/data.img" of="$disk" bs=512 seek=2048 conv=notrunc status=none
done

array_uuid=$(python3 -c 'import os; print(os.urandom(16).hex())')
python3 "$(dirname "$0")/raid-meta.py" "$out_a" 2048 "$part_sectors" 0 2 "$array_uuid" 1
python3 "$(dirname "$0")/raid-meta.py" "$out_b" 2048 "$part_sectors" 1 2 "$array_uuid" 1

echo "mkraid: $out_a and $out_b (16 MiB, RAID1 members, array $array_uuid)"
