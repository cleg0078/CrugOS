#!/usr/bin/env python3
"""Flip one byte in an image to simulate media damage.

usage: tamper.py IMAGE OFFSET [MASK]
"""

import sys


def main():
    if len(sys.argv) not in (3, 4):
        print("usage: tamper.py IMAGE OFFSET [MASK]", file=sys.stderr)
        return 2
    path = sys.argv[1]
    try:
        offset = int(sys.argv[2], 0)
        mask = int(sys.argv[3], 0) if len(sys.argv) == 4 else 0xFF
    except ValueError:
        print("tamper: offset and mask must be integers", file=sys.stderr)
        return 2
    if offset < 0 or not 0 <= mask <= 0xFF:
        print("tamper: offset must be positive and mask one byte",
              file=sys.stderr)
        return 2
    try:
        with open(path, "r+b") as image:
            image.seek(offset)
            before = image.read(1)
            if not before:
                print("tamper: offset %d is past the end of %s" % (offset, path),
                      file=sys.stderr)
                return 1
            image.seek(offset)
            image.write(bytes([before[0] ^ mask]))
    except OSError as exc:
        print("tamper: %s" % exc, file=sys.stderr)
        return 1
    print("tamper: flipped byte %d of %s (0x%02x xor 0x%02x)" %
          (offset, path, before[0], mask))
    return 0


if __name__ == "__main__":
    sys.exit(main())
