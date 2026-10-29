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

# Optional ring-3 programs (make test-user through test-user6): the kernel
# loads /bin/hello, /bin/badptr, /bin/fault, /bin/iso, the shell /bin/sh, the
# spawn probe /bin/spawn and its child /bin/child, the fork probe /bin/fork and
# the large-fork probe /bin/forkbig from the root filesystem,
# plus the script /etc/sh.rc the shell runs.
# USER_DIR comes from the environment so the raid/nvme/ahci/crypt call sites
# keep their positional arguments unchanged.
if [ -n "${USER_DIR:-}" ]; then
    debugfs -w -R "mkdir /bin" "$fs_image" >/dev/null 2>&1 || true
    for prog in hello badptr fault iso sh spawn child fork forkbig; do
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

# Optional .app bundles (make test-app-bundles): the kernel scans /Applications
# at boot and registers every <Name>.app whose manifest.toml parses. Hello.app
# and Orbit.app are valid; Broken.app has no version, so the rejection path is
# exercised too. APP_DIR comes from the environment, like USER_DIR.
if [ -n "${APP_DIR:-}" ]; then
    apps_tmp=$tmp/apps
    mkdir -p "$apps_tmp/Hello.app/bin/x86_64" \
             "$apps_tmp/Orbit.app/bin/x86_64" \
             "$apps_tmp/Broken.app"
    for prog in app_hello app_orbit; do
        if [ ! -f "$APP_DIR/$prog" ]; then
            echo "mkdisk: missing app payload $APP_DIR/$prog" >&2
            exit 1
        fi
    done
    cp "$APP_DIR/app_hello" "$apps_tmp/Hello.app/bin/x86_64/app_hello"
    cp "$APP_DIR/app_orbit" "$apps_tmp/Orbit.app/bin/x86_64/app_orbit"
    printf '%s\n' \
        '[app]' 'id = "hello"' 'name = "Hello"' 'version = "1.0.0"' \
        '' '[exec]' 'prog = "bin/x86_64/app_hello"' \
        '' '[tooltypes]' 'GREET = "world"' \
        '' '[icon]' 'rgb = "FFB000"' > "$apps_tmp/Hello.app/manifest.toml"
    printf '%s\n' \
        '[app]' 'id = "orbit"' 'name = "Orbit"' 'version = "1.0.0"' \
        '' '[exec]' 'prog = "bin/x86_64/app_orbit"' \
        '' '[tooltypes]' 'MODE = "demo"' \
        '' '[icon]' 'rgb = "30C0FF"' > "$apps_tmp/Orbit.app/manifest.toml"
    printf '%s\n' \
        '[app]' 'id = "broken"' 'name = "Broken"' \
        '' '[exec]' 'prog = "bin/x86_64/app_hello"' > "$apps_tmp/Broken.app/manifest.toml"
    debugfs -w -R "mkdir /Applications" "$fs_image" >/dev/null 2>&1 || true
    for app in Hello Orbit; do
        debugfs -w -R "mkdir /Applications/$app.app" "$fs_image" >/dev/null 2>&1 || true
        debugfs -w -R "mkdir /Applications/$app.app/bin" "$fs_image" >/dev/null 2>&1 || true
        debugfs -w -R "mkdir /Applications/$app.app/bin/x86_64" "$fs_image" >/dev/null 2>&1 || true
        debugfs -w -R "write $apps_tmp/$app.app/manifest.toml /Applications/$app.app/manifest.toml" "$fs_image" >/dev/null
    done
    debugfs -w -R "write $apps_tmp/Hello.app/bin/x86_64/app_hello /Applications/Hello.app/bin/x86_64/app_hello" "$fs_image" >/dev/null
    debugfs -w -R "write $apps_tmp/Orbit.app/bin/x86_64/app_orbit /Applications/Orbit.app/bin/x86_64/app_orbit" "$fs_image" >/dev/null
    debugfs -w -R "mkdir /Applications/Broken.app" "$fs_image" >/dev/null 2>&1 || true
    debugfs -w -R "write $apps_tmp/Broken.app/manifest.toml /Applications/Broken.app/manifest.toml" "$fs_image" >/dev/null
    app_script=$tmp/app.rc
    printf '%s\n' 'app list' 'app info orbit' 'app run orbit' 'exit 9' > "$app_script"
    debugfs -w -R "mkdir /etc" "$fs_image" >/dev/null 2>&1 || true
    debugfs -w -R "write $app_script /etc/app.rc" "$fs_image" >/dev/null
fi

# Optional signed packages (make test-pkg / make test-caps): PKG_DIR holds the
# two .crpkg files the host signer produced, the trust list and the capability
# policy.  The installer reads /System/packages, checks the signing key against
# /System/trusted/keys.conf and records installs in /System/pkgdb.
if [ -n "${PKG_DIR:-}" ]; then
    for tree in System System/packages System/trusted System/pkgdb; do
        debugfs -w -R "mkdir /$tree" "$fs_image" >/dev/null 2>&1 || true
    done
    for pkg in "$PKG_DIR"/*.crpkg; do
        if [ ! -f "$pkg" ]; then
            echo "mkdisk: no packages in $PKG_DIR" >&2
            exit 1
        fi
        debugfs -w -R "write $pkg /System/packages/$(basename "$pkg")" "$fs_image" >/dev/null
    done
    for conf in keys.conf perms.conf; do
        if [ ! -f "$PKG_DIR/$conf" ]; then
            echo "mkdisk: missing $PKG_DIR/$conf" >&2
            exit 1
        fi
    done
    debugfs -w -R "write $PKG_DIR/keys.conf /System/trusted/keys.conf" "$fs_image" >/dev/null
    debugfs -w -R "write $PKG_DIR/perms.conf /System/perms.conf" "$fs_image" >/dev/null
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
