#!/usr/bin/env python3
"""Verify that CrugOS itself replays a crashed multi-descriptor journal.

The first boot on the journal gate image commits 100 blocks in one
transaction (two descriptors on a 1 KiB journal) and exits without
checkpointing. The second boot must replay that transaction at mount, show
the committed file contents, and leave a filesystem the host e2fsck accepts.

usage: check-journal-replay.py SERIAL_LOG EXIT_FILE DISK_IMAGE
"""

import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

SECTOR = 512
REPLAY_PATTERN = re.compile(rb"CRUGOS: journal replayed (\d+) block\(s\) at mount")
VERIFIED_MARKER = b"CRUGOS: journal replay verified"
MIN_BLOCKS = 100


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


def main():
    if len(sys.argv) != 4:
        print("usage: check-journal-replay.py SERIAL_LOG EXIT_FILE DISK_IMAGE",
              file=sys.stderr)
        return 2
    serial = Path(sys.argv[1])
    exit_file = Path(sys.argv[2])
    disk = Path(sys.argv[3])
    if not serial.exists():
        return fail("missing serial log %s" % serial)
    log = serial.read_bytes()
    if b"CRUGOS: panic" in log or b"PANIC" in log:
        return fail("kernel panicked while replaying the journal")
    match = REPLAY_PATTERN.search(log)
    if match is None:
        return fail("the kernel did not replay the journal at mount")
    blocks = int(match.group(1))
    if blocks < MIN_BLOCKS:
        return fail("only %d block(s) were replayed, expected at least %d" %
                    (blocks, MIN_BLOCKS))
    if VERIFIED_MARKER not in log:
        return fail("the replayed file contents did not match the commit")
    if not exit_file.exists():
        return fail("missing QEMU exit status file %s" % exit_file)
    status = exit_file.read_text().strip()
    if status != "33":
        return fail("guest exit status %s, expected 33" % status)
    tmpdir = Path(tempfile.mkdtemp(prefix=".checkreplay.", dir=str(disk.parent)))
    try:
        part = tmpdir / "part.img"
        extract_partition(disk, part)
        clean = run(["e2fsck", "-fn", str(part)])
        if clean.returncode != 0:
            return fail("filesystem is damaged after the kernel replay: %s%s" %
                        (clean.stdout, clean.stderr))
        print("ok: the kernel replayed %d journal block(s) and left a clean "
              "filesystem" % blocks)
        return 0
    except (OSError, ValueError, RuntimeError) as exc:
        return fail(str(exc))
    finally:
        for child in tmpdir.glob("*"):
            child.unlink()
        tmpdir.rmdir()


if __name__ == "__main__":
    sys.exit(main())
