#!/usr/bin/env bash
# Report the kernel size budget: <1 MiB stretch, <2 MiB hard limit.
set -euo pipefail

KERNEL=${1:?usage: size-report.sh KERNEL_ELF}
HARD_LIMIT=2097152
STRETCH_LIMIT=1048576

if [[ ! -f "$KERNEL" ]]; then
    echo "size-report: missing $KERNEL" >&2
    exit 1
fi

mapfile -t sizes < <(size -A "$KERNEL" | awk '
    $1 ~ /^\.text/    { text += $2 }
    $1 ~ /^\.rodata/  { rodata += $2 }
    $1 ~ /^\.data/    { data += $2 }
    $1 ~ /^\.bss/     { bss += $2 }
    END { printf "%d %d %d %d\n", text, rodata, data, bss }')
read -r text rodata data bss <<< "${sizes[0]}"
total=$((text + rodata + data + bss))
file_size=$(stat -c%s "$KERNEL")

printf 'kernel: %s\n' "$KERNEL"
printf '  .text    %8d B  (%d KiB)\n' "$text" "$((text / 1024))"
printf '  .rodata  %8d B  (%d KiB)\n' "$rodata" "$((rodata / 1024))"
printf '  .data    %8d B  (%d KiB)\n' "$data" "$((data / 1024))"
printf '  .bss     %8d B  (%d KiB)\n' "$bss" "$((bss / 1024))"
printf '  load     %8d B  (%d KiB)\n' "$total" "$((total / 1024))"
printf '  file     %8d B  (%d KiB)\n' "$file_size" "$((file_size / 1024))"
printf '  budgets  stretch %d KiB / hard %d KiB\n' "$((STRETCH_LIMIT / 1024))" "$((HARD_LIMIT / 1024))"

if (( file_size > HARD_LIMIT || total > HARD_LIMIT )); then
    echo "FAIL: kernel exceeds the 2 MiB hard limit" >&2
    exit 1
fi
if (( file_size > STRETCH_LIMIT || total > STRETCH_LIMIT )); then
    echo "warning: kernel exceeds the 1 MiB stretch goal"
else
    echo "ok: kernel under the 1 MiB stretch goal"
fi
