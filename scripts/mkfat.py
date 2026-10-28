#!/usr/bin/env python3
"""Populate a FAT16 image from a directory tree.

The UEFI hybrid ISO carries a FAT image in its El Torito boot entry, and this
host has no mtools, so the file system is written directly.  The output is
deterministic: entries are emitted in sorted order, every timestamp is fixed
and unused bytes stay zero, so one source tree always produces the same image.

    scripts/mkfat.py --source esp --output build/x86_64/iso/tree/boot/efi.img
    scripts/mkfat.py --list build/x86_64/iso/tree/boot/efi.img

Only 8.3 names are supported.  A longer name is a hard error instead of a
silent truncation, because EFI/BOOT/BOOTX64.EFI and crugos/kernel.elf both fit
and anything else would be ambiguous for the loader.
"""

import argparse
import os
import struct
import sys

SECTOR = 512
SPC = 8
CLUSTER = SECTOR * SPC
RESERVED = 1
NUM_FATS = 2
ROOT_ENTRIES = 512
ROOT_SECTORS = ROOT_ENTRIES * 32 // SECTOR
MEDIA = 0xF8
FAT16_MIN = 4085
FAT16_MAX = 65524
EOC = 0xFFFF

# 2026-01-01 00:00:00, the one timestamp used by every directory entry.
FIXED_DATE = ((2026 - 1980) << 9) | (1 << 5) | 1
FIXED_TIME = 0

ATTR_VOLUME = 0x08
ATTR_DIR = 0x10
ATTR_ARCHIVE = 0x20

LETTERS = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-~!#$%&()@^{}"


class FatError(Exception):
    pass


def entry_name(name):
    if not name or name in (".", ".."):
        raise FatError("mkfat: invalid name: " + repr(name))
    if "." in name:
        base, _, ext = name.rpartition(".")
    else:
        base, ext = name, ""
    if not base or len(base) > 8 or len(ext) > 3:
        raise FatError("mkfat: name does not fit 8.3: " + name)
    flags = 0
    raw = bytearray()
    for part, bit, width in ((base, 0x08, 8), (ext, 0x10, 3)):
        if any(c.islower() for c in part):
            flags |= bit
        upper = part.upper()
        for c in upper:
            if c not in LETTERS:
                raise FatError("mkfat: unsupported character in name: " + name)
        raw += upper.encode("ascii").ljust(width, b" ")
    return bytes(raw), flags


def clusters_for(size):
    return max(1, (size + CLUSTER - 1) // CLUSTER)


def dirent(name11, attr, flags, cluster, size):
    # name, attr, NT flags, create-tenths, create time/date, access date,
    # cluster high, write time/date, cluster low, size.
    return struct.pack("<11sBBBHHHHHHHI", name11, attr, flags, 0,
                       FIXED_TIME, FIXED_DATE, FIXED_DATE, 0,
                       FIXED_TIME, FIXED_DATE, cluster, size)


class Node:
    def __init__(self, name, isdir, data=b"", children=None):
        self.name = name
        self.isdir = isdir
        self.data = data
        self.children = children if children is not None else []
        self.cluster = 0
        self.size = len(data)


def scan(path, name):
    names = sorted(os.listdir(path), key=lambda item: item.upper())
    children = []
    for child in names:
        full = os.path.join(path, child)
        entry_name(child)
        if os.path.islink(full):
            raise FatError("mkfat: symbolic link is not supported: " + full)
        if os.path.isdir(full):
            children.append(scan(full, child))
        elif os.path.isfile(full):
            with open(full, "rb") as handle:
                children.append(Node(child, False, handle.read()))
        else:
            raise FatError("mkfat: unsupported file type: " + full)
    if not children:
        raise FatError("mkfat: empty directory is not supported: " + path)
    return Node(name, True, b"", children)


class Image:
    def __init__(self, size_mb, label):
        text = label.upper()
        if not text or len(text) > 11:
            raise FatError("mkfat: volume label must be 1..11 characters")
        self.label = text.ljust(11)[:11].encode("ascii")
        self.total_sectors = size_mb * 1024 * 1024 // SECTOR
        spf = 1
        clusters = 0
        for _ in range(64):
            data = self.total_sectors - RESERVED - ROOT_SECTORS - NUM_FATS * spf
            if data <= 0:
                raise FatError("mkfat: image is too small for a file system")
            clusters = data // SPC
            need = ((clusters + 2) * 2 + SECTOR - 1) // SECTOR
            if need == spf:
                break
            spf = need
        else:
            raise FatError("mkfat: could not settle the FAT size")
        if clusters < FAT16_MIN or clusters > FAT16_MAX:
            raise FatError("mkfat: " + str(clusters) + " clusters is outside the FAT16 range " + str(FAT16_MIN) + ".." + str(FAT16_MAX) + "; use between 20 and 600 MiB")
        self.spf = spf
        self.clusters = clusters
        self.data_start = (RESERVED + NUM_FATS * spf + ROOT_SECTORS) * SECTOR
        self.root_start = (RESERVED + NUM_FATS * spf) * SECTOR
        self.image = bytearray(self.total_sectors * SECTOR)
        self.fat = [0] * (clusters + 2)
        self.fat[0] = 0xFFF8
        self.fat[1] = EOC
        self.next = 2

    def alloc(self, count):
        first = self.next
        for index in range(count):
            if self.next >= self.clusters + 2:
                raise FatError("mkfat: out of clusters")
            self.fat[self.next] = self.next + 1 if index + 1 < count else EOC
            self.next += 1
        return first

    def offset(self, cluster):
        return self.data_start + (cluster - 2) * CLUSTER

    def write(self, cluster, payload):
        start = self.offset(cluster)
        if start + len(payload) > len(self.image):
            raise FatError("mkfat: write past the end of the image")
        self.image[start:start + len(payload)] = payload

    def write_boot(self):
        bpb = bytearray(SECTOR)
        bpb[0:3] = b"\xeb\x3c\x90"
        bpb[3:11] = b"CRUGOS  "
        struct.pack_into("<H", bpb, 11, SECTOR)
        bpb[13] = SPC
        struct.pack_into("<H", bpb, 14, RESERVED)
        bpb[16] = NUM_FATS
        struct.pack_into("<H", bpb, 17, ROOT_ENTRIES)
        struct.pack_into("<H", bpb, 19, 0)
        bpb[21] = MEDIA
        struct.pack_into("<H", bpb, 22, self.spf)
        struct.pack_into("<H", bpb, 24, 63)
        struct.pack_into("<H", bpb, 26, 255)
        struct.pack_into("<I", bpb, 28, 0)
        struct.pack_into("<I", bpb, 32, self.total_sectors)
        bpb[36] = 0x80
        bpb[38] = 0x29
        struct.pack_into("<I", bpb, 39, 0x43524731)
        bpb[43:54] = self.label
        bpb[54:62] = b"FAT16   "
        bpb[510] = 0x55
        bpb[511] = 0xAA
        self.image[0:SECTOR] = bpb

    def write_fats(self):
        raw = bytearray(self.spf * SECTOR)
        for index, value in enumerate(self.fat):
            struct.pack_into("<H", raw, index * 2, value)
        for copy in range(NUM_FATS):
            start = (RESERVED + copy * self.spf) * SECTOR
            self.image[start:start + len(raw)] = raw


def alloc_tree(image, node):
    node.cluster = image.alloc(clusters_for(32 * (2 + len(node.children))))
    for child in node.children:
        if child.isdir:
            alloc_tree(image, child)
        else:
            child.cluster = image.alloc(clusters_for(child.size))


def dir_payload(node, parent_cluster):
    entries = bytearray()
    entries += dirent(b".          ", ATTR_DIR, 0, node.cluster, 0)
    entries += dirent(b"..         ", ATTR_DIR, 0, parent_cluster, 0)
    for child in node.children:
        name, flags = entry_name(child.name)
        if child.isdir:
            entries += dirent(name, ATTR_DIR, flags, child.cluster, 0)
        else:
            entries += dirent(name, ATTR_ARCHIVE, flags, child.cluster,
                              child.size)
    return bytes(entries)


def write_tree(image, node, parent_cluster):
    image.write(node.cluster, dir_payload(node, parent_cluster))
    for child in node.children:
        if child.isdir:
            write_tree(image, child, node.cluster)
        else:
            image.write(child.cluster, child.data)


def build(source, output, size_mb, label):
    root = scan(source, "/")
    image = Image(size_mb, label)
    for child in root.children:
        if child.isdir:
            alloc_tree(image, child)
        else:
            child.cluster = image.alloc(clusters_for(child.size))
    image.write_boot()
    image.write_fats()
    entries = bytearray()
    entries += dirent(image.label, ATTR_VOLUME, 0, 0, 0)
    for child in root.children:
        name, flags = entry_name(child.name)
        if child.isdir:
            entries += dirent(name, ATTR_DIR, flags, child.cluster, 0)
        else:
            entries += dirent(name, ATTR_ARCHIVE, flags, child.cluster,
                              child.size)
    if len(entries) > ROOT_SECTORS * SECTOR:
        raise FatError("mkfat: root directory overflow")
    image.image[image.root_start:image.root_start + len(entries)] = entries
    for child in root.children:
        if child.isdir:
            write_tree(image, child, 0)
        else:
            image.write(child.cluster, child.data)
    used = image.next - 2
    temporary = output + ".tmp"
    with open(temporary, "wb") as handle:
        handle.write(image.image)
    os.replace(temporary, output)
    print("  FAT   %s (%d MiB, %d clusters, %d used)" %
          (output, size_mb, image.clusters, used))


def parse(image):
    if len(image) < 3 * SECTOR or image[510] != 0x55 or image[511] != 0xAA:
        raise FatError("mkfat: not a FAT image (bad boot signature)")
    bytes_per_sector, = struct.unpack_from("<H", image, 11)
    spc = image[13]
    reserved, = struct.unpack_from("<H", image, 14)
    fats = image[16]
    root_entries, = struct.unpack_from("<H", image, 17)
    spf, = struct.unpack_from("<H", image, 22)
    total, = struct.unpack_from("<I", image, 32)
    if bytes_per_sector != SECTOR or spc == 0:
        raise FatError("mkfat: unexpected sector or cluster size")
    root_start = (reserved + fats * spf) * bytes_per_sector
    return {
        "cluster_size": bytes_per_sector * spc,
        "root_start": root_start,
        "root_entries": root_entries,
        "data_start": root_start + root_entries * 32,
        "fat_start": reserved * bytes_per_sector,
        "total": total,
    }


def entries_in(data):
    result = []
    for offset in range(0, len(data) - 31, 32):
        raw = bytes(data[offset:offset + 32])
        if raw[0] == 0x00:
            break
        if raw[0] == 0xE5:
            continue
        attr = raw[11]
        name = raw[0:8].decode("ascii", "replace").rstrip()
        ext = raw[8:11].decode("ascii", "replace").rstrip()
        flags = raw[12]
        if flags & 0x08:
            name = name.lower()
        if flags & 0x10:
            ext = ext.lower()
        cluster, = struct.unpack_from("<H", raw, 26)
        size, = struct.unpack_from("<I", raw, 28)
        result.append((name, ext, attr, cluster, size))
    return result


def chain_bytes(image, info, cluster):
    data = bytearray()
    seen = set()
    while 2 <= cluster < 0xFFF8 and cluster not in seen:
        seen.add(cluster)
        start = info["data_start"] + (cluster - 2) * info["cluster_size"]
        data += image[start:start + info["cluster_size"]]
        next_cluster, = struct.unpack_from("<H", image,
                                           info["fat_start"] + cluster * 2)
        cluster = next_cluster
    return bytes(data)


def walk(image):
    info = parse(image)
    out = []

    def visit(prefix, data):
        for name, ext, attr, cluster, size in entries_in(data):
            if attr & ATTR_VOLUME:
                continue
            if name in (".", ".."):
                continue
            full = prefix + name.upper() + ("." + ext.upper() if ext else "")
            if attr & ATTR_DIR:
                out.append((full, 0))
                visit(full + "/", chain_bytes(image, info, cluster))
            else:
                out.append((full, size))

    start = info["root_start"]
    visit("", image[start:start + info["root_entries"] * 32])
    return out


def find_file(image, path):
    """Return the bytes of one file inside an existing image."""
    info = parse(image)
    parts = [part for part in path.strip("/").upper().split("/") if part]
    if not parts:
        raise FatError("mkfat: empty path")
    data = image[info["root_start"]:info["root_start"] +
                 info["root_entries"] * 32]
    for index, part in enumerate(parts):
        found = None
        for name, ext, attr, cluster, size in entries_in(data):
            if attr & ATTR_VOLUME or name in (".", ".."):
                continue
            full = name.upper() + ("." + ext.upper() if ext else "")
            if full == part:
                found = (attr, cluster, size)
                break
        if found is None:
            raise FatError("mkfat: no such file: " + path)
        attr, cluster, size = found
        if index + 1 < len(parts):
            if not attr & ATTR_DIR:
                raise FatError("mkfat: not a directory: " + part)
            data = chain_bytes(image, info, cluster)
        else:
            if attr & ATTR_DIR:
                raise FatError("mkfat: is a directory: " + path)
            return chain_bytes(image, info, cluster)[:size]
    raise FatError("mkfat: no such file: " + path)


def main(argv):
    parser = argparse.ArgumentParser(add_help=True)
    parser.add_argument("--source", help="directory to copy into the image")
    parser.add_argument("--output", help="image file to write")
    parser.add_argument("--size-mb", type=int, default=32)
    parser.add_argument("--label", default="CRUGOS")
    parser.add_argument("--list", help="list the files in an existing image")
    parser.add_argument("--read", help="print one file of an existing image")
    parser.add_argument("--image", help="existing image for --list/--read")
    args = parser.parse_args(argv)
    try:
        source = args.image or args.list
        if args.list or args.read:
            if not source:
                print("mkfat: --list needs an image", file=sys.stderr)
                return 2
            with open(source, "rb") as handle:
                image = bytearray(handle.read())
            if args.read:
                sys.stdout.buffer.write(find_file(image, args.read))
                return 0
            for path, size in walk(image):
                if size:
                    print("%s\t%d" % (path, size))
                else:
                    print("%s\tDIR" % path)
            return 0
        if not args.source or not args.output:
            print("mkfat: --source and --output are required",
                  file=sys.stderr)
            return 2
        build(args.source, args.output, args.size_mb, args.label)
    except FatError as error:
        print(error, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
