#!/usr/bin/env python3
"""Verify a CrugOS boot: serial markers, guest version, CPU policy, storage mode, encryption, networking, TCP, DNS, USB, HID mice, audio and the framebuffer cursor, then the exit code.

usage: boot-check.py SERIAL_LOG EXIT_CODE_FILE \
    [VERSION_FILE [EXPECT_AVX2 [EXPECT_RAID [EXPECT_NVME [EXPECT_REBUILD \
    [EXPECT_AHCI [EXPECT_CRYPT [EXPECT_AES [EXPECT_NET [EXPECT_TCP \
    [EXPECT_DNS [EXPECT_USB [EXPECT_MOUSE [EXPECT_AUDIO \
    [EXPECT_GFX]]]]]]]]]]]]]]]
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
    "CRUGOS: input ready",
    "CRUGOS: net ready",
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
    "CRUGOS: phase 5d boot ok",
)
# A rejected unlock stops before scheduling, storage mounting and self-tests.
WRONG_CRYPT_MARKERS = (
    "CRUGOS: hello",
    "CRUGOS: pmm ready",
    "CRUGOS: vm ready",
    "CRUGOS: cpu ready",
    "CRUGOS: timer ready",
    "CRUGOS: heap ready",
    "CRUGOS: input ready",
    "CRUGOS: net ready",
    "CRUGOS: block ready",
    "CRUGOS: gpt ready",
    "CRUGOS: crypt unlock failed",
    "CRUGOS: no root filesystem",
)
# An interactive prompt that got no usable passphrase stops the same way.
LOCKED_CRYPT_MARKERS = (
    "CRUGOS: hello",
    "CRUGOS: pmm ready",
    "CRUGOS: vm ready",
    "CRUGOS: cpu ready",
    "CRUGOS: timer ready",
    "CRUGOS: heap ready",
    "CRUGOS: input ready",
    "CRUGOS: net ready",
    "CRUGOS: block ready",
    "CRUGOS: gpt ready",
    "CRUGOS: crypt locked (no passphrase)",
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
    if len(sys.argv) not in tuple(range(3, 19)):
        print("usage: boot-check.py SERIAL_LOG EXIT_CODE_FILE "
              "[VERSION_FILE [EXPECT_AVX2 [EXPECT_RAID [EXPECT_NVME "
              "[EXPECT_REBUILD [EXPECT_AHCI [EXPECT_CRYPT [EXPECT_AES "
              "[EXPECT_NET [EXPECT_TCP [EXPECT_DNS [EXPECT_USB "
              "[EXPECT_MOUSE [EXPECT_AUDIO [EXPECT_GFX]]]]]]]]]]]]]]]",
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
    expect_net = sys.argv[11] if len(sys.argv) >= 12 else "no"
    expect_tcp = sys.argv[12] if len(sys.argv) >= 13 else "no"
    expect_dns = sys.argv[13] if len(sys.argv) >= 14 else "no"
    expect_usb = sys.argv[14] if len(sys.argv) >= 15 else "no"
    expect_mouse = sys.argv[15] if len(sys.argv) >= 16 else "no"
    expect_audio = sys.argv[16] if len(sys.argv) >= 17 else "no"
    expect_gfx = sys.argv[17] if len(sys.argv) >= 18 else "no"

    if not serial_path.exists():
        return fail(f"missing serial log {serial_path}", "")
    if not exit_path.exists():
        return fail(f"missing exit code file {exit_path}", "")

    log = serial_path.read_text(errors="replace")
    text = re.sub(r"\r\n?", "\n", log)

    if "CRUGOS PANIC" in text or "CRUGOS loader: FAIL" in text:
        return fail("panic or loader failure detected", text)

    if expect_crypt not in ("no", "yes", "wrong", "locked"):
        return fail(f"invalid EXPECT_CRYPT {expect_crypt!r}", text)
    if expect_aes not in ("any", "software", "aes-ni"):
        return fail(f"invalid EXPECT_AES {expect_aes!r}", text)
    if expect_net not in ("no", "yes"):
        return fail(f"invalid EXPECT_NET {expect_net!r}", text)
    if expect_tcp not in ("no", "yes"):
        return fail(f"invalid EXPECT_TCP {expect_tcp!r}", text)
    if expect_dns not in ("no", "yes"):
        return fail(f"invalid EXPECT_DNS {expect_dns!r}", text)
    if expect_usb not in ("no", "yes"):
        return fail(f"invalid EXPECT_USB {expect_usb!r}", text)
    if expect_mouse not in ("no", "yes"):
        return fail(f"invalid EXPECT_MOUSE {expect_mouse!r}", text)
    if expect_audio not in ("no", "yes"):
        return fail(f"invalid EXPECT_AUDIO {expect_audio!r}", text)
    if expect_gfx not in ("no", "yes"):
        return fail(f"invalid EXPECT_GFX {expect_gfx!r}", text)

    required = list(REQUIRED_MARKERS)
    if expect_crypt == "wrong":
        # The boot is expected to stop before a root filesystem exists.
        required = list(WRONG_CRYPT_MARKERS)
    elif expect_crypt == "locked":
        # A timed-out or cancelled prompt fails closed before mounting.
        required = list(LOCKED_CRYPT_MARKERS)
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

    if expect_net == "yes":
        if "CRUGOS: net selftest ok" not in text:
            return fail("expected the network selftest to succeed", text)
        if not re.search(r"CRUGOS: dhcp lease \d+\.\d+\.\d+\.\d+/\d+\.\d+\.\d+\.\d+ gw ", text):
            return fail("expected a DHCP lease line from the guest", text)
        if not re.search(r"CRUGOS: arp resolved \d+\.\d+\.\d+\.\d+", text):
            return fail("expected the gateway ARP resolution to succeed", text)
        if not re.search(r"CRUGOS: icmp echo reply from \d+\.\d+\.\d+\.\d+ seq=1 ttl=\d+", text):
            return fail("expected an ICMP echo reply from the gateway", text)

    if expect_tcp == "yes":
        for marker in (
            "CRUGOS: tcp listen ready (port 8080)",
            "CRUGOS: tcp connect ok",
            "CRUGOS: tcp http ok",
            "CRUGOS: tcp socket syscall ok",
            "CRUGOS: tcp http served",
        ):
            if marker not in text:
                return fail(f"missing serial marker: {marker!r}", text)
        if not re.search(r"CRUGOS: tcp http ok \(\d+ bytes\)", text):
            return fail("expected the HTTP client leg to report a byte count", text)
        if not re.search(r"CRUGOS: tcp http served \(\d+ bytes\)", text):
            return fail("expected the HTTP server leg to report a byte count", text)

    if expect_dns == "yes":
        for marker in (
            "CRUGOS: dns resolved",
            "CRUGOS: dns cache hit",
            "CRUGOS: dns socket syscall ok",
            "CRUGOS: dns selftest ok",
            "CRUGOS: net selftest ok",
        ):
            if marker not in text:
                return fail(f"missing serial marker: {marker!r}", text)
        if not re.search(r"dns resolved \S+ -> \d+\.\d+\.\d+\.\d+ via (udp|tcp|cache)",
                         text):
            return fail("expected the resolver to report a resolved name", text)

    if expect_usb == "yes":
        for marker in (
            "CRUGOS: input ready (serial only)",
            "CRUGOS: usb controller ready (xhci",
            "CRUGOS: usb keyboard ready",
            "CRUGOS: usb selftest ok",
        ):
            if marker not in text:
                return fail(f"missing serial marker: {marker!r}", text)
        keys = re.findall(r"CRUGOS: usb key '(.)'", text)
        if not keys:
            return fail("expected the USB keyboard to decode at least one key", text)
        if not re.search(r"CRUGOS: usb keyboard ready \(endpoint 0x[0-9a-f]{2}, "
                         r"\d+ byte reports\)", text):
            return fail("expected the boot keyboard endpoint to be reported", text)

    if expect_mouse == "yes":
        for marker in (
            "CRUGOS: input ready (serial only)",
            "CRUGOS: usb controller ready (xhci",
            "CRUGOS: usb selftest ok",
        ):
            if marker not in text:
                return fail(f"missing serial marker: {marker!r}", text)
        if not re.search(r"CRUGOS: usb mouse ready \(endpoint 0x[0-9a-f]{2}, "
                         r"\d+ byte reports\)", text):
            return fail("expected the boot mouse endpoint to be reported", text)
        if not re.search(r"CRUGOS: usb mouse move dx=-?\d+ dy=-?\d+", text):
            return fail("expected the USB mouse to report pointer motion", text)
        if not re.search(r"CRUGOS: usb mouse button 0x0[1-7]", text):
            return fail("expected the USB mouse to report a button", text)

    if expect_audio == "yes":
        for marker in (
            "CRUGOS: usb audio attached (uac",
            "CRUGOS: audio device ready (usb",
            "CRUGOS: audio stream started",
            "CRUGOS: audio selftest ok",
        ):
            if marker not in text:
                return fail(f"missing serial marker: {marker!r}", text)
        if not re.search(r"CRUGOS: audio streamed \d+ frame\(s\) in \d+ ms", text):
            return fail("expected the audio selftest to report streamed frames", text)
        if ("CRUGOS: audio captured" not in text and
                "CRUGOS: audio capture unavailable" not in text):
            return fail("expected the audio selftest to report its capture result", text)

    if expect_gfx == "yes":
        for marker in (
            "CRUGOS: gfx ready (",
            "CRUGOS: gfx cursor shown at",
            "CRUGOS: gfx selftest ok",
        ):
            if marker not in text:
                return fail(f"missing serial marker: {marker!r}", text)
        positions = re.findall(r"CRUGOS: gfx cursor at \(-?\d+, -?\d+\)", text)
        if len(positions) < 2:
            return fail("expected the cursor to move at least once", text)

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

    expected_exit = (ENCRYPT_FAIL_EXIT if expect_crypt in ("wrong", "locked")
                     else SUCCESS_EXIT)
    if status != expected_exit:
        return fail(f"guest exit status {status} != expected {expected_exit}", text)

    boot_report = [line for line in text.splitlines()
                   if line.lstrip().startswith("boot") and ":" in line]
    print("ok: boot markers found, version", guest_version,
          "avx2", expect_avx2, "raid", expect_raid,
          "nvme", expect_nvme, "rebuild", expect_rebuild,
          "ahci", expect_ahci, "crypt", expect_crypt, "aes", expect_aes,
          "net", expect_net, "tcp", expect_tcp, "dns", expect_dns,
          "usb", expect_usb, "mouse", expect_mouse, "audio", expect_audio,
          "gfx", expect_gfx, "QEMU exit status", status)
    for line in boot_report:
        print("  " + line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
