#!/usr/bin/env python3
"""Verify that a CrugOS journal crash is replayed by the host e2fsck.

The kernel commits a transaction with JBD2 checksums, publishes an unclean
superblock and exits without checkpointing. e2fsck must replay the journal
and leave the filesystem clean with the committed file contents.

usage: check-journal.py SERIAL_LOG EXIT_FILE DISK_IMAGE
"""

import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

SECTOR = 512
COMMIT_MARKER = b"CRUGOS: dirty journal committed (no checkpoint)"
CRASH_FILE_SIZE = 102400
PATTERN = bytes(((i * 13 + 7) & 0xFF) ^ 0xA5 for i in range(CRASH_FILE_SIZE))
DESCRIPTOR_PATTERN = re.compile(
    rb"CRUGOS: journal committed (\d+) block\(s\) in (\d+) descriptor\(s\)")


def fail(message):
    print("FAIL: " + message)
    return 1


def run(args):
    return subprocess.run(args, capture_output=True, text=True, check=False)


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


def debugfs_dump(image, path, out):
    result = run(["debugfs", "-R", "dump %s %s" % (path, out), str(image)])
    if result.returncode != 0 or not out.exists():
        raise RuntimeError("debugfs could not dump %s: %s%s" %
                           (path, result.stdout, result.stderr))
    return out.read_bytes()


def main():
    if len(sys.argv) != 4:
        print("usage: check-journal.py SERIAL_LOG EXIT_FILE DISK_IMAGE",
              file=sys.stderr)
        return 2
    serial = Path(sys.argv[1])
    exit_file = Path(sys.argv[2])
    disk = Path(sys.argv[3])
    if not serial.exists():
        return fail("missing serial log %s" % serial)
    log = serial.read_bytes()
    if COMMIT_MARKER not in log:
        return fail("kernel did not commit the dirty journal transaction")
    if b"CRUGOS: panic" in log or b"PANIC" in log:
        return fail("kernel panicked during the journal crash test")
    if not exit_file.exists():
        return fail("missing QEMU exit status file %s" % exit_file)
    status = exit_file.read_text().strip()
    if status != "33":
        return fail("guest exit status %s, expected 33" % status)
    match = DESCRIPTOR_PATTERN.search(log)
    if match is None:
        return fail("kernel did not report the journal descriptor chain")
    blocks, descriptors = int(match.group(1)), int(match.group(2))
    if blocks < CRASH_FILE_SIZE // 1024 or descriptors < 2:
        return fail("journal transaction used %d block(s) in %d descriptor(s), "
                    "expected a multi-descriptor chain" % (blocks, descriptors))
    tmpdir = Path(tempfile.mkdtemp(prefix=".checkjournal.", dir=str(disk.parent)))
    try:
        part = tmpdir / "part.img"
        extract_partition(disk, part)
        dirty = run(["e2fsck", "-fy", str(part)])
        if dirty.returncode not in (0, 1):
            return fail("e2fsck could not replay the journal: %s%s" %
                        (dirty.stdout, dirty.stderr))
        clean = run(["e2fsck", "-fn", str(part)])
        if clean.returncode != 0:
            return fail("filesystem is dirty after journal replay: %s%s" %
                        (clean.stdout, clean.stderr))
        data = debugfs_dump(part, "/selftest/j.bin", tmpdir / "j.bin")
        if data != PATTERN:
            return fail("/selftest/j.bin is %d bytes and does not match the "
                        "committed contents" % len(data))
        print("ok: e2fsck replayed the journal checksums and /selftest/j.bin "
              "holds the committed contents")
        return 0
    except (OSError, ValueError, RuntimeError) as exc:
        return fail(str(exc))
    finally:
        for child in tmpdir.glob("*"):
            child.unlink()
        tmpdir.rmdir()


if __name__ == "__main__":
    sys.exit(main())