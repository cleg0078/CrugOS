#!/usr/bin/env python3
"""Verify a CrugOS boot: serial markers, guest version and exit code.

usage: boot-check.py SERIAL_LOG EXIT_CODE_FILE [VERSION_FILE]
"""

import re
import sys
from pathlib import Path

REQUIRED_MARKERS = (
    "CRUGOS: hello",
    "CRUGOS: phase 0 boot ok",
)
VERSION_PATTERN = re.compile(r"CRUGOS: version (\d+\.\d+\.\d+(?:[-+][0-9A-Za-z.+-]+)?)")
SUCCESS_EXIT = 33  # isa-debug-exit: port 0xf4 value 0x10 -> (0x10 << 1) | 1


def fail(message: str, log: str) -> int:
    print("FAIL: " + message)
    tail = log.splitlines()[-40:]
    if tail:
        print("--- serial tail ---")
        print(chr(10).join(tail))
    return 1


def main() -> int:
    if len(sys.argv) not in (3, 4):
        print("usage: boot-check.py SERIAL_LOG EXIT_CODE_FILE [VERSION_FILE]", file=sys.stderr)
        return 2

    serial_path = Path(sys.argv[1])
    exit_path = Path(sys.argv[2])
    version_path = Path(sys.argv[3]) if len(sys.argv) == 4 else None

    if not serial_path.exists():
        return fail(f"missing serial log {serial_path}", "")
    if not exit_path.exists():
        return fail(f"missing exit code file {exit_path}", "")

    log = serial_path.read_text(errors="replace")
    text = re.sub(r"\r\n?", "\n", log)

    if "CRUGOS PANIC" in text or "CRUGOS loader: FAIL" in text:
        return fail("panic or loader failure detected", text)

    for marker in REQUIRED_MARKERS:
        if marker not in text:
            return fail(f"missing serial marker: {marker!r}", text)

    match = VERSION_PATTERN.search(text)
    if not match:
        return fail("missing serial marker: 'CRUGOS: version <semver>'", text)
    guest_version = match.group(1)

    if version_path is not None:
        if not version_path.exists():
            return fail(f"missing version file {version_path}", text)
        expected = version_path.read_text().strip()
        if guest_version != expected:
            return fail(f"guest version {guest_version!r} != VERSION {expected!r}", text)

    raw_exit = exit_path.read_text().strip()
    try:
        status = int(raw_exit)
    except ValueError:
        return fail(f"invalid exit code {raw_exit!r}", text)

    if status != SUCCESS_EXIT:
        return fail(f"guest did not exit cleanly: QEMU status {status}", text)

    boot_report = [line for line in text.splitlines() if line.startswith("boot:")]
    print("ok: boot markers found, version", guest_version,
          "QEMU exit status", status)
    for line in boot_report:
        print("  " + line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
