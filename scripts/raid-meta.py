#!/usr/bin/env python3
"""Write CrugOS RAID1/RAID10 member metadata into a GPT partition."""

import os
import struct
import sys
import zlib
from pathlib import Path

SECTOR = 512
MAGIC = 0x43524131
VERSION = 1
LEVEL_1 = 1
LEVEL_10 = 10

# u32 magic, u16 version, u16 level, u32 member_count, u32 slot,
# u32 chunk_sectors, u32 state, u64 array_sectors, u64 member_sectors,
# u64 generation, 16s array_uuid, 16s member_uuid, 428s reserved, u32 checksum
FORMAT = "<IHHIIIIQQQ16s16s428sI"


def build(slot, members, array_uuid, generation, member_sectors, level, chunk):
    array_sectors = member_sectors - 2
    member_uuid = os.urandom(16)
    body = struct.pack(FORMAT, MAGIC, VERSION, level, members, slot, chunk, 0,
                       array_sectors, member_sectors, generation,
                       array_uuid, member_uuid, b"\x00" * 428, 0)
    checksum = zlib.crc32(body) & 0xFFFFFFFF
    return body[:-4] + struct.pack("<I", checksum)


def main():
    if len(sys.argv) not in (8, 9, 10):
        print("usage: raid-meta.py DISK FIRST_LBA SECTORS SLOT MEMBERS ARRAY_UUID "
              "GENERATION [LEVEL [CHUNK_SECTORS]]", file=sys.stderr)
        return 2
    disk = Path(sys.argv[1])
    first = int(sys.argv[2])
    sectors = int(sys.argv[3])
    slot = int(sys.argv[4])
    members = int(sys.argv[5])
    array_uuid = bytes.fromhex(sys.argv[6])
    generation = int(sys.argv[7])
    level = int(sys.argv[8]) if len(sys.argv) >= 9 else LEVEL_1
    chunk = int(sys.argv[9]) if len(sys.argv) >= 10 else 0
    if len(array_uuid) != 16:
        print("raid-meta: ARRAY_UUID must be 16 bytes of hex", file=sys.stderr)
        return 2
    if sectors < 3 or slot >= members or members < 1:
        print("raid-meta: invalid geometry", file=sys.stderr)
        return 2
    if level == LEVEL_1:
        if chunk != 0 or members > 8:
            print("raid-meta: RAID1 needs chunk=0 and 1..8 members",
                  file=sys.stderr)
            return 2
    elif level == LEVEL_10:
        if chunk == 0 or members < 4 or members > 8 or members % 2:
            print("raid-meta: RAID10 needs even 4..8 members and chunk>0",
                  file=sys.stderr)
            return 2
    else:
        print("raid-meta: unsupported level", file=sys.stderr)
        return 2
    record = build(slot, members, array_uuid, generation, sectors, level, chunk)
    if len(record) != SECTOR:
        print("raid-meta: internal metadata size error", file=sys.stderr)
        return 1
    with disk.open("r+b") as f:
        for lba in (first + sectors - 1, first + sectors - 2):
            f.seek(lba * SECTOR)
            f.write(record)
    print("raid-meta: level %d slot %d at LBA %d/%d on %s" %
          (level, slot, first + sectors - 2, first + sectors - 1, disk))
    return 0


if __name__ == "__main__":
    sys.exit(main())
