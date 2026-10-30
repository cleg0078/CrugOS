#!/usr/bin/env python3
"""Verify a CrugOS boot: serial markers, guest version, CPU policy, storage mode, encryption, networking, TCP, DNS, USB, HID mice, audio, the framebuffer cursor, the Workbench desktop, its menus and gadgets, its shell window, its keyboard
navigation, its desktop objects, its scrollable drawer, the virtio-gpu 2D
scanout, the ring-3 userspace
selftest, the ring-3 shell that reads a script and writes a file, the ring-3
spawn/exec/wait path and the ring-3 fork path, then the exit code.

Pass --storage=none anywhere on the command line when the boot medium ships no
block device (the hybrid ISO does): the storage, journal, GPT and ext4 markers
are then replaced by the single 'CRUGOS: storage skipped' marker.

usage: boot-check.py SERIAL_LOG EXIT_CODE_FILE \
    [VERSION_FILE [EXPECT_AVX2 [EXPECT_RAID [EXPECT_NVME [EXPECT_REBUILD \
    [EXPECT_AHCI [EXPECT_CRYPT [EXPECT_AES [EXPECT_NET [EXPECT_TCP \
    [EXPECT_DNS [EXPECT_USB [EXPECT_MOUSE [EXPECT_AUDIO \
    [EXPECT_GFX [EXPECT_WB [EXPECT_MENU [EXPECT_SHELL [EXPECT_KEYS [EXPECT_DESK \
    [EXPECT_ICONS [EXPECT_DRAWER [EXPECT_SELECT [EXPECT_HEAP \
    [EXPECT_USER [EXPECT_USERS [EXPECT_USHELL [EXPECT_SPAWN [EXPECT_FORK]]]]]]]]]]]]]]]]]]]]]]]]]] \
    [--storage=none]
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
    "CRUGOS: phase 8b boot ok",
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
# A boot medium without a block device (the hybrid ISO) skips the storage stack.
STORAGE_MARKERS = (
    "CRUGOS: block ready",
    "CRUGOS: gpt ready",
    "CRUGOS: ext4 ready",
    "CRUGOS: storage ready",
    "CRUGOS: journal ready",
    "CRUGOS: writable storage ok",
    "CRUGOS: journal selftest ok",
)
STORAGE_SKIPPED_MARKER = "CRUGOS: storage skipped (storage=none)"
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
    # The storage flag is a switch, not a positional argument, so strip it first
    # and keep every existing positional call site unchanged.
    storage_mode = "disk"
    screen_mode = "no"
    cx_mode = "no"
    app_mode = "no"
    pkg_mode = "no"
    caps_mode = "no"
    wbpkg_mode = "no"
    gpu_mode = "no"
    gpu_edid_mode = "no"
    gpu_cursor_mode = "no"
    argv = []
    for arg in sys.argv[1:]:
        if arg == "--storage=none":
            storage_mode = "none"
        elif arg.startswith("--storage="):
            print(f"FAIL: invalid storage mode {arg!r}", file=sys.stderr)
            return 2
        elif arg == "--expect-screen=yes":
            screen_mode = "yes"
        elif arg.startswith("--expect-screen="):
            print(f"FAIL: invalid screen expectation {arg!r}", file=sys.stderr)
            return 2
        elif arg == "--expect-cx=yes":
            cx_mode = "yes"
        elif arg.startswith("--expect-cx="):
            print(f"FAIL: invalid exchange expectation {arg!r}", file=sys.stderr)
            return 2
        elif arg == "--expect-app=yes":
            app_mode = "yes"
        elif arg.startswith("--expect-app="):
            print(f"FAIL: invalid app expectation {arg!r}", file=sys.stderr)
            return 2
        elif arg == "--expect-pkg=yes":
            pkg_mode = "yes"
        elif arg.startswith("--expect-pkg="):
            print(f"FAIL: invalid package expectation {arg!r}", file=sys.stderr)
            return 2
        elif arg == "--expect-caps=yes":
            caps_mode = "yes"
        elif arg.startswith("--expect-caps="):
            print(f"FAIL: invalid caps expectation {arg!r}", file=sys.stderr)
            return 2
        elif arg == "--expect-wbpkg=yes":
            wbpkg_mode = "yes"
        elif arg.startswith("--expect-wbpkg="):
            print(f"FAIL: invalid packages-window expectation {arg!r}", file=sys.stderr)
            return 2
        elif arg == "--expect-gpu=yes":
            gpu_mode = "yes"
        elif arg.startswith("--expect-gpu="):
            print(f"FAIL: invalid gpu expectation {arg!r}", file=sys.stderr)
            return 2
        elif arg == "--expect-gpu-edid=yes":
            gpu_edid_mode = "yes"
        elif arg.startswith("--expect-gpu-edid="):
            print(f"FAIL: invalid gpu EDID expectation {arg!r}", file=sys.stderr)
            return 2
        elif arg == "--expect-gpu-cursor=yes":
            gpu_cursor_mode = "yes"
        elif arg.startswith("--expect-gpu-cursor="):
            print(f"FAIL: invalid gpu cursor expectation {arg!r}", file=sys.stderr)
            return 2
        else:
            argv.append(arg)
    sys.argv = [sys.argv[0]] + argv

    if len(sys.argv) < 3 or len(sys.argv) > 33:
        print("usage: boot-check.py SERIAL_LOG EXIT_CODE_FILE "
              "[VERSION_FILE [EXPECT_AVX2 [EXPECT_RAID [EXPECT_NVME "
              "[EXPECT_REBUILD [EXPECT_AHCI [EXPECT_CRYPT [EXPECT_AES "
              "[EXPECT_NET [EXPECT_TCP [EXPECT_DNS [EXPECT_USB "
              "[EXPECT_MOUSE [EXPECT_AUDIO [EXPECT_GFX [EXPECT_WB "
              "[EXPECT_MENU [EXPECT_SHELL [EXPECT_KEYS [EXPECT_DESK "
              "[EXPECT_ICONS [EXPECT_DRAWER [EXPECT_SELECT [EXPECT_HEAP "
              "[EXPECT_USER [EXPECT_USERS [EXPECT_USHELL [EXPECT_SPAWN [EXPECT_FORK "
              "[EXPECT_FORKBIG]]]]]]]]]]]]]]]]]]]]]]]]]] "
              "[--storage=none] [--expect-screen=yes] [--expect-cx=yes] "
              "[--expect-app=yes] [--expect-pkg=yes] [--expect-caps=yes] "
              "[--expect-wbpkg=yes] [--expect-gpu=yes] "
              "[--expect-gpu-edid=yes] [--expect-gpu-cursor=yes]",
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
    expect_wb = sys.argv[18] if len(sys.argv) >= 19 else "no"
    expect_menu = sys.argv[19] if len(sys.argv) >= 20 else "no"
    expect_shell = sys.argv[20] if len(sys.argv) >= 21 else "no"
    expect_keys = sys.argv[21] if len(sys.argv) >= 22 else "no"
    expect_desk = sys.argv[22] if len(sys.argv) >= 23 else "no"
    expect_icons = sys.argv[23] if len(sys.argv) >= 24 else "no"
    expect_drawer = sys.argv[24] if len(sys.argv) >= 25 else "no"
    expect_select = sys.argv[25] if len(sys.argv) >= 26 else "no"
    expect_heap = sys.argv[26] if len(sys.argv) >= 27 else "no"
    expect_user = sys.argv[27] if len(sys.argv) >= 28 else "no"
    expect_users = sys.argv[28] if len(sys.argv) >= 29 else "no"
    expect_ushell = sys.argv[29] if len(sys.argv) >= 30 else "no"
    expect_spawn = sys.argv[30] if len(sys.argv) >= 31 else "no"
    expect_fork = sys.argv[31] if len(sys.argv) >= 32 else "no"
    expect_forkbig = sys.argv[32] if len(sys.argv) >= 33 else "no"

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
    if expect_wb not in ("no", "yes"):
        return fail(f"invalid EXPECT_WB {expect_wb!r}", text)
    if expect_menu not in ("no", "yes"):
        return fail(f"invalid EXPECT_MENU {expect_menu!r}", text)
    if expect_shell not in ("no", "yes"):
        return fail(f"invalid EXPECT_SHELL {expect_shell!r}", text)
    if expect_keys not in ("no", "yes"):
        return fail(f"invalid EXPECT_KEYS {expect_keys!r}", text)
    if expect_desk not in ("no", "yes"):
        return fail(f"invalid EXPECT_DESK {expect_desk!r}", text)
    if expect_icons not in ("no", "yes"):
        return fail(f"invalid EXPECT_ICONS {expect_icons!r}", text)
    if expect_drawer not in ("no", "yes"):
        return fail(f"invalid EXPECT_DRAWER {expect_drawer!r}", text)
    if expect_select not in ("no", "yes"):
        return fail(f"invalid EXPECT_SELECT {expect_select!r}", text)
    if expect_heap not in ("no", "yes"):
        return fail(f"invalid EXPECT_HEAP {expect_heap!r}", text)
    if expect_user not in ("no", "yes"):
        return fail(f"invalid EXPECT_USER {expect_user!r}", text)
    if expect_users not in ("no", "yes"):
        return fail(f"invalid EXPECT_USERS {expect_users!r}", text)
    if expect_ushell not in ("no", "yes"):
        return fail(f"invalid EXPECT_USHELL {expect_ushell!r}", text)
    if expect_spawn not in ("no", "yes"):
        return fail(f"invalid EXPECT_SPAWN {expect_spawn!r}", text)
    if expect_fork not in ("no", "yes"):
        return fail(f"invalid EXPECT_FORK {expect_fork!r}", text)
    if screen_mode not in ("no", "yes"):
        return fail(f"invalid screen mode {screen_mode!r}", text)
    if cx_mode not in ("no", "yes"):
        return fail(f"invalid exchange mode {cx_mode!r}", text)
    if app_mode not in ("no", "yes"):
        return fail(f"invalid app mode {app_mode!r}", text)
    if pkg_mode not in ("no", "yes"):
        return fail(f"invalid package mode {pkg_mode!r}", text)
    if caps_mode not in ("no", "yes"):
        return fail(f"invalid caps mode {caps_mode!r}", text)
    if wbpkg_mode not in ("no", "yes"):
        return fail(f"invalid packages-window mode {wbpkg_mode!r}", text)
    if gpu_mode not in ("no", "yes"):
        return fail(f"invalid gpu mode {gpu_mode!r}", text)
    if gpu_edid_mode not in ("no", "yes"):
        return fail(f"invalid gpu EDID mode {gpu_edid_mode!r}", text)
    if gpu_cursor_mode not in ("no", "yes"):
        return fail(f"invalid gpu cursor mode {gpu_cursor_mode!r}", text)
    if gpu_edid_mode == "yes" and gpu_mode != "yes":
        return fail("--expect-gpu-edid=yes also needs --expect-gpu=yes", text)
    if gpu_cursor_mode == "yes" and gpu_mode != "yes":
        return fail("--expect-gpu-cursor=yes also needs --expect-gpu=yes", text)

    required = list(REQUIRED_MARKERS)
    if storage_mode == "none":
        # No block device: the storage stack reports the skip instead.
        required = [marker for marker in required if marker not in STORAGE_MARKERS]
        required.append(STORAGE_SKIPPED_MARKER)
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

    if gpu_mode == "yes":
        # The gate boots without a VGA adapter, so the only display is the
        # driver's own resource: every marker below proves the 2D scanout.
        for marker in (
            "CRUGOS: virtio-gpu ready (",
            "CRUGOS: gpu scanout ok",
            "CRUGOS: wb desktop ready",
            "CRUGOS: gpu selftest ok",
            "CRUGOS: wb selftest ok",
        ):
            if marker not in text:
                return fail(f"missing serial marker: {marker!r}", text)
        ready = re.search(
            r"CRUGOS: virtio-gpu ready \((\d+)x(\d+), (\d+) backing page\(s\), "
            r"resource \d+, modern pci\)", text)
        if ready is None:
            return fail("expected the virtio-gpu resource to be created", text)
        if int(ready.group(3)) < 1:
            return fail("expected at least one backing page", text)
        scanout = re.search(
            r"CRUGOS: gpu scanout ok \((\d+)x(\d+), (\d+) transfer\(s\), "
            r"(\d+) flush\(es\)\)", text)
        if scanout is None:
            return fail("expected the GPU scanout to report its size", text)
        if (scanout.group(1), scanout.group(2)) != (ready.group(1), ready.group(2)):
            return fail("the scanout size differs from the resource size", text)
        if int(scanout.group(3)) < 1 or int(scanout.group(4)) < 1:
            return fail("expected at least one flush to the host", text)
        selftest = re.search(
            r"CRUGOS: gpu selftest ok \((\d+)x(\d+), (\d+) transfer\(s\), "
            r"(\d+) flush\(es\)\)", text)
        if selftest is None:
            return fail("expected the GPU selftest to report its counters", text)
        if (selftest.group(1), selftest.group(2)) != (ready.group(1), ready.group(2)):
            return fail("the selftest size differs from the resource size", text)
        if int(selftest.group(3)) < 2 or int(selftest.group(4)) < 2:
            return fail("expected the desktop repaint to flush at least twice", text)
    elif "CRUGOS: virtio-gpu ready (" in text:
        return fail("unexpected virtio-gpu scanout without --expect-gpu=yes", text)

    if gpu_edid_mode == "yes":
        # The device advertises one detailed timing; the driver must query it
        # and adopt it instead of its fixed fallback list.
        edid = re.search(
            r"CRUGOS: gpu edid (\d+)x(\d+) \(vendor ([A-Z?]{3}), "
            r"product 0x([0-9a-f]{4}), (\d+) mode\(s\)\)", text)
        if edid is None:
            return fail("expected the GPU to decode the device EDID", text)
        if int(edid.group(5)) < 1:
            return fail("expected at least one EDID timing", text)
        mode = re.search(
            r"CRUGOS: gpu mode (\d+)x(\d+) \(edid preferred\)", text)
        if mode is None:
            return fail("expected the EDID preferred timing to become the mode", text)
        if (mode.group(1), mode.group(2)) != (edid.group(1), edid.group(2)):
            return fail("the chosen mode differs from the EDID preferred timing", text)

    if gpu_cursor_mode == "yes":
        # The hardware cursor lives on its own virtqueue; a screendump of the
        # scanout device cannot show it, so the serial counters are the proof.
        for marker in (
            "CRUGOS: gpu cursor ready (64x64, resource 2, hardware plane)",
            "CRUGOS: gpu cursor show ",
            "CRUGOS: gpu cursor move ",
        ):
            if marker not in text:
                return fail(f"missing serial marker: {marker!r}", text)
        cursor = re.search(
            r"CRUGOS: gpu cursor selftest ok \((\d+) show\(s\), "
            r"(\d+) move\(s\)\)", text)
        if cursor is None:
            return fail("expected the hardware cursor selftest counters", text)
        if int(cursor.group(1)) < 1 or int(cursor.group(2)) < 1:
            return fail("the cursor plane received no show or move", text)

    if expect_wb == "yes":
        for marker in (
            "CRUGOS: wb desktop ready",
            "CRUGOS: wb selftest ok",
        ):
            if marker not in text:
                return fail(f"missing serial marker: {marker!r}", text)
        if not re.search(r"CRUGOS: wb window \d+ opened \(-?\d+, -?\d+, \d+x\d+\)", text):
            return fail("expected a Workbench window to be opened", text)
        if not re.search(r"CRUGOS: wb window \d+ moved to \(-?\d+, -?\d+\)", text):
            return fail("expected a Workbench window drag to finish", text)
        if not re.search(r"CRUGOS: wb cursor at \(-?\d+, -?\d+\)", text):
            return fail("expected the Workbench to report cursor motion", text)

    if expect_menu == "yes":
        for marker in (
            "CRUGOS: wb desktop ready",
            "CRUGOS: wb menu",
            "CRUGOS: wb gadget",
            "CRUGOS: wb selftest ok",
        ):
            if marker not in text:
                return fail(f"missing serial marker: {marker!r}", text)
        if not re.search(r"CRUGOS: wb menu '\S+' open \(window \d+\)", text):
            return fail("expected the Workbench to open a menu", text)
        if not re.search(r"CRUGOS: wb menu '\S+' selected \('\S+'\)", text):
            return fail("expected a menu selection", text)
        if not re.search(r"CRUGOS: wb gadget '\S+' clicked in window \d+", text):
            return fail("expected a gadget click", text)
        if not re.search(r"CRUGOS: wb window \d+ closed \(gadget\)", text):
            return fail("expected the close gadget to close a window", text)
        if not re.search(r"CRUGOS: wb action \d+ (menu|gadget|close)", text):
            return fail("expected the Workbench to report the action queue", text)
        if not re.search(r"CRUGOS: wb selftest ok \(\d+ drag\(s\), [1-9]\d* action\(s\)\)", text):
            return fail("expected the Workbench to count at least one action", text)

    if expect_shell == "yes":
        for marker in (
            "CRUGOS: wb desktop ready",
            "CRUGOS: wb shell ready",
            "CRUGOS: sh run",
            "CRUGOS: sh out",
            "CRUGOS: sh selftest ok",
        ):
            if marker not in text:
                return fail(f"missing serial marker: {marker!r}", text)
        if not re.search(r"CRUGOS: wb shell ready \(window \d+, \d+x\d+ text\)", text):
            return fail("expected the Workbench to open a shell window", text)
        if not re.search(r"CRUGOS: sh run '.+'", text):
            return fail("expected the shell to run a command line", text)
        if "CRUGOS: sh out CrugOS " not in text:
            return fail("expected the shell ver command to print the version", text)
        if not re.search(r"CRUGOS: sh selftest ok \([1-9]\d* command\(s\)\)", text):
            return fail("expected the shell to count at least one command", text)

    if expect_keys == "yes":
        for marker in (
            "CRUGOS: wb desktop ready",
            "CRUGOS: wb keys ready",
            "CRUGOS: wb selftest ok",
        ):
            if marker not in text:
                return fail(f"missing serial marker: {marker!r}", text)
        if not re.search(r"CRUGOS: wb keys ready \(window -?\d+ active\)", text):
            return fail("expected the Workbench to report its active window", text)
        if not re.search(r"CRUGOS: wb window \d+ resize start \(-?\d+, -?\d+\)", text):
            return fail("expected a resize drag to start", text)
        if not re.search(r"CRUGOS: wb window \d+ resized to \d+x\d+", text):
            return fail("expected a resize drag to finish", text)
        if not re.search(r"CRUGOS: wb action \d+ resize window \d+ \(\d+x\d+\)", text):
            return fail("expected the Workbench to queue the resize event", text)
        if not re.search(r"CRUGOS: wb window \d+ focused \(Tab\)", text):
            return fail("expected Tab to move the window focus", text)
        if not re.search(r"CRUGOS: wb window \d+ key move to \(-?\d+, -?\d+\)", text):
            return fail("expected the arrow keys to move the active window", text)
        if not re.search(r"CRUGOS: wb menu '.+' item \d+ highlighted", text):
            return fail("expected the arrow keys to highlight a menu item", text)
        if not re.search(r"CRUGOS: wb menu '.+' selected \('.+'\)", text):
            return fail("expected Enter to select a menu item", text)
        if not re.search(r"CRUGOS: wb selftest ok \(\d+ drag\(s\), [1-9]\d* action\(s\)\)", text):
            return fail("expected the Workbench to count at least one action", text)

    if expect_desk == "yes":
        for marker in (
            "CRUGOS: wb desktop ready",
            "CRUGOS: wb icon ready",
            "CRUGOS: wb drawer",
            "CRUGOS: wb selftest ok",
            "CRUGOS: wb desk selftest ok",
        ):
            if marker not in text:
                return fail(f"missing serial marker: {marker!r}", text)
        if not re.search(r"CRUGOS: wb icon ready \('System:', volume\)", text):
            return fail("expected a volume object on the Workbench desktop", text)
        if not re.search(r"CRUGOS: wb icon 'System:' selected", text):
            return fail("expected a desktop object to be selected", text)
        if not re.search(r"CRUGOS: wb action \d+ icon \d+", text):
            return fail("expected the Workbench to queue an icon event", text)
        if not re.search(r"CRUGOS: wb drawer '.+' opened \([1-9]\d* entries\)", text):
            return fail("expected a double click to open a drawer", text)
        if not re.search(r"CRUGOS: wb window \d+ sent to back", text):
            return fail("expected the depth gadget to send a window to the back", text)
        if not re.search(r"CRUGOS: wb action \d+ depth window \d+", text):
            return fail("expected the Workbench to queue a depth event", text)
        if not re.search(r"CRUGOS: wb desk selftest ok \([1-9]\d* drawer\(s\)\)", text):
            return fail("expected the Workbench to count at least one drawer", text)

    if expect_icons == "yes":
        for marker in (
            "CRUGOS: wb desktop ready",
            "CRUGOS: wb icons ready",
            "CRUGOS: wb selftest ok",
            "CRUGOS: wb icons selftest ok",
        ):
            if marker not in text:
                return fail(f"missing serial marker: {marker!r}", text)
        if not re.search(r"CRUGOS: wb icons ready \([1-9]\d* object\(s\), keyboard focus\)", text):
            return fail("expected the icon table to report keyboard focus", text)
        if not re.search(r"CRUGOS: wb icon 'System:' drag to \(\d+, \d+\)", text):
            return fail("expected a desktop icon drag", text)
        if not re.search(r"CRUGOS: wb icon 'System:' dropped at \(\d+, \d+\)", text):
            return fail("expected the icon drag to end", text)
        if not re.search(r"CRUGOS: wb action \d+ icon-move 0", text):
            return fail("expected the Workbench to queue an icon move", text)
        if not re.search(r"CRUGOS: wb icon 'System:' focused", text):
            return fail("expected the keyboard to focus a desktop icon", text)
        if "CRUGOS: wb icon activated (keyboard)" not in text:
            return fail("expected Enter to activate the selected icon", text)
        if not re.search(r"CRUGOS: wb action \d+ front window \d+", text):
            return fail("expected the front gadget to queue an event", text)
        if not re.search(r"CRUGOS: wb window \d+ sent to front", text):
            return fail("expected the front gadget to raise a window", text)
        if not re.search(r"CRUGOS: wb drawer '.+' opened \([1-9]\d* entries\)", text):
            return fail("expected the keyboard to open a drawer", text)
        if not re.search(r"CRUGOS: wb icons selftest ok \([1-9]\d* move\(s\), [1-9]\d* front\(s\)\)", text):
            return fail("expected the Workbench to count icon moves and fronts", text)

    if expect_drawer == "yes":
        for marker in (
            "CRUGOS: wb desktop ready",
            "CRUGOS: wb drawer mode",
            "CRUGOS: wb drawer ready",
            "CRUGOS: wb drawer scrolled",
            "CRUGOS: wb drawer refreshed",
            "CRUGOS: wb drawer selftest ok",
            "CRUGOS: wb selftest ok",
        ):
            if marker not in text:
                return fail(f"missing serial marker: {marker!r}", text)
        if not re.search(r"CRUGOS: wb fill [1-9]\d* file\(s\)", text):
            return fail("expected the drawer gate to fill the volume", text)
        if not re.search(r"CRUGOS: wb drawer ready \([1-9]\d* entries, [1-9]\d* row\(s\)\)", text):
            return fail("expected the drawer to report its visible rows", text)
        if not re.search(r"CRUGOS: wb drawer scrolled \(top [1-9]\d* of [1-9]\d*\)", text):
            return fail("expected the arrow keys to scroll the drawer", text)
        if not re.search(r"CRUGOS: wb drawer refreshed \([1-9]\d* entries\)", text):
            return fail("expected 'r' to refresh the drawer listing", text)
        if not re.search(r"CRUGOS: wb window \d+ resized to \d+x\d+", text):
            return fail("expected the grip to resize the drawer", text)
        if not re.search(r"CRUGOS: wb action \d+ resize window \d+ \(\d+x\d+\)", text):
            return fail("expected the Workbench to queue a resize event", text)
        if not re.search(r"CRUGOS: wb drawer selftest ok \([1-9]\d* scroll\(s\), [1-9]\d* refresh\(es\)\)", text):
            return fail("expected the Workbench to count drawer scrolls", text)

    if expect_select == "yes":
        for marker in (
            "CRUGOS: wb desktop ready",
            "CRUGOS: wb select mode",
            "CRUGOS: wb drawer ready",
            "CRUGOS: wb drawer selected",
            "CRUGOS: wb drawer scrollbar to",
            "CRUGOS: wb select selftest ok",
            "CRUGOS: wb selftest ok",
        ):
            if marker not in text:
                return fail(f"missing serial marker: {marker!r}", text)
        if not re.search(r"CRUGOS: wb select mode \(click a row, drag the scrollbar\)", text):
            return fail("expected the desktop to report select mode", text)
        if not re.search(r"CRUGOS: wb drawer selected '[^']+' \(index [0-9]+ of [0-9]+\)", text):
            return fail("expected a row click to select a drawer row", text)
        if not re.search(r"CRUGOS: wb drawer scrollbar to \(top [1-9]\d* of [0-9]+\)", text):
            return fail("expected the scrollbar drag to move the view", text)
        if not re.search(r"CRUGOS: wb select selftest ok \(([1-9]\d*) select\(s\), ([1-9]\d*) scrollbar drag\(s\)\)", text):
            return fail("expected the Workbench to count selections and scrollbar drags", text)
    if screen_mode == "yes":
        for marker in (
            "CRUGOS: wb desktop ready",
            "CRUGOS: wb screens ready",
            "CRUGOS: wb screen",
            "CRUGOS: wb screens selftest ok",
            "CRUGOS: wb selftest ok",
        ):
            if marker not in text:
                return fail(f"missing serial marker: {marker!r}", text)
        if not re.search(r"CRUGOS: wb screens ready \([2-9]\d* screen\(s\), front [0-9]+\)", text):
            return fail("expected the desktop to report at least two screens", text)
        if not re.search(r"CRUGOS: wb screen \d+ front \(\d+ window\(s\)\)", text):
            return fail("expected the screen gadget to swap the front screen", text)
        if not re.search(r"CRUGOS: wb window \d+ moved to screen \d+", text):
            return fail("expected a window to move between screens", text)
        if not re.search(r"CRUGOS: wb action \d+ screen \d+ front", text):
            return fail("expected the desktop to queue a screen event", text)
        if not re.search(r"CRUGOS: wb screens selftest ok \([1-9]\d* switch\(es\), [1-9]\d* move\(s\)\)", text):
            return fail("expected the desktop to count screen switches and moves", text)

    if cx_mode == "yes":
        for marker in (
            "CRUGOS: wb desktop ready",
            "CRUGOS: wb cx ready",
            "CRUGOS: wb commodity",
            "CRUGOS: wb cx selftest ok",
            "CRUGOS: wb selftest ok",
        ):
            if marker not in text:
                return fail(f"missing serial marker: {marker!r}", text)
        if not re.search(r"CRUGOS: wb cx ready \(([1-9]\d*) commodity\(ies\)\)", text):
            return fail("expected the exchange to register at least one commodity", text)
        if not re.search(r"CRUGOS: wb action \d+ commodity Mute off", text):
            return fail("expected a manager row click to switch a commodity off", text)
        if not re.search(r"CRUGOS: wb commodity Mute blocked \(hotkey off\)", text):
            return fail("expected a disabled hotkey to be reported as blocked", text)
        if not re.search(r"CRUGOS: wb action \d+ commodity Mute fired \(muted\)", text):
            return fail("expected the Mute hotkey to fire once re-enabled", text)
        if not re.search(r"CRUGOS: wb action \d+ commodity Clock fired \(uptime [0-9]+ s\)", text):
            return fail("expected the Clock hotkey to report the uptime", text)
        if not re.search(r"CRUGOS: wb cx selftest ok \(2 toggle\(s\), 2 fire\(s\)\)", text):
            return fail("expected the exchange to count two toggles and two fires", text)

    if app_mode == "yes":
        for marker in (
            "CRUGOS: app registry ready",
            "CRUGOS: wb app icon ready",
            "CRUGOS: wb app ready",
            "CRUGOS: wb app selected",
            "CRUGOS: wb app launch",
            "CRUGOS: wb app reaped",
            "CRUGOS: wb app selftest ok",
            "CRUGOS: wb selftest ok",
        ):
            if marker not in text:
                return fail(f"missing serial marker: {marker!r}", text)
        if not re.search(r"CRUGOS: app registry ready \(([1-9]\d*) app\(s\), ([1-9]\d*) rejected\)", text):
            return fail("expected the registry to load a bundle and reject the broken one", text)
        if not re.search(r"CRUGOS: wb app ready \(([1-9]\d*) application\(s\)\)", text):
            return fail("expected the Applications window to list a bundle", text)
        if not re.search(r"CRUGOS: wb app selected \w+ \(index \d+ of \d+\)", text):
            return fail("expected a bundle row to be selected", text)
        if not re.search(r"CRUGOS: wb app launch \w+ tid \d+", text):
            return fail("expected a double click to launch a bundle", text)
        if not re.search(r"CRUGOS: wb app reaped tid \d+ exit 0", text):
            return fail("expected the launched bundle to be reaped with its exit status", text)
        if not re.search(r"CRUGOS: wb app selftest ok \(\d+ click\(s\), [1-9]\d* launch\(es\)\)", text):
            return fail("expected the desktop to count the app clicks and launches", text)
    if pkg_mode == "yes":
        for marker in (
            "CRUGOS: package registry ready",
            "CRUGOS: pkg selftest start",
            "CRUGOS: pkg selftest probe untrusted rejected (untrusted-key)",
            "CRUGOS: pkg selftest updated captest 1.0.0 -> 2.0.0",
            "CRUGOS: pkg selftest rejected downgrade (downgrade)",
            "CRUGOS: pkg selftest rejected second install (already-installed)",
            "CRUGOS: pkg selftest removed captest",
            "CRUGOS: pkg selftest rejected second remove (not-installed)",
            "CRUGOS: pkg selftest rejected untrusted (untrusted-key)",
            "CRUGOS: pkg selftest app registered captest",
            "CRUGOS: pkg selftest ok (2 installed, 1 updated, 1 removed)",
        ):
            if marker not in text:
                return fail(f"missing serial marker: {marker!r}", text)
        if not re.search(r"CRUGOS: pkg selftest probe captest 1\.0\.0 trusted", text):
            return fail("expected the signed package to probe as trusted", text)
        if not re.search(r"CRUGOS: pkg installed captest 1\.0\.0 \(\d+ file\(s\), \d+ cap\(s\)\)", text):
            return fail("expected the installer to materialise the bundle", text)
        if not re.search(r"CRUGOS: pkg updated captest 1\.0\.0 -> 2\.0\.0 "
                         r"\(\d+ file\(s\), \d+ cap\(s\)\)", text):
            return fail("expected the newer package to replace the old bundle", text)
        if not re.search(r"CRUGOS: pkg removed captest \(0 package\(s\)\)", text):
            return fail("expected removal to empty the registry", text)
        if not re.search(r"CRUGOS: pkg selftest installed captest 1\.0\.0", text):
            return fail("expected the reinstalled package in the registry", text)
    elif "CRUGOS: pkg selftest ok" in text:
        return fail("unexpected package selftest without --expect-pkg=yes", text)

    if caps_mode == "yes":
        for marker in (
            "CRUGOS: caps selftest start",
            "CRUGOS: pkg installed captest 1.0.0",
            "CRUGOS: captest ok",
            "CRUGOS: caps selftest ok (exit 5, caps 0x1)",
        ):
            if marker not in text:
                return fail(f"missing serial marker: {marker!r}", text)
        if not re.search(r"CRUGOS: caps selftest manifest 0x1 policy 0x1 granted 0x1", text):
            return fail("expected the manifest and policy to intersect at fs.read", text)
        if not re.search(r"CRUGOS: caps selftest launched tid \d+ caps 0x1", text):
            return fail("expected the launched thread to hold exactly fs.read", text)
        if "CRUGOS: captest denied fs.write" not in text:
            return fail("expected the child to be denied fs.write", text)
        if "CRUGOS: captest denied pkg.install" not in text:
            return fail("expected the child to be denied pkg.install", text)
    elif "CRUGOS: caps selftest ok" in text:
        return fail("unexpected caps selftest without --expect-caps=yes", text)

    if wbpkg_mode == "yes":
        for marker in (
            "CRUGOS: wb desktop ready",
            "CRUGOS: wb pkg ready",
            "CRUGOS: wb pkg installed /System/packages/captest.crpkg",
            "CRUGOS: wb pkg removed captest",
            "CRUGOS: wb pkg selftest ok",
            "CRUGOS: wb selftest ok",
        ):
            if marker not in text:
                return fail(f"missing serial marker: {marker!r}", text)
        if not re.search(r"CRUGOS: wb pkg ready \(window \d+, 0 package\(s\)\)", text):
            return fail("expected the Packages window to open before the registry fills", text)
        if not re.search(r"CRUGOS: wb pkg installed /System/packages/captest\.crpkg "
                         r"\(1 package\(s\)\)", text):
            return fail("expected one body click to install the signed package", text)
        if not re.search(r"CRUGOS: wb pkg removed captest \(0 package\(s\)\)", text):
            return fail("expected the second body click to remove the package", text)
        if not re.search(r"CRUGOS: wb pkg selftest ok \(2 install\(s\), 1 remove\(s\), "
                         r"1 package\(s\)\)", text):
            return fail("expected the desktop to count two installs and one removal", text)
    elif "CRUGOS: wb pkg installed" in text:
        return fail("unexpected package install without --expect-wbpkg=yes", text)

    if expect_heap == "yes":
        for marker in (
            "CRUGOS: heap ready",
            "CRUGOS: heap check ok",
            "CRUGOS: heap stress ok",
            "CRUGOS: heap guard ok",
        ):
            if marker not in text:
                return fail(f"missing serial marker: {marker!r}", text)
        if not re.search(r"CRUGOS: heap check ok \(\d+ slab\(s\), \d+ object\(s\), "
                         r"\d+ large block\(s\)\)", text):
            return fail("expected the heap check to report its inventory", text)
        if not re.search(r"CRUGOS: heap stress ok \(\d+ op\(s\), \d+ alloc\(s\), "
                         r"\d+ free\(s\)\)", text):
            return fail("expected the heap stress loop to complete", text)
        if not re.search(r"CRUGOS: heap guard ok \(3 corruption\(s\) detected\)", text):
            return fail("expected the heap guard to prove three corruptions are caught", text)

    if expect_user == "yes":
        for marker in (
            "CRUGOS: user hello from ring 3",
            "CRUGOS: user badptr blocked (EFAULT)",
            "CRUGOS: user fault killed tid",
            "CRUGOS: user selftest ok",
        ):
            if marker not in text:
                return fail(f"missing serial marker: {marker!r}", text)
        if not re.search(r"CRUGOS: user fault killed tid \d+ code -14", text):
            return fail("expected the user page fault to kill the program with -14", text)
        if not re.search(r"CRUGOS: user selftest ok \(3 program\(s\), 1 fault\(s\)\)", text):
            return fail("expected the userspace selftest to report three programs", text)
    elif "CRUGOS: user selftest ok" in text:
        return fail("unexpected userspace selftest without EXPECT_USER=yes", text)

    if expect_users == "yes":
        for marker in (
            "CRUGOS: user isolation start",
            "CRUGOS: user isolation ok",
        ):
            if marker not in text:
                return fail(f"missing serial marker: {marker!r}", text)
        if not re.search(r"CRUGOS: user isolation frames 0x[0-9a-f]+ vs 0x[0-9a-f]+ "
                         r"at 0x8000030000", text):
            return fail("expected the isolation test to translate the shared word "
                        "in both spaces", text)
        if "CRUGOS: user isolation failed" in text:
            return fail("the isolation test reported a shared frame or a mismatch", text)
        if not re.search(r"CRUGOS: user isolation ok \(2 program\(s\), 0 mismatch\(es\)\)",
                         text):
            return fail("expected the isolation test to report two programs "
                        "and no mismatch", text)
        if not re.search(r"CRUGOS: iso tid \d+ word 0x[0-9a-f]+ ok", text):
            return fail("expected the isolation programs to print their tag", text)
    elif "CRUGOS: user isolation start" in text:
        return fail("unexpected isolation selftest without EXPECT_USERS=yes", text)

    if expect_ushell == "yes":
        for marker in (
            "CRUGOS: user shell start",
            "CRUGOS: user shell finished (exit 7)",
            "CRUGOS: sh start (argc 2)",
            "CRUGOS: sh exit 7",
        ):
            if marker not in text:
                return fail(f"missing serial marker: {marker!r}", text)
        if not re.search(r"CRUGOS: sh write /shout\.txt 22 byte\(s\)", text):
            return fail("expected the ring-3 shell to write /shout.txt", text)
        for marker in ("CRUGOS: sh error", "CRUGOS: user shell failed"):
            if marker in text:
                return fail(f"the ring-3 shell reported {marker!r}", text)
    elif "CRUGOS: user shell start" in text:
        return fail("unexpected ring-3 shell without EXPECT_USHELL=yes", text)

    if expect_spawn == "yes":
        for marker in (
            "CRUGOS: user spawn test start",
            "CRUGOS: user spawn child exit 5",
            "CRUGOS: user child arg from-spawn",
            "CRUGOS: user child arg from-exec",
            "CRUGOS: user spawn test ok (child exit 5, exec exit 5)",
        ):
            if marker not in text:
                return fail(f"missing serial marker: {marker!r}", text)
        if not re.search(r"CRUGOS: user spawn tid \d+", text):
            return fail("expected the spawn program to report the child tid", text)
        if not re.search(r"CRUGOS: user exec /bin/child \(tid \d+\)", text):
            return fail("expected SYS_EXEC to replace the spawn program with /bin/child",
                        text)
        for marker in ("CRUGOS: user spawn failed",
                       "CRUGOS: user spawn wrong exit code",
                       "CRUGOS: user spawn exec failed"):
            if marker in text:
                return fail(f"the spawn program reported {marker!r}", text)
    elif "CRUGOS: user spawn test start" in text:
        return fail("unexpected spawn selftest without EXPECT_SPAWN=yes", text)

    if expect_fork == "yes":
        for marker in (
            "CRUGOS: user fork test start",
            "CRUGOS: user fork child word 8738",
            "CRUGOS: user fork child second 21845",
            "CRUGOS: user fork parent word 4369",
            "CRUGOS: user fork parent second 13107",
            "CRUGOS: user fork parent write 17476",
            "CRUGOS: user fork test ok (child exit 3, memory isolated)",
        ):
            if marker not in text:
                return fail(f"missing serial marker: {marker!r}", text)
        fork_line = re.search(
            r"CRUGOS: user fork tid \d+ -> \d+ \(([0-9]+) page\(s\) shared, COW\)",
            text)
        if not fork_line:
            return fail("expected the kernel to report a shared copy-on-write fork",
                        text)
        shared_pages = int(fork_line.group(1))
        if shared_pages < 2:
            return fail(f"the fork shared {shared_pages} page(s), expected at least 2",
                        text)
        if not re.search(r"CRUGOS: user fork tid \d+", text):
            return fail("expected the forking program to report the child tid", text)
        copies = re.findall(
            r"CRUGOS: user cow fault tid \d+ va=0x[0-9a-f]+ \((copy)\)", text)
        if len(copies) < 2:
            return fail(f"expected two copy-on-write copies, saw {len(copies)}", text)
        if not re.search(
            r"CRUGOS: user cow fault tid \d+ va=0x[0-9a-f]+ \((sole owner)\)",
            text):
            return fail("expected the parent's post-reap store to keep its frame",
                        text)
        for marker in ("CRUGOS: user fork failed",
                       "CRUGOS: user fork wrong child exit",
                       "CRUGOS: user fork memory not isolated",
                       "CRUGOS: user fork parent write lost"):
            if marker in text:
                return fail(f"the fork program reported {marker!r}", text)
    elif "CRUGOS: user fork test start" in text:
        return fail("unexpected fork selftest without EXPECT_FORK=yes", text)

    if expect_forkbig == "yes":
        for marker in (
            "CRUGOS: user forkbig test start",
            "CRUGOS: user forkbig test ok (child exit 7, 300 page(s) isolated)",
        ):
            if marker not in text:
                return fail(f"missing serial marker: {marker!r}", text)
        if not re.search(r"CRUGOS: user forkbig tid \d+", text):
            return fail("expected the large fork to report the child tid", text)
        big_pages = [int(n) for n in re.findall(
            r"CRUGOS: user fork tid \d+ -> \d+ \(([0-9]+) page\(s\) shared, COW\)",
            text)]
        if not big_pages or max(big_pages) < 300:
            return fail(f"the large fork shared {big_pages} page(s), expected >= 300",
                        text)
        if not re.search(
            r"CRUGOS: user cow fault tid \d+ va=0x[0-9a-f]+ \((copy)\)",
            text):
            return fail("expected the large fork's child stores to copy", text)
        for marker in ("CRUGOS: user forkbig failed",
                       "CRUGOS: user forkbig memory not isolated"):
            if marker in text:
                return fail(f"the large fork program reported {marker!r}", text)
    elif "CRUGOS: user forkbig test start" in text:
        return fail("unexpected large-fork selftest without EXPECT_FORKBIG=yes",
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
          "gfx", expect_gfx, "wb", expect_wb, "menu", expect_menu,
          "shell", expect_shell, "keys", expect_keys, "desk", expect_desk,
          "icons", expect_icons, "drawer", expect_drawer,
          "select", expect_select, "screen", screen_mode, "cx", cx_mode,
          "app", app_mode, "pkg", pkg_mode, "caps", caps_mode,
          "wbpkg", wbpkg_mode, "gpu", gpu_mode,
          "gpuedid", gpu_edid_mode, "gpucursor", gpu_cursor_mode,
          "heap", expect_heap, "user", expect_user,
          "isolated", expect_users, "ushell", expect_ushell,
          "spawn", expect_spawn, "fork", expect_fork,
          "forkbig", expect_forkbig,
          "storage", storage_mode,
          "QEMU exit status", status)
    for line in boot_report:
        print("  " + line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
