#!/usr/bin/env python3
"""Boot crafted disk images and prove the kernel rejects them.

The hardening pass 1 validators (include/crug/harden.h) are exercised on the
host by tests/host/test_harden.c. This gate goes one step further and feeds
the real kernel hostile bytes: each case copies the pristine image, patches
one field of the superblock, the root inode or the crypt header, boots it
under QEMU and requires a specific rejection marker without a panic and
without ever mounting the filesystem.

usage: run-hardening.py --disk build/x86_64/disk.img \
                        --crypt-disk build/x86_64/crypt.img \
                        --crypt-pass crugos-test \
                        --serial-log build/x86_64/serial.log \
                        --exit-code build/x86_64/exit-code.txt

The plain image also boots once unmodified as the control case, so a green
run proves both that the rejections happen and that the validation itself
did not break a valid filesystem.
"""

import argparse
import re
import shutil
import struct
import subprocess
import sys
from pathlib import Path

SECTOR = 512

# ext4 superblock fields used to find the root inode (offsets are relative to
# the start of the 1 KiB superblock at block 1).
SB_INODES_COUNT = 0x00
SB_BLOCKS_COUNT = 0x04
SB_FIRST_DATA_BLOCK = 0x14
SB_LOG_BLOCK_SIZE = 0x18
SB_BLOCKS_PER_GROUP = 0x20
SB_INODES_PER_GROUP = 0x28
SB_MAGIC = 0x38
SB_INODE_SIZE = 0x58
SB_INCOMPAT = 0x60
SB_DESC_SIZE = 0xFE
SB_CHECKSUM_SEED = 0x270
SB_UUID = 0x68
EXT4_MAGIC = 0xEF53
EXT4_FEATURE_INCOMPAT_CSUM_SEED = 0x2000
EXT4_FEATURE_INCOMPAT_64BIT = 0x80
EXT4_INODE_GENERATION = 0x64
EXT4_INODE_CSUM_LO = 0x7C
EXT4_INODE_CSUM_HI = 0x82
CRYPT_KDF_M_COST = 0x18
CRYPT_VOLUME_SECTORS = 0x38
CRC32C_POLY = 0x82F63B78

PANIC_MARKERS = ("CRUGOS: panic", "trap   : page fault")
MOUNT_MARKERS = ("CRUGOS: ext4 ready", "CRUGOS: storage ready")
CONTROL_PHASE_RE = re.compile(r"CRUGOS: phase [0-9a-z]+ boot ok")
CONTROL_MARKERS = (
    "CRUGOS: ext4 ready",
    "CRUGOS: storage ready",
    "CRUGOS: writable storage ok",
    "CRUGOS: selftest ok",
)

MARKER_GEOMETRY = "CRUGOS: ext4 mount rejected (geometry)"
MARKER_ROOT_EXTENT = "CRUGOS: ext4 mount rejected (root extent)"
MARKER_KDF_POLICY = "CRUGOS: crypt unlock failed (kdf policy)"
MARKER_VOLUME = "CRUGOS: crypt unlock failed (volume exceeds partition)"


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


def u16(buf, off):
    return struct.unpack_from("<H", buf, off)[0]


def u32(buf, off):
    return struct.unpack_from("<I", buf, off)[0]


def put32(buf, off, value):
    struct.pack_into("<I", buf, off, value & 0xFFFFFFFF)


def gpt_partition(disk, name):
    """Return (first_lba, last_lba) of the GPT partition called name."""
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
        text = raw_name.decode("utf-16le", errors="replace")
        if text.split(chr(0), 1)[0] == name:
            if last < first:
                raise ValueError("empty partition %r" % name)
            return first, last
    raise ValueError("partition %r not found" % name)


def inode_location(buf, sb):
    """Return (inode_offset, inode_size, csum_seed) for inode 2."""
    block_size = 1024 << u32(buf, sb + SB_LOG_BLOCK_SIZE)
    first_data_block = u32(buf, sb + SB_FIRST_DATA_BLOCK)
    inodes_per_group = u32(buf, sb + SB_INODES_PER_GROUP)
    inode_size = u32(buf, sb + SB_INODE_SIZE) or 128
    incompat = u32(buf, sb + SB_INCOMPAT)
    desc_size = 32
    if incompat & EXT4_FEATURE_INCOMPAT_64BIT:
        desc_size = u16(buf, sb + SB_DESC_SIZE)
    fs_start = sb - block_size
    gdt_block = first_data_block + 1
    desc = fs_start + gdt_block * block_size
    table = u32(buf, desc + 8)
    if desc_size >= 64:
        table |= u16(buf, desc + 0x28) << 32
    index = (2 - 1) % inodes_per_group
    off = index * inode_size
    inode = fs_start + table * block_size + off
    if incompat & EXT4_FEATURE_INCOMPAT_CSUM_SEED:
        seed = u32(buf, sb + SB_CHECKSUM_SEED)
    else:
        seed = crc32c(0xFFFFFFFF, buf[sb + SB_UUID:sb + SB_UUID + 16])
    return inode, inode_size, seed


def fix_inode_csum(buf, inode, inode_size, seed):
    """Recompute the metadata_csum of one inode after editing it."""
    generation = u32(buf, inode + EXT4_INODE_GENERATION)
    struct.pack_into("<H", buf, inode + EXT4_INODE_CSUM_LO, 0)
    if inode_size >= EXT4_INODE_CSUM_HI + 2:
        struct.pack_into("<H", buf, inode + EXT4_INODE_CSUM_HI, 0)
    crc = crc32c(crc32c(seed, struct.pack("<I", 2)),
                 struct.pack("<I", generation))
    crc = crc32c(crc, buf[inode:inode + inode_size])
    struct.pack_into("<H", buf, inode + EXT4_INODE_CSUM_LO, crc & 0xFFFF)
    if inode_size >= EXT4_INODE_CSUM_HI + 2:
        struct.pack_into("<H", buf, inode + EXT4_INODE_CSUM_HI,
                         (crc >> 16) & 0xFFFF)


def patch_geometry_bpg(buf, part):
    put32(buf, part + 1024 + SB_BLOCKS_PER_GROUP, 100000)


def patch_geometry_ipg(buf, part):
    put32(buf, part + 1024 + SB_INODES_PER_GROUP, 100000)


def patch_root_extent(buf, part):
    sb = part + 1024
    inode, inode_size, seed = inode_location(buf, sb)
    if u16(buf, inode + 40) != 0xF30A:
        raise ValueError("root inode has no extent header")
    # eh_max lives 4 bytes into the extent header; the inline root only owns
    # the 60-byte i_block area, so max=5 would walk 72 bytes.
    struct.pack_into("<H", buf, inode + 44, 5)
    fix_inode_csum(buf, inode, inode_size, seed)


def patch_kdf_high(buf, part):
    put32(buf, part + CRYPT_KDF_M_COST, 131072)


def patch_kdf_low(buf, part):
    put32(buf, part + CRYPT_KDF_M_COST, 512)


def patch_volume_huge(buf, part):
    struct.pack_into("<Q", buf, part + CRYPT_VOLUME_SECTORS,
                     0x0000FFFFFFFFFFFF)


CASES = [
    {"name": "control", "image": "disk", "patch": None, "exit": 33},
    {"name": "geometry-bpg", "image": "disk", "patch": patch_geometry_bpg,
     "marker": MARKER_GEOMETRY, "exit": 35},
    {"name": "geometry-ipg", "image": "disk", "patch": patch_geometry_ipg,
     "marker": MARKER_GEOMETRY, "exit": 35},
    {"name": "root-extent", "image": "disk", "patch": patch_root_extent,
     "marker": MARKER_ROOT_EXTENT, "exit": 35},
    {"name": "kdf-high", "image": "crypt", "patch": patch_kdf_high,
     "marker": MARKER_KDF_POLICY, "exit": 35},
    {"name": "kdf-low", "image": "crypt", "patch": patch_kdf_low,
     "marker": MARKER_KDF_POLICY, "exit": 35},
    {"name": "volume-huge", "image": "crypt", "patch": patch_volume_huge,
     "marker": MARKER_VOLUME, "exit": 35},
]


def boot(args, image, log, exit_code, passphrase):
    cmd = [
        args.make, "--no-print-directory", "run",
        "DISK_IMG=" + str(image),
        "CRYPT_PASS=" + passphrase,
        "CMDLINE_EXTRA=",
        "QEMU_DISPLAY=none",
        "NET_MODE=none",
        "SERIAL_TCP=",
        "SERIAL_LOG=" + str(log),
        "QEMU_EXIT=" + str(exit_code),
        "TIMEOUT_SEC=%d" % args.timeout,
    ]
    try:
        subprocess.run(cmd, cwd=args.repo, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT,
                       timeout=args.timeout + 120)
    except subprocess.TimeoutExpired:
        return "the make run wrapper did not finish"
    if not exit_code.exists():
        return "QEMU wrote no exit status (timeout or crash)"
    return None


def log_text(path):
    return path.read_text(errors="replace") if path.exists() else ""


def tail(text, count=12):
    rows = [r for r in text.splitlines() if r.strip()]
    return chr(10).join(rows[-count:])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo",
                    default=str(Path(__file__).resolve().parent.parent))
    ap.add_argument("--make", default="make")
    ap.add_argument("--disk", required=True)
    ap.add_argument("--crypt-disk", required=True)
    ap.add_argument("--crypt-pass", default="crugos-test")
    ap.add_argument("--serial-log", required=True)
    ap.add_argument("--exit-code", required=True)
    ap.add_argument("--timeout", type=int, default=120)
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    disk = Path(args.disk)
    crypt_disk = Path(args.crypt_disk)
    if not disk.exists():
        return fail("missing disk image %s" % disk)
    if not crypt_disk.exists():
        return fail("missing crypt image %s" % crypt_disk)
    if not args.crypt_pass:
        return fail("the crypt cases need --crypt-pass")

    base = Path(args.serial_log)
    case_dir = base.parent / "hardening"
    if case_dir.exists():
        shutil.rmtree(case_dir)
    case_dir.mkdir(parents=True, exist_ok=True)
    print("harden: case dir %s" % case_dir)

    rejected = 0
    control_ok = False
    for case in CASES:
        name = case["name"]
        source = crypt_disk if case["image"] == "crypt" else disk
        passphrase = args.crypt_pass if case["image"] == "crypt" else ""
        image = case_dir / (name + ".img")
        log = case_dir / (name + ".log")
        exit_code = case_dir / (name + ".exit")
        shutil.copyfile(source, image)
        if case["patch"] is not None:
            data = bytearray(image.read_bytes())
            try:
                part_name = ("crugcrypt" if case["image"] == "crypt"
                             else "crugroot")
                first, _last = gpt_partition(image, part_name)
                part = first * SECTOR
                case["patch"](data, part)
            except (ValueError, struct.error) as exc:
                return fail("case %s: cannot patch the image (%s)"
                            % (name, exc))
            image.write_bytes(data)
            print("harden: %s patched at partition offset %d"
                  % (name, part))
        else:
            print("harden: %s boots the pristine image" % name)

        error = boot(args, image, log, exit_code, passphrase)
        text = log_text(log)
        if error is not None:
            return fail("case %s: %s" % (name, error) + chr(10) + tail(text))
        raw = exit_code.read_text().strip()
        status = int(raw) if raw else -1
        if status != case["exit"]:
            return fail("case %s exited %d, expected %d"
                        % (name, status, case["exit"]) + chr(10) + tail(text))
        for marker in PANIC_MARKERS:
            if marker in text:
                return fail("case %s hit %r" % (name, marker) + chr(10) +
                            tail(text))
        if case["patch"] is None:
            if not CONTROL_PHASE_RE.search(text):
                return fail("control boot never printed the phase marker"
                            + chr(10) + tail(text))
            for marker in CONTROL_MARKERS:
                if marker not in text:
                    return fail("control boot never printed %r" % marker
                                + chr(10) + tail(text))
            control_ok = True
            print("ok: harden control boot (exit 33)")
        else:
            if case["marker"] not in text:
                return fail("case %s did not print %r"
                            % (name, case["marker"]) + chr(10) + tail(text))
            for marker in MOUNT_MARKERS:
                if marker in text:
                    return fail("case %s still mounted the filesystem (%r)"
                                % (name, marker) + chr(10) + tail(text))
            rejected += 1
            print("ok: harden %s -> %s (exit 35)"
                  % (name, case["marker"]))
        if not args.keep:
            image.unlink()

    if not control_ok:
        return fail("the control boot never ran")
    print("ok: hardening pass 1 rejected %d crafted image(s) "
          "(control boot exit 33)" % rejected)
    if rejected != len(CASES) - 1:
        return fail("only %d of %d hostile images were rejected"
                    % (rejected, len(CASES) - 1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
