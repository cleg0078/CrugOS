#!/usr/bin/env bash
# Boot the ESP tree under QEMU + OVMF and record serial output, debug output and
# the QEMU exit status. Never fails the caller: verdicts live in the log files.
set -euo pipefail

: "${ESP_DIR:?ESP_DIR is required}"
: "${SERIAL_LOG:?SERIAL_LOG is required}"
: "${DEBUG_LOG:?DEBUG_LOG is required}"
: "${EXIT_CODE_FILE:?EXIT_CODE_FILE is required}"

QEMU=${QEMU:-qemu-system-x86_64}
OVMF_CODE=${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE_4M.fd}
OVMF_VARS=${OVMF_VARS:-/usr/share/OVMF/OVMF_VARS_4M.fd}
MEMORY_MB=${MEMORY_MB:-512}
SMP=${SMP:-1}
TIMEOUT_SEC=${TIMEOUT_SEC:-30}

for tool in "$QEMU"; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "run-qemu: missing $tool" >&2
        exit 1
    fi
done
for firmware in "$OVMF_CODE" "$OVMF_VARS"; do
    if [[ ! -r "$firmware" ]]; then
        echo "run-qemu: missing firmware $firmware" >&2
        exit 1
    fi
done
if [[ ! -d "$ESP_DIR/EFI/BOOT" ]]; then
    echo "run-qemu: no ESP at $ESP_DIR" >&2
    exit 1
fi

mkdir -p "$(dirname "$SERIAL_LOG")" "$(dirname "$DEBUG_LOG")" \
         "$(dirname "$EXIT_CODE_FILE")" "$TMPDIR"
rm -f "$SERIAL_LOG" "$DEBUG_LOG" "$EXIT_CODE_FILE"

VARS_COPY=$(dirname "$SERIAL_LOG")/OVMF_VARS.fd
cp "$OVMF_VARS" "$VARS_COPY"

echo "run-qemu: booting $ESP_DIR (${MEMORY_MB} MiB, ${SMP} vCPU, timeout ${TIMEOUT_SEC}s)"
set +e
TMPDIR="$TMPDIR" timeout --foreground "$TIMEOUT_SEC" "$QEMU" \
    -machine q35 \
    -m "$MEMORY_MB" \
    -smp "$SMP" \
    -drive if=pflash,format=raw,unit=0,readonly=on,file="$OVMF_CODE" \
    -drive if=pflash,format=raw,unit=1,file="$VARS_COPY" \
    -drive file=fat:rw:"$ESP_DIR",format=raw,media=disk \
    -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
    -display none \
    -serial "file:$SERIAL_LOG" \
    -debugcon "file:$DEBUG_LOG" \
    -no-reboot
status=$?
set -e

echo "$status" > "$EXIT_CODE_FILE"
if [[ $status -eq 33 ]]; then
    echo "run-qemu: guest exited successfully (isa-debug-exit 0x10 -> status 33)"
elif [[ $status -eq 35 ]]; then
    echo "run-qemu: guest panicked (isa-debug-exit 0x11 -> status 35)"
elif [[ $status -eq 124 ]]; then
    echo "run-qemu: timed out after ${TIMEOUT_SEC}s"
else
    echo "run-qemu: QEMU exited with status $status"
fi
echo "run-qemu: serial log $SERIAL_LOG"
exit 0
