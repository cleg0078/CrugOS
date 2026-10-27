#!/usr/bin/env bash
# Boot the ESP tree under QEMU + OVMF and record serial output, debug output and
# the QEMU exit status. Never fails the caller: verdicts live in the log files.
set -euo pipefail

: "${ESP_DIR:?ESP_DIR is required}"
: "${SERIAL_LOG:?SERIAL_LOG is required}"
: "${DEBUG_LOG:?DEBUG_LOG is required}"
: "${EXIT_CODE_FILE:?EXIT_CODE_FILE is required}"
DISK_IMG=${DISK_IMG:-}
EXTRA_DISKS=${EXTRA_DISKS:-}
NVME_DISK=${NVME_DISK:-}
NVME_SERIAL=${NVME_SERIAL:-CRUGNVME0}

QEMU=${QEMU:-qemu-system-x86_64}
OVMF_CODE=${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE_4M.fd}
OVMF_VARS=${OVMF_VARS:-/usr/share/OVMF/OVMF_VARS_4M.fd}
MEMORY_MB=${MEMORY_MB:-512}
SMP=${SMP:-1}
TIMEOUT_SEC=${TIMEOUT_SEC:-30}
QEMU_CPU=${QEMU_CPU:-}
QEMU_ACCEL=${QEMU_ACCEL:-}
QEMU_DISPLAY=${QEMU_DISPLAY:-none}
QEMU_EXTRA=${QEMU_EXTRA:-}

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

CPU_ARGS=()
if [[ -n "$QEMU_CPU" ]]; then
    CPU_ARGS=(-cpu "$QEMU_CPU")
fi

ACCEL_ARGS=()
if [[ -n "$QEMU_ACCEL" ]]; then
    ACCEL_ARGS=(-accel "$QEMU_ACCEL")
fi

EXTRA_ARGS=()
if [[ -n "$QEMU_EXTRA" ]]; then
    read -r -a EXTRA_ARGS <<< "$QEMU_EXTRA"
fi

DISK_ARGS=()
if [[ -n "$DISK_IMG" ]]; then
    if [[ ! -r "$DISK_IMG" ]]; then
        echo "run-qemu: missing disk image $DISK_IMG" >&2
        exit 1
    fi
    DISK_ARGS=(-drive "file=$DISK_IMG,if=none,id=disk0,format=raw" \
               -device "virtio-blk-pci,drive=disk0,disable-modern=on")
fi

NVME_ARGS=()
if [[ -n "$NVME_DISK" ]]; then
    if [[ ! -r "$NVME_DISK" ]]; then
        echo "run-qemu: missing NVMe image $NVME_DISK" >&2
        exit 1
    fi
    NVME_ARGS=(-drive "file=$NVME_DISK,if=none,id=nvme0,format=raw" \
               -device "nvme,drive=nvme0,serial=$NVME_SERIAL,logical_block_size=512")
fi

index=1
for extra in $EXTRA_DISKS; do
    if [[ ! -r "$extra" ]]; then
        echo "run-qemu: missing extra disk image $extra" >&2
        exit 1
    fi
    DISK_ARGS+=(-drive "file=$extra,if=none,id=disk$index,format=raw" \
                -device "virtio-blk-pci,drive=disk$index,disable-modern=on")
    index=$((index + 1))
done

echo "run-qemu: booting $ESP_DIR (${MEMORY_MB} MiB, ${SMP} vCPU, cpu ${QEMU_CPU:-default}, accel ${QEMU_ACCEL:-default}, display ${QEMU_DISPLAY}, disk ${DISK_IMG:-none}, extra ${EXTRA_DISKS:-none}, nvme ${NVME_DISK:-none}, timeout ${TIMEOUT_SEC}s)"
set +e
TMPDIR="$TMPDIR" timeout --foreground "$TIMEOUT_SEC" "$QEMU" \
    -machine q35 \
    -m "$MEMORY_MB" \
    -smp "$SMP" \
    "${ACCEL_ARGS[@]}" \
    "${CPU_ARGS[@]}" \
    -drive if=pflash,format=raw,unit=0,readonly=on,file="$OVMF_CODE" \
    -drive if=pflash,format=raw,unit=1,file="$VARS_COPY" \
    -drive file=fat:rw:"$ESP_DIR",format=raw,media=disk \
    "${DISK_ARGS[@]}" \
    "${NVME_ARGS[@]}" \
    -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
    -display "$QEMU_DISPLAY" \
    -serial "file:$SERIAL_LOG" \
    -debugcon "file:$DEBUG_LOG" \
    "${EXTRA_ARGS[@]}" \
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
