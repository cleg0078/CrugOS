#!/usr/bin/env python3
"""Verify a CrugOS RAID1 member disk after a boot test."""

import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

SECTOR = 512
MAGIC = 0x43524131
FORMAT = "<IHHIIIIQQQ16s16s428sI"


def fail(message):
    print("FAIL: " + message)
    return 1


def gpt_partition(disk, name="crugraid"):
    with disk.open("rb") as f:
        f.seek(SECTOR)
        header = f.read(SECTOR)
        if header[0:8] != b"EFI PART":
            raise ValueError("missing GPT header")
        entries_lba = struct.unpack_from("<Q", header, 72)[0]
        count = struct.unpack_from("<I", header, 80)[0]
        entry_size = struct.unpack_from("<I", header, 84)[0]
        f.seek(entries_lba * SECTOR)
        entries = f.read(count * entry_size)
    for i in range(count):
        off = i * entry_size
        first, last = struct.unpack_from("<QQ", entries, off + 32)
        raw_name = entries[off + 56:off + 128]
        part_name = raw_name.decode("utf-16le", errors="replace").split("\x00", 1)[0]
        if part_name == name:
            if last < first + 2:
                raise ValueError("RAID partition too small")
            return first, last
    raise ValueError("partition %r not found" % name)


def run(args):
    return subprocess.run(args, capture_output=True, text=True, check=False)


def check_meta(disk, first, last, slot):
    sectors = last - first + 1
    with disk.open("rb") as f:
        records = []
        for lba in (last, last - 1):
            f.seek(lba * SECTOR)
            records.append(f.read(SECTOR))
    if records[0] != records[1]:
        raise ValueError("metadata copies differ")
    rec = records[0]
    fields = struct.unpack(FORMAT, rec)
    magic, version, level, members, got_slot, chunk, state, array_sectors, \
        member_sectors, generation, _, _, _, checksum = fields
    if magic != MAGIC or version != 1 or level != 1:
        raise ValueError("bad metadata header")
    if members != 2 or got_slot != slot or chunk != 0 or state != 0:
        raise ValueError("bad metadata geometry")
    if member_sectors != sectors or array_sectors != sectors - 2:
        raise ValueError("metadata does not match partition size")
    body = rec[:SECTOR - 4] + b"\x00" * 4
    if (zlib.crc32(body) & 0xFFFFFFFF) != checksum:
        raise ValueError("metadata checksum mismatch")
    return array_sectors, generation


def extract_data(disk, first, sectors, out):
    with disk.open("rb") as src, out.open("wb") as dst:
        src.seek(first * SECTOR)
        remaining = sectors * SECTOR
        while remaining:
            chunk = src.read(min(remaining, 1 << 20))
            if not chunk:
                raise ValueError("short read from %s" % disk)
            dst.write(chunk)
            remaining -= len(chunk)


def debugfs_dump(image, path, out):
    result = run(["debugfs", "-R", "dump %s %s" % (path, out), str(image)])
    if result.returncode != 0 or not out.exists():
        raise RuntimeError("debugfs could not dump %s: %s%s" %
                           (path, result.stdout, result.stderr))
    return out.read_bytes()


def check_fs(image, tmpdir):
    fsck = run(["e2fsck", "-fn", str(image)])
    if fsck.returncode != 0:
        return "e2fsck rejected the array filesystem: %s%s" % (fsck.stdout, fsck.stderr)
    if debugfs_dump(image, "/hello.txt", tmpdir / "hello.txt") != b"hello from ext4\n":
        return "unexpected /hello.txt contents"
    pattern = bytes((i * 7 + 3) & 0xFF for i in range(1000))
    if debugfs_dump(image, "/selftest/b.txt", tmpdir / "b.txt") != pattern:
        return "/selftest/b.txt does not match the truncated pattern"
    stat = run(["debugfs", "-R", "stat /selftest", str(image)])
    if "Inode:" not in stat.stdout:
        return "/selftest directory missing"
    for stale in ("/selftest/a.txt", "/selftest/tmp.txt"):
        stat = run(["debugfs", "-R", "stat %s" % stale, str(image)])
        if "Inode:" in stat.stdout:
            return "stale self-test file remains: %s" % stale
    return None


def main():
    if len(sys.argv) not in (2, 3):
        print("usage: check-raid.py DISK_A [DISK_B]", file=sys.stderr)
        return 2
    a = Path(sys.argv[1])
    b = Path(sys.argv[2]) if len(sys.argv) == 3 else None
    for disk in (a, b):
        if disk is not None and not disk.exists():
            return fail("missing disk image %s" % disk)
    tmpdir = Path(tempfile.mkdtemp(prefix=".checkraid.", dir=str(a.parent)))
    try:
        first, last = gpt_partition(a)
        sectors = last - first + 1
        array_sectors, generation = check_meta(a, first, last, 0)
        image_a = tmpdir / "a.img"
        extract_data(a, first, sectors - 2, image_a)
        error = check_fs(image_a, tmpdir)
        if error:
            return fail(error)
        if b is not None:
            first_b, last_b = gpt_partition(b)
            if (first_b, last_b) != (first, last):
                return fail("members have different partition geometry")
            _, generation_b = check_meta(b, first_b, last_b, 1)
            if generation_b != generation:
                return fail("members have different generations")
            image_b = tmpdir / "b.img"
            extract_data(b, first_b, sectors - 2, image_b)
            if image_a.read_bytes() != image_b.read_bytes():
                return fail("RAID1 member data differs")
        print("ok: RAID1 metadata and ext4 root clean (%u data sectors, generation %u)" %
              (array_sectors, generation))
        return 0
    except (OSError, ValueError, RuntimeError) as exc:
        return fail(str(exc))
    finally:
        for child in tmpdir.glob("*"):
            child.unlink()
        tmpdir.rmdir()


if __name__ == "__main__":
    sys.exit(main())
