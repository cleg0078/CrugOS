#!/usr/bin/env python3
"""Enable JBD2 checksum v3 on the ext4 journal of a disk or partition image.

mke2fs 1.47 leaves the journal superblock without checksums even when the
ext4 superblock uses metadata_csum, so CrugOS turns them on here. The
journal superblock is the first block of inode 8.

usage: journal-csum.py IMAGE
"""

import struct
import subprocess
import sys
import tempfile
from pathlib import Path

SECTOR = 512
JBD2_MAGIC = b"\xc0\x3b\x39\x98"
JBD2_SUPERBLOCK_V2 = 4
JBD2_INCOMPAT_CSUM_V2 = 0x0008
JBD2_INCOMPAT_CSUM_V3 = 0x0010
JBD2_COMPAT_CHECKSUM = 0x0001
JBD2_CRC32C_CHKSUM = 4
SB_INCOMPAT = 0x28
SB_COMPAT = 0x24
SB_CSUM_TYPE = 0x50
SB_CSUM = 0xFC
SB_CSUM_LEN = 0x400
CRC32C_POLY = 0x82F63B78


def _table():
    table = []
    for i in range(256):
        c = i
        for _ in range(8):
            c = (c >> 1) ^ CRC32C_POLY if c & 1 else c >> 1
        table.append(c)
    return table


_TABLE = _table()


def crc32c(seed, data):
    c = seed & 0xFFFFFFFF
    for b in data:
        c = _TABLE[(c ^ b) & 0xFF] ^ (c >> 8)
    return c & 0xFFFFFFFF


def fail(message):
    print("FAIL: " + message)
    return 1


def gpt_partition(disk, name="crugroot"):
    with disk.open("rb") as f:
        f.seek(SECTOR)
        header = f.read(SECTOR)
        if header[0:8] != b"EFI PART":
            raise ValueError("missing GPT header")
        entries_lba = struct.unpack_from("<Q", header, 72)[0]
        count = struct.unpack_from("<I", header, 80)[0]
        entry_size = struct.unpack_from("<I", header, 84)[0]
        if count == 0 or entry_size < 128:
            raise ValueError("invalid GPT partition table")
        f.seek(entries_lba * SECTOR)
        entries = f.read(count * entry_size)
    for i in range(count):
        off = i * entry_size
        first, last = struct.unpack_from("<QQ", entries, off + 32)
        raw_name = entries[off + 56:off + 128]
        part_name = raw_name.decode("utf-16le", errors="replace").split("\x00", 1)[0]
        if part_name == name:
            if last < first:
                raise ValueError("empty root partition")
            return first, last
    raise ValueError("partition %r not found" % name)


def extract_partition(disk, out):
    first, last = gpt_partition(disk)
    with disk.open("rb") as src, out.open("wb") as dst:
        src.seek(first * SECTOR)
        remaining = (last - first + 1) * SECTOR
        while remaining:
            chunk = src.read(min(remaining, 1 << 20))
            if not chunk:
                raise RuntimeError("short read from disk image")
            dst.write(chunk)
            remaining -= len(chunk)
    return first * SECTOR


def journal_sb_block(part, block_size):
    result = subprocess.run(["debugfs", "-R", "blocks <8>", str(part)],
                            capture_output=True, text=True, check=False)
    if result.returncode != 0:
        raise RuntimeError("debugfs could not list the journal inode: %s%s" %
                           (result.stdout, result.stderr))
    numbers = []
    for token in result.stdout.split():
        if token.isdigit():
            numbers.append(int(token))
    if not numbers:
        raise RuntimeError("journal inode 8 has no blocks")
    return numbers[0]


def patch(part, base_offset):
    with part.open("r+b") as f:
        f.seek(1024)
        sb = f.read(1024)
        if len(sb) != 1024 or struct.unpack_from("<H", sb, 0x38)[0] != 0xEF53:
            raise ValueError("no ext4 superblock in %s" % part)
        log_block = struct.unpack_from("<I", sb, 0x18)[0]
        if log_block > 2:
            raise ValueError("unsupported ext4 block size")
        block_size = 1024 << log_block
        jblock = journal_sb_block(part, block_size)
        joff = jblock * block_size
        f.seek(joff)
        jsb = bytearray(f.read(SB_CSUM_LEN))
        if len(jsb) != SB_CSUM_LEN or bytes(jsb[0:4]) != JBD2_MAGIC:
            raise ValueError("journal superblock not found at block %d" % jblock)
        if struct.unpack_from(">I", jsb, 4)[0] != JBD2_SUPERBLOCK_V2:
            raise ValueError("journal superblock has an unexpected type")
        if struct.unpack_from(">I", jsb, 12)[0] != block_size:
            raise ValueError("journal block size does not match the filesystem")
        incompat = struct.unpack_from(">I", jsb, SB_INCOMPAT)[0]
        compat = struct.unpack_from(">I", jsb, SB_COMPAT)[0]
        if incompat & JBD2_INCOMPAT_CSUM_V3:
            print("journal-csum: journal checksum v3 already enabled")
            return jblock
        if incompat & JBD2_INCOMPAT_CSUM_V2:
            raise ValueError("journal already uses checksum v2")
        compat &= ~JBD2_COMPAT_CHECKSUM
        struct.pack_into(">I", jsb, SB_COMPAT, compat)
        struct.pack_into(">I", jsb, SB_INCOMPAT, incompat | JBD2_INCOMPAT_CSUM_V3)
        jsb[SB_CSUM_TYPE] = JBD2_CRC32C_CHKSUM
        struct.pack_into(">I", jsb, SB_CSUM, 0)
        struct.pack_into(">I", jsb, SB_CSUM, crc32c(0xFFFFFFFF, bytes(jsb)))
        f.seek(joff)
        f.write(jsb)
        f.flush()
    return jblock


def main():
    if len(sys.argv) != 2:
        print("usage: journal-csum.py IMAGE", file=sys.stderr)
        return 2
    image = Path(sys.argv[1])
    if not image.exists():
        return fail("missing image %s" % image)
    try:
        with image.open("rb") as f:
            f.seek(SECTOR)
            in_place = f.read(8) != b"EFI PART"
        if in_place:
            jblock = patch(image, 0)
            print("journal-csum: enabled JBD2 checksum v3 in %s (journal block %d)" %
                  (image, jblock))
            return 0
        tmpdir = Path(tempfile.mkdtemp(prefix=".jcsum.", dir=str(image.parent)))
        try:
            part = tmpdir / "part.img"
            base = extract_partition(image, part)
            jblock = patch(part, base)
            first, last = gpt_partition(image)
            with part.open("rb") as src, image.open("r+b") as dst:
                src.seek(0)
                dst.seek(first * SECTOR)
                remaining = (last - first + 1) * SECTOR
                while remaining:
                    chunk = src.read(min(remaining, 1 << 20))
                    if not chunk:
                        raise RuntimeError("short read from the patched partition")
                    dst.write(chunk)
                    remaining -= len(chunk)
            print("journal-csum: enabled JBD2 checksum v3 in %s (journal block %d)" %
                  (image, jblock))
            return 0
        finally:
            for child in tmpdir.glob("*"):
                child.unlink()
            tmpdir.rmdir()
    except (OSError, ValueError, RuntimeError) as exc:
        return fail(str(exc))


if __name__ == "__main__":
    sys.exit(main())