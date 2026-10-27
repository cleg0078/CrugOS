#!/usr/bin/env python3
"""Verify the writable ext4 self-test persisted on a CrugOS GPT disk.

usage: check-disk.py DISK_IMAGE
"""

import struct
import subprocess
import sys
import tempfile
from pathlib import Path

SECTOR = 512


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


def run(args):
    return subprocess.run(args, capture_output=True, text=True, check=False)


def debugfs_dump(image, path, out):
    result = run(["debugfs", "-R", "dump %s %s" % (path, out), str(image)])
    if result.returncode != 0 or not out.exists():
        raise RuntimeError("debugfs could not dump %s: %s%s" %
                           (path, result.stdout, result.stderr))
    return out.read_bytes()


def main():
    if len(sys.argv) != 2:
        print("usage: check-disk.py DISK_IMAGE", file=sys.stderr)
        return 2
    disk = Path(sys.argv[1])
    if not disk.exists():
        return fail("missing disk image %s" % disk)
    tmpdir = Path(tempfile.mkdtemp(prefix=".checkdisk.", dir=str(disk.parent)))
    try:
        first, last = gpt_partition(disk)
        part = tmpdir / "part.img"
        with disk.open("rb") as src, part.open("wb") as dst:
            src.seek(first * SECTOR)
            remaining = (last - first + 1) * SECTOR
            while remaining:
                chunk = src.read(min(remaining, 1 << 20))
                if not chunk:
                    return fail("short read from disk image")
                dst.write(chunk)
                remaining -= len(chunk)
        fsck = run(["e2fsck", "-fn", str(part)])
        if fsck.returncode != 0:
            return fail("e2fsck rejected the root filesystem: %s%s" %
                        (fsck.stdout, fsck.stderr))
        hello = debugfs_dump(part, "/hello.txt", tmpdir / "hello.txt")
        if hello != b"hello from ext4\n":
            return fail("unexpected /hello.txt contents")
        if debugfs_dump(part, "/selftest/b.txt", tmpdir / "b.txt") != bytes(
                (i * 7 + 3) & 0xFF for i in range(1000)):
            return fail("/selftest/b.txt does not match the truncated pattern")
        stat = run(["debugfs", "-R", "stat /selftest", str(part)])
        if "Inode:" not in stat.stdout:
            return fail("/selftest directory missing")
        for stale in ("/selftest/a.txt", "/selftest/tmp.txt"):
            stat = run(["debugfs", "-R", "stat %s" % stale, str(part)])
            if "Inode:" in stat.stdout:
                return fail("stale self-test file remains: %s" % stale)
        print("ok: ext4 root is clean, /hello.txt and /selftest/b.txt persisted")
        return 0
    except (OSError, ValueError, RuntimeError) as exc:
        return fail(str(exc))
    finally:
        for child in tmpdir.glob("*"):
            child.unlink()
        tmpdir.rmdir()


if __name__ == "__main__":
    sys.exit(main())
