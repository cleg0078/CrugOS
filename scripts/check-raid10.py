#!/usr/bin/env python3
"""Verify CrugOS RAID10 member disks after a boot test."""

import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

SECTOR = 512
MAGIC = 0x43524131
FORMAT = "<IHHIIIIQQQ16s16s428sI"
LEVEL_10 = 10
MEMBERS = 4
CHUNK = 128


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


def check_meta(disk, first, last):
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
    magic, version, level, members, slot, chunk, state, array_sectors, \
        member_sectors, generation, _, _, _, checksum = fields
    if magic != MAGIC or version != 1 or level != LEVEL_10:
        raise ValueError("bad RAID10 metadata header")
    if members != MEMBERS or chunk != CHUNK or state != 0:
        raise ValueError("bad RAID10 metadata geometry")
    if slot >= MEMBERS:
        raise ValueError("member slot out of range")
    if member_sectors != sectors or array_sectors != sectors - 2:
        raise ValueError("metadata does not match partition size")
    body = rec[:SECTOR - 4] + b"\x00" * 4
    if (zlib.crc32(body) & 0xFFFFFFFF) != checksum:
        raise ValueError("metadata checksum mismatch")
    return slot, array_sectors, generation


def read_member_sectors(disk, first, lba, count):
    with disk.open("rb") as f:
        f.seek((first + lba) * SECTOR)
        data = f.read(count * SECTOR)
    if len(data) != count * SECTOR:
        raise ValueError("short read from %s at array sector %d" % (disk, lba))
    return data


def chunk_count(array_sectors):
    return (array_sectors + CHUNK - 1) // CHUNK


def rebuild_array(members, array_sectors, out):
    """Assemble one logical image, taking each chunk from one member of its pair."""
    by_pair = {}
    for slot, disk, first in sorted(members):
        by_pair.setdefault(slot // 2, (slot, disk, first))
    with out.open("wb") as dst:
        for chunk in range(chunk_count(array_sectors)):
            pair = chunk % (MEMBERS // 2)
            slot, disk, first = by_pair[pair]
            lba = chunk * CHUNK
            count = min(CHUNK, array_sectors - lba)
            dst.write(read_member_sectors(disk, first, lba, count))


def check_mirrors(members, array_sectors):
    """Each mirror pair must hold identical bytes for its own chunks."""
    by_pair = {}
    for slot, disk, first in sorted(members):
        by_pair.setdefault(slot // 2, []).append((slot, disk, first))
    for chunk in range(chunk_count(array_sectors)):
        pair = chunk % (MEMBERS // 2)
        group = by_pair[pair]
        lba = chunk * CHUNK
        count = min(CHUNK, array_sectors - lba)
        reference = read_member_sectors(group[0][1], group[0][2], lba, count)
        for slot, disk, first in group[1:]:
            if read_member_sectors(disk, first, lba, count) != reference:
                raise ValueError("mirror pair %d differs at chunk %d (slot %d)" %
                                 (pair, chunk, slot))


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
    if len(sys.argv) < 2 or len(sys.argv) > 5:
        print("usage: check-raid10.py DISK_A [DISK_B [DISK_C [DISK_D]]]",
              file=sys.stderr)
        return 2
    disks = [Path(p) for p in sys.argv[1:]]
    for disk in disks:
        if not disk.exists():
            return fail("missing disk image %s" % disk)
    tmpdir = Path(tempfile.mkdtemp(prefix=".checkraid10.", dir=str(disks[0].parent)))
    try:
        first, last = gpt_partition(disks[0])
        sectors = last - first + 1
        slots = set()
        generations = set()
        array_sectors = None
        members = []
        for disk in disks:
            first_d, last_d = gpt_partition(disk)
            if (first_d, last_d) != (first, last):
                return fail("members have different partition geometry")
            slot, sectors_d, generation = check_meta(disk, first_d, last_d)
            if slot in slots:
                return fail("duplicate member slot %d" % slot)
            slots.add(slot)
            generations.add(generation)
            if array_sectors is None:
                array_sectors = sectors_d
            elif array_sectors != sectors_d:
                return fail("members have different array sizes")
            members.append((slot, disk, first_d))
        if len(generations) != 1:
            return fail("members have different generations")
        pairs = {slot // 2 for slot in slots}
        if pairs != set(range(MEMBERS // 2)):
            return fail("degraded array does not cover every mirror pair")
        check_mirrors(members, array_sectors)
        image = tmpdir / "array.img"
        rebuild_array(members, array_sectors, image)
        error = check_fs(image, tmpdir)
        if error:
            return fail(error)
        print("ok: RAID10 metadata and ext4 root clean "
              "(%d/%d members, %d mirror pairs, chunk %d sectors, generation %d)" %
              (len(disks), MEMBERS, MEMBERS // 2, CHUNK, generations.pop()))
        return 0
    except (OSError, ValueError, RuntimeError) as exc:
        return fail(str(exc))
    finally:
        for child in tmpdir.glob("*"):
            child.unlink()
        tmpdir.rmdir()


if __name__ == "__main__":
    sys.exit(main())
