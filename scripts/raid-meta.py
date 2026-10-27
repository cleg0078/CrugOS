#!/usr/bin/env python3
"""Write CrugOS RAID1 member metadata into a GPT partition."""

import os
import struct
import sys
import zlib
from pathlib import Path

SECTOR = 512
MAGIC = 0x43524131
VERSION = 1
LEVEL_1 = 1

# u32 magic, u16 version, u16 level, u32 member_count, u32 slot,
# u32 chunk_sectors, u32 state, u64 array_sectors, u64 member_sectors,
# u64 generation, 16s array_uuid, 16s member_uuid, 428s reserved, u32 checksum
FORMAT = "<IHHIIIIQQQ16s16s428sI"


def build(slot, members, array_uuid, generation, member_sectors):
    array_sectors = member_sectors - 2
    member_uuid = os.urandom(16)
    body = struct.pack(FORMAT, MAGIC, VERSION, LEVEL_1, members, slot, 0, 0,
                       array_sectors, member_sectors, generation,
                       array_uuid, member_uuid, b"\x00" * 428, 0)
    checksum = zlib.crc32(body) & 0xFFFFFFFF
    return body[:-4] + struct.pack("<I", checksum)


def main():
    if len(sys.argv) != 8:
        print("usage: raid-meta.py DISK FIRST_LBA SECTORS SLOT MEMBERS ARRAY_UUID GENERATION",
              file=sys.stderr)
        return 2
    disk = Path(sys.argv[1])
    first = int(sys.argv[2])
    sectors = int(sys.argv[3])
    slot = int(sys.argv[4])
    members = int(sys.argv[5])
    array_uuid = bytes.fromhex(sys.argv[6])
    generation = int(sys.argv[7])
    if len(array_uuid) != 16:
        print("raid-meta: ARRAY_UUID must be 16 bytes of hex", file=sys.stderr)
        return 2
    if sectors < 3 or slot >= members or members < 1:
        print("raid-meta: invalid geometry", file=sys.stderr)
        return 2
    record = build(slot, members, array_uuid, generation, sectors)
    if len(record) != SECTOR:
        print("raid-meta: internal metadata size error", file=sys.stderr)
        return 1
    with disk.open("r+b") as f:
        for lba in (first + sectors - 1, first + sectors - 2):
            f.seek(lba * SECTOR)
            f.write(record)
    print("raid-meta: slot %d at LBA %d/%d on %s" %
          (slot, first + sectors - 2, first + sectors - 1, disk))
    return 0


if __name__ == "__main__":
    sys.exit(main())
