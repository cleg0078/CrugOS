#!/usr/bin/env bash
# Build a GPT disk image with one ext4 root partition and a test file.
# The journal gets JBD2 checksum v3 (mke2fs 1.47 does not enable it).
# mode "crash" also plants /simulate-crash, which makes the kernel commit a
# transaction and exit without checkpointing (see scripts/check-journal.py).
# mode "crypt" creates an AES-256-XTS crypt volume (crypt.device): the ext4
# filesystem lives in the data area after the 16 KiB header, so it is built as
# a standalone image and sealed by tools/mkcrypt.
#
# usage: mkdisk.sh OUTPUT [SIZE_MB] [normal|crash|crypt] [MCRYPT [PASS [RECOVERY [M_KIB]]]]
set -euo pipefail

out=${1:?usage: mkdisk.sh OUTPUT [SIZE_MB] [normal|crash|crypt] [MCRYPT [PASS [RECOVERY [M_KIB]]]]}
size_mb=${2:-16}
mode=${3:-normal}
part_sectors=24576
part_mb=$((part_sectors / 2048))
script_dir=$(dirname "$0")
crypt_tool=${4:-}
crypt_pass=${5:-crugos-test}
crypt_recovery=${6:-}
crypt_m=${7:-4096}
crypt_header_bytes=16384

case "$mode" in
    normal|crash|crypt) ;;
    *) echo "mkdisk: unknown mode '$mode' (normal|crash|crypt)" >&2; exit 1 ;;
esac

for tool in truncate sgdisk mke2fs debugfs dd python3; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "mkdisk: missing $tool" >&2
        exit 1
    fi
done
if [ "$mode" = crypt ]; then
    if [ -z "$crypt_tool" ] || [ ! -x "$crypt_tool" ]; then
        echo "mkdisk: crypt mode needs an executable mkcrypt (got '$crypt_tool')" >&2
        exit 1
    fi
    if [ -z "$crypt_pass" ]; then
        echo "mkdisk: crypt mode needs a password" >&2
        exit 1
    fi
fi

out_dir=$(dirname "$out")
mkdir -p "$out_dir"
tmp=$(mktemp -d "$out_dir/.mkdisk.XXXXXX")
trap 'rm -rf "$tmp"' EXIT

truncate -s "${size_mb}M" "$out"
if [ "$mode" = crypt ]; then
    sgdisk --clear \
        --new=1:2048:+"$part_sectors" \
        --typecode=1:8f1c7d2e-3b45-4c6a-9e01-c2a1b7f0d9e3 \
        --change-name=1:crugcrypt \
        "$out" >/dev/null
else
    sgdisk --clear \
        --new=1:2048:+"$part_sectors" \
        --typecode=1:8300 \
        --change-name=1:crugroot \
        "$out" >/dev/null
fi

part_bytes=$((part_mb * 1024 * 1024))
fs_bytes=$part_bytes
fs_image=$tmp/part.img
if [ "$mode" = crypt ]; then
    fs_bytes=$((part_bytes - crypt_header_bytes))
    fs_image=$tmp/fs.img
fi

truncate -s "$part_bytes" "$tmp/part.img"
truncate -s "$fs_bytes" "$fs_image"
mke2fs -q -t ext4 -F -b 1024 -I 256 -J size=1 \
    -O has_journal,metadata_csum,^64bit,^flex_bg,^dir_index,^resize_inode,^sparse_super,^large_file,^orphan_file \
    "$fs_image"
"$script_dir/journal-csum.py" "$fs_image"
printf 'hello from ext4\n' > "$tmp/hello.txt"
debugfs -w -R "write $tmp/hello.txt /hello.txt" "$fs_image" >/dev/null
if [ "$mode" = crash ]; then
    printf 'dirty journal gate\n' > "$tmp/crash.txt"
    debugfs -w -R "write $tmp/crash.txt /simulate-crash" "$fs_image" >/dev/null
fi

# Optional ring-3 programs (make test-user through test-user5): the kernel
# loads /bin/hello, /bin/badptr, /bin/fault, /bin/iso, the shell /bin/sh, the
# spawn probe /bin/spawn and its child /bin/child, and the fork probe /bin/fork
# from the root filesystem,
# plus the script /etc/sh.rc the shell runs.
# USER_DIR comes from the environment so the raid/nvme/ahci/crypt call sites
# keep their positional arguments unchanged.
if [ -n "${USER_DIR:-}" ]; then
    debugfs -w -R "mkdir /bin" "$fs_image" >/dev/null 2>&1 || true
    for prog in hello badptr fault iso sh spawn child fork; do
        if [ ! -f "$USER_DIR/$prog" ]; then
            echo "mkdisk: missing user program $USER_DIR/$prog" >&2
            exit 1
        fi
        debugfs -w -R "write $USER_DIR/$prog /bin/$prog" "$fs_image" >/dev/null
    done
    debugfs -w -R "mkdir /etc" "$fs_image" >/dev/null 2>&1 || true
    shell_script=$tmp/sh.rc
    printf '%s\n' \
        'ver' \
        'echo hello from the user shell' \
        'cat /hello.txt' \
        'ls /' \
        'write /shout.txt hello-from-user-shell' \
        'cat /shout.txt' \
        'exit 7' > "$shell_script"
    debugfs -w -R "write $shell_script /etc/sh.rc" "$fs_image" >/dev/null
fi

if [ "$mode" = crypt ]; then
    mkcrypt_args=(format "$tmp/part.img" --plain "$fs_image" --password "$crypt_pass" --m "$crypt_m")
    if [ -n "$crypt_recovery" ]; then
        mkcrypt_args+=(--recovery "$crypt_recovery")
    fi
    "$crypt_tool" "${mkcrypt_args[@]}"
    data_bytes=$fs_bytes
fi

dd if="$tmp/part.img" of="$out" bs=512 seek=2048 conv=notrunc status=none

if [ "$mode" = crypt ]; then
    echo "mkdisk: $out (${size_mb} MiB, AES-256-XTS crypt root at LBA 2048, data ${data_bytes} bytes, mode $mode)"
else
    echo "mkdisk: $out (${size_mb} MiB, ext4 root at LBA 2048, journal checksums, mode $mode)"
fi
