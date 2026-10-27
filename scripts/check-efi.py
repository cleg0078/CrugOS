#!/usr/bin/env python3
"""Structural checks on the UEFI loader PE32+ image."""

import struct
import sys
from pathlib import Path

PE32_MAGIC = 0x20B
MACHINE_AMD64 = 0x8664
EFI_SUBSYSTEM = 10
IMAGE_SCN_MEM_EXECUTE = 0x20000000


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: check-efi.py BOOTX64.EFI", file=sys.stderr)
        return 2
    path = Path(sys.argv[1])
    data = path.read_bytes()
    errors = []

    if len(data) < 0x40 or data[:2] != b"MZ":
        print("FAIL: missing DOS header" if data[:2] != b"MZ" else "FAIL: truncated DOS header")
        return 1
    pe_offset = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe_offset:pe_offset + 4] != b"PE\0\0":
        print("FAIL: missing PE signature")
        return 1

    machine, nsections = struct.unpack_from("<HH", data, pe_offset + 4)
    opt_size = struct.unpack_from("<H", data, pe_offset + 20)[0]
    opt = pe_offset + 24
    magic = struct.unpack_from("<H", data, opt)[0]
    entry = struct.unpack_from("<I", data, opt + 16)[0]
    subsystem = struct.unpack_from("<H", data, opt + 68)[0]
    image_base = struct.unpack_from("<Q", data, opt + 24)[0]
    section_alignment = struct.unpack_from("<I", data, opt + 32)[0]

    if machine != MACHINE_AMD64:
        errors.append(f"machine 0x{machine:x} != 0x{MACHINE_AMD64:x}")
    if magic != PE32_MAGIC:
        errors.append(f"optional magic 0x{magic:x} != 0x{PE32_MAGIC:x} (not PE32+)")
    if subsystem != EFI_SUBSYSTEM:
        errors.append(f"subsystem {subsystem} != {EFI_SUBSYSTEM} (EFI application)")
    if entry == 0:
        errors.append("AddressOfEntryPoint is zero")
    if nsections == 0:
        errors.append("no sections")
    if section_alignment != 0x1000:
        errors.append(f"SectionAlignment 0x{section_alignment:x} != 0x1000")

    section_table = opt + opt_size
    sections = []
    for index in range(nsections):
        base = section_table + index * 40
        if base + 40 > len(data):
            errors.append("truncated section table")
            break
        name = data[base:base + 8].rstrip(b"\0").decode("latin-1")
        vsize, vaddr, raw_size, raw_ptr = struct.unpack_from("<IIII", data, base + 8)
        chars = struct.unpack_from("<I", data, base + 36)[0]
        sections.append((name, vsize, vaddr, raw_size, chars))

    entry_section = None
    for name, vsize, vaddr, raw_size, chars in sections:
        span = max(vsize, raw_size)
        if vaddr <= entry < vaddr + span:
            entry_section = (name, chars)
            break
    if entry == 0:
        pass
    elif entry_section is None:
        errors.append(f"entry RVA 0x{entry:x} outside all sections")
    elif not entry_section[1] & IMAGE_SCN_MEM_EXECUTE:
        errors.append(f"entry lands in non-executable section {entry_section[0]!r}")

    print(f"loader: {path}")
    print(f"  image base   0x{image_base:x}")
    print(f"  entry RVA    0x{entry:x}")
    for name, vsize, vaddr, raw_size, chars in sections:
        print(f"  section {name:<8} vaddr 0x{vaddr:08x} vsize {vsize:#x} flags 0x{chars:08x}")

    if errors:
        for error in errors:
            print("FAIL: " + error)
        return 1
    print("ok: valid PE32+ EFI application")
    return 0


if __name__ == "__main__":
    sys.exit(main())
