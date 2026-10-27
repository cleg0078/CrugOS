#!/usr/bin/env bash
# Build a GPT disk image with one ext4 root partition and a test file.
set -euo pipefail

out=${1:?usage: mkdisk.sh OUTPUT [SIZE_MB]}
size_mb=${2:-16}
part_sectors=12288
part_mb=$((part_sectors / 2048))

for tool in truncate sgdisk mke2fs debugfs dd; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "mkdisk: missing $tool" >&2
        exit 1
    fi
done

out_dir=$(dirname "$out")
mkdir -p "$out_dir"
tmp=$(mktemp -d "$out_dir/.mkdisk.XXXXXX")
trap 'rm -rf "$tmp"' EXIT

truncate -s "${size_mb}M" "$out"
sgdisk --clear \
    --new=1:2048:+"$part_sectors" \
    --typecode=1:8300 \
    --change-name=1:crugroot \
    "$out" >/dev/null

truncate -s "${part_mb}M" "$tmp/part.img"
mke2fs -q -t ext4 -F -b 1024 -I 256 \
    -O ^has_journal,^metadata_csum,^64bit,^flex_bg,^dir_index,^resize_inode,^sparse_super,^large_file \
    "$tmp/part.img"
printf 'hello from ext4\n' > "$tmp/hello.txt"
debugfs -w -R "write $tmp/hello.txt /hello.txt" "$tmp/part.img" >/dev/null
dd if="$tmp/part.img" of="$out" bs=512 seek=2048 conv=notrunc status=none

echo "mkdisk: $out (${size_mb} MiB, ext4 root at LBA 2048)"
