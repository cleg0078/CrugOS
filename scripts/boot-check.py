#!/usr/bin/env python3
"""Verify a CrugOS boot: required serial markers and a successful exit code."""

import re
import sys
from pathlib import Path

REQUIRED_MARKERS = (
    "CRUGOS: hello",
    "CRUGOS: phase 0 boot ok",
)
SUCCESS_EXIT = 33  # isa-debug-exit: port 0xf4 value 0x10 -> (0x10 << 1) | 1


def fail(message: str, log: str) -> int:
    print("FAIL: " + message)
    tail = log.splitlines()[-40:]
    if tail:
        print("--- serial tail ---")
        print(chr(10).join(tail))
    return 1


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: boot-check.py SERIAL_LOG EXIT_CODE_FILE", file=sys.stderr)
        return 2

    serial_path = Path(sys.argv[1])
    exit_path = Path(sys.argv[2])

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

    raw_exit = exit_path.read_text().strip()
    try:
        status = int(raw_exit)
    except ValueError:
        return fail(f"invalid exit code {raw_exit!r}", text)

    if status != SUCCESS_EXIT:
        return fail(f"guest did not exit cleanly: QEMU status {status}", text)

    boot_report = [line for line in text.splitlines() if line.startswith("boot:")]
    print("ok: boot markers found, QEMU exit status", status)
    for line in boot_report:
        print("  " + line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
