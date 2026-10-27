#!/usr/bin/env bash
# Zero a RAID member partition (data and metadata) while keeping its GPT.
# Simulates a blank replacement disk for the cold-spare rebuild gate.
set -euo pipefail

disk=${1:?usage: raid-blank.sh DISK}
part_start=2048
part_sectors=24576

if [ ! -f "$disk" ]; then
    echo "raid-blank: missing disk $disk" >&2
    exit 1
fi

dd if=/dev/zero of="$disk" bs=512 seek="$part_start" count="$part_sectors" \
    conv=notrunc status=none
echo "raid-blank: $disk (partition zeroed, GPT kept)"
