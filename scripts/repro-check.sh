#!/usr/bin/env bash
# Build twice into separate trees and verify identical kernel and loader bytes.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

MAKE=${MAKE:-make}
ARCH=${ARCH:-x86_64}

rm -rf build/repro-a build/repro-b
"$MAKE" --no-print-directory BUILD_ROOT=build/repro-a kernel loader
"$MAKE" --no-print-directory BUILD_ROOT=build/repro-b kernel loader

A=build/repro-a/$ARCH
B=build/repro-b/$ARCH
status=0
for artifact in kernel.elf BOOTX64.EFI; do
    if cmp -s "$A/$artifact" "$B/$artifact"; then
        echo "ok: $artifact is reproducible"
    else
        echo "FAIL: $artifact differs between builds" >&2
        sha256sum "$A/$artifact" "$B/$artifact" >&2
        status=1
    fi
done
exit "$status"
