#!/usr/bin/env bash
# Build a GPT disk image with one ext4 root partition and a test file.
# The journal gets JBD2 checksum v3 (mke2fs 1.47 does not enable it).
# mode "crash" also plants /simulate-crash, which makes the kernel commit a
# transaction and exit without checkpointing (see scripts/check-journal.py).
set -euo pipefail

out=${1:?usage: mkdisk.sh OUTPUT [SIZE_MB] [normal|crash]}
size_mb=${2:-16}
mode=${3:-normal}
part_sectors=24576
part_mb=$((part_sectors / 2048))
script_dir=$(dirname "$0")

case "$mode" in
    normal|crash) ;;
    *) echo "mkdisk: unknown mode '$mode' (normal|crash)" >&2; exit 1 ;;
esac

for tool in truncate sgdisk mke2fs debugfs dd python3; do
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
mke2fs -q -t ext4 -F -b 1024 -I 256 -J size=1 \
    -O has_journal,metadata_csum,^64bit,^flex_bg,^dir_index,^resize_inode,^sparse_super,^large_file,^orphan_file \
    "$tmp/part.img"
"$script_dir/journal-csum.py" "$tmp/part.img"
printf 'hello from ext4\n' > "$tmp/hello.txt"
debugfs -w -R "write $tmp/hello.txt /hello.txt" "$tmp/part.img" >/dev/null
if [ "$mode" = crash ]; then
    printf 'dirty journal gate\n' > "$tmp/crash.txt"
    debugfs -w -R "write $tmp/crash.txt /simulate-crash" "$tmp/part.img" >/dev/null
fi
dd if="$tmp/part.img" of="$out" bs=512 seek=2048 conv=notrunc status=none

echo "mkdisk: $out (${size_mb} MiB, ext4 root at LBA 2048, journal checksums, mode $mode)"