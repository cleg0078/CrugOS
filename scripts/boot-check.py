#!/usr/bin/env python3
"""Verify a CrugOS boot: serial markers, guest version, CPU policy, storage mode, encryption and exit code.

usage: boot-check.py SERIAL_LOG EXIT_CODE_FILE \
    [VERSION_FILE [EXPECT_AVX2 [EXPECT_RAID [EXPECT_NVME [EXPECT_REBUILD \
    [EXPECT_AHCI [EXPECT_CRYPT [EXPECT_AES]]]]]]]]
"""

import re
import sys
from pathlib import Path

REQUIRED_MARKERS = (
    "CRUGOS: hello",
    "CRUGOS: pmm ready",
    "CRUGOS: vm ready",
    "CRUGOS: cpu ready",
    "CRUGOS: timer ready",
    "CRUGOS: heap ready",
    "CRUGOS: sched ready",
    "CRUGOS: syscall ready",
    "CRUGOS: block ready",
    "CRUGOS: gpt ready",
    "CRUGOS: ext4 ready",
    "CRUGOS: storage ready",
    "CRUGOS: journal ready",
    "CRUGOS: writable storage ok",
    "CRUGOS: journal selftest ok",
    "CRUGOS: selftest ok",
    "CRUGOS: nvme ready",
    "CRUGOS: ahci ready",
    "CRUGOS: phase 3b9 boot ok",
)
# A rejected unlock stops before scheduling, storage mounting and self-tests.
WRONG_CRYPT_MARKERS = (
    "CRUGOS: hello",
    "CRUGOS: pmm ready",
    "CRUGOS: vm ready",
    "CRUGOS: cpu ready",
    "CRUGOS: timer ready",
    "CRUGOS: heap ready",
    "CRUGOS: block ready",
    "CRUGOS: gpt ready",
    "CRUGOS: crypt unlock failed",
    "CRUGOS: no root filesystem",
)
VERSION_PATTERN = re.compile(r"CRUGOS: version (\d+\.\d+\.\d+(?:[-+][0-9A-Za-z.+-]+)?)")
SUCCESS_EXIT = 33  # isa-debug-exit: port 0xf4 value 0x10 -> (0x10 << 1) | 1
ENCRYPT_FAIL_EXIT = 35  # arch_exit_qemu(0x11): no root filesystem


def fail(message: str, log: str) -> int:
    print("FAIL: " + message)
    tail = log.splitlines()[-40:]
    if tail:
        print("--- serial tail ---")
        print(chr(10).join(tail))
    return 1


def main() -> int:
    if len(sys.argv) not in tuple(range(3, 12)):
        print("usage: boot-check.py SERIAL_LOG EXIT_CODE_FILE "
              "[VERSION_FILE [EXPECT_AVX2 [EXPECT_RAID [EXPECT_NVME "
              "[EXPECT_REBUILD [EXPECT_AHCI [EXPECT_CRYPT [EXPECT_AES]]]]]]]]",
              file=sys.stderr)
        return 2

    serial_path = Path(sys.argv[1])
    exit_path = Path(sys.argv[2])
    version_path = Path(sys.argv[3]) if len(sys.argv) >= 4 else None
    expect_avx2 = sys.argv[4] if len(sys.argv) >= 5 else "yes"
    expect_raid = sys.argv[5] if len(sys.argv) >= 6 else "no"
    expect_nvme = sys.argv[6] if len(sys.argv) >= 7 else "no"
    expect_rebuild = sys.argv[7] if len(sys.argv) >= 8 else "no"
    expect_ahci = sys.argv[8] if len(sys.argv) >= 9 else "no"
    expect_crypt = sys.argv[9] if len(sys.argv) >= 10 else "no"
    expect_aes = sys.argv[10] if len(sys.argv) >= 11 else "any"

    if not serial_path.exists():
        return fail(f"missing serial log {serial_path}", "")
    if not exit_path.exists():
        return fail(f"missing exit code file {exit_path}", "")

    log = serial_path.read_text(errors="replace")
    text = re.sub(r"\r\n?", "\n", log)

    if "CRUGOS PANIC" in text or "CRUGOS loader: FAIL" in text:
        return fail("panic or loader failure detected", text)

    if expect_crypt not in ("no", "yes", "wrong"):
        return fail(f"invalid EXPECT_CRYPT {expect_crypt!r}", text)
    if expect_aes not in ("any", "software", "aes-ni"):
        return fail(f"invalid EXPECT_AES {expect_aes!r}", text)

    required = list(REQUIRED_MARKERS)
    if expect_crypt == "wrong":
        # The boot is expected to stop before a root filesystem exists.
        required = list(WRONG_CRYPT_MARKERS)
    elif expect_crypt == "yes":
        required.append("CRUGOS: crypt ready")
    for marker in required:
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

    if expect_avx2 == "yes" and "avx2=yes" not in text:
        return fail("AVX2 was required but the guest did not report avx2=yes", text)
    if expect_avx2 == "no" and "avx2=no" not in text:
        return fail("compatibility build expected avx2=no", text)

    if expect_raid not in ("no", "healthy", "degraded"):
        return fail(f"invalid EXPECT_RAID {expect_raid!r}", text)
    if expect_raid != "no":
        if "CRUGOS: raid ready" not in text:
            return fail("expected a RAID root but the guest did not assemble one", text)
        marker = "CRUGOS: raid " + expect_raid
        if marker not in text:
            return fail(f"missing serial marker: {marker!r}", text)

    if expect_nvme not in ("no", "yes"):
        return fail(f"invalid EXPECT_NVME {expect_nvme!r}", text)
    if expect_nvme == "yes":
        if "nvme0n1" not in text:
            return fail("expected an NVMe namespace but the guest did not report nvme0n1", text)
        match = re.search(r"CRUGOS: nvme ready \((\d+) device", text)
        if not match or int(match.group(1)) < 1:
            return fail("expected at least one NVMe block device", text)

    if expect_ahci not in ("no", "yes"):
        return fail(f"invalid EXPECT_AHCI {expect_ahci!r}", text)
    if expect_ahci == "yes":
        if not re.search(r"CRUGOS: root device sd[a-z]", text):
            return fail("expected an AHCI root but the guest mounted a different device", text)
        if not re.search(r"ahci\s*: sd[a-z] \d+ sectors", text):
            return fail("expected at least one AHCI block device", text)

    if expect_crypt == "yes":
        match = re.search(r"CRUGOS: crypt aes: (\S+)", text)
        if not match:
            return fail("expected the crypt device to report its AES implementation", text)
        if expect_aes != "any" and match.group(1) != expect_aes:
            return fail(f"crypt AES implementation {match.group(1)!r} != EXPECT_AES {expect_aes!r}",
                        text)

    if expect_rebuild == "no" and "CRUGOS: raid rebuilt" in text:
        return fail("unexpected RAID member rebuild during boot", text)
    if expect_rebuild != "no":
        try:
            rebuilt = int(expect_rebuild)
        except ValueError:
            return fail(f"invalid EXPECT_REBUILD {expect_rebuild!r}", text)
        if rebuilt < 1:
            return fail(f"invalid EXPECT_REBUILD {expect_rebuild!r}", text)
        marker = f"CRUGOS: raid rebuilt ({rebuilt} member(s))"
        if marker not in text:
            return fail(f"missing serial marker: {marker!r}", text)

    raw_exit = exit_path.read_text().strip()
    try:
        status = int(raw_exit)
    except ValueError:
        return fail(f"invalid exit code {raw_exit!r}", text)

    expected_exit = ENCRYPT_FAIL_EXIT if expect_crypt == "wrong" else SUCCESS_EXIT
    if status != expected_exit:
        return fail(f"guest exit status {status} != expected {expected_exit}", text)

    boot_report = [line for line in text.splitlines()
                   if line.lstrip().startswith("boot") and ":" in line]
    print("ok: boot markers found, version", guest_version,
          "avx2", expect_avx2, "raid", expect_raid,
          "nvme", expect_nvme, "rebuild", expect_rebuild,
          "ahci", expect_ahci, "crypt", expect_crypt, "aes", expect_aes,
          "QEMU exit status", status)
    for line in boot_report:
        print("  " + line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
