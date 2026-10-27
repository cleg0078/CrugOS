#!/usr/bin/env bash
# Verify the toolchain needed to build and boot CrugOS phase 0.
set -uo pipefail

failures=0
warnings=0

require() {
    local name=$1
    if command -v "$name" >/dev/null 2>&1; then
        printf 'ok: %-18s %s\n' "$name" "$(command -v "$name")"
    else
        printf 'FAIL: missing required tool %s\n' "$name"
        failures=$((failures + 1))
    fi
}

optional() {
    local name=$1
    if command -v "$name" >/dev/null 2>&1; then
        printf 'ok: %-18s %s\n' "$name" "$(command -v "$name")"
    else
        printf 'note: optional tool %s not found\n' "$name"
        warnings=$((warnings + 1))
    fi
}

require crugc
require ld
require python3
require qemu-system-x86_64
require objcopy
optional size
optional sha256sum

if command -v crugc >/dev/null 2>&1; then
    printf '  crugc: %s\n' "$(crugc --version 2>/dev/null | head -n 1)"
fi
if command -v ld >/dev/null 2>&1; then
    printf '  ld:    %s\n' "$(ld --version 2>/dev/null | head -n 1)"
fi
if command -v qemu-system-x86_64 >/dev/null 2>&1; then
    printf '  qemu:  %s\n' "$(qemu-system-x86_64 --version | head -n 1)"
fi

OVMF_CODE=${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE_4M.fd}
OVMF_VARS=${OVMF_VARS:-/usr/share/OVMF/OVMF_VARS_4M.fd}
for firmware in "$OVMF_CODE" "$OVMF_VARS"; do
    if [[ -r "$firmware" ]]; then
        printf 'ok: %-18s %s\n' ovmf "$firmware"
    else
        printf 'FAIL: missing OVMF firmware %s\n' "$firmware"
        failures=$((failures + 1))
    fi
done

if [[ -e /dev/kvm ]]; then
    echo "ok: /dev/kvm present (hardware acceleration available)"
else
    echo "note: /dev/kvm absent, QEMU will run with TCG emulation"
fi

py=$(python3 -c 'import sys; print(f"{sys.version_info.major}.{sys.version_info.minor}")' 2>/dev/null || echo '?')
printf 'ok: %-18s %s\n' python "$py"

if [[ $failures -gt 0 ]]; then
    printf '\npreflight: %d required item(s) missing\n' "$failures" >&2
    exit 1
fi
printf '\npreflight: toolchain ready (%d note(s))\n' "$warnings"
