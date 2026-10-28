#!/usr/bin/env python3
"""Verify a CrugOS boot: serial markers, guest version, CPU policy, storage mode, encryption, networking, TCP, DNS, USB, HID mice, audio, the framebuffer cursor, the Workbench desktop, its menus and gadgets, its shell window, its keyboard
navigation, its desktop objects and its scrollable drawer, then the exit code.

Pass --storage=none anywhere on the command line when the boot medium ships no
block device (the hybrid ISO does): the storage, journal, GPT and ext4 markers
are then replaced by the single 'CRUGOS: storage skipped' marker.

usage: boot-check.py SERIAL_LOG EXIT_CODE_FILE \
    [VERSION_FILE [EXPECT_AVX2 [EXPECT_RAID [EXPECT_NVME [EXPECT_REBUILD \
    [EXPECT_AHCI [EXPECT_CRYPT [EXPECT_AES [EXPECT_NET [EXPECT_TCP \
    [EXPECT_DNS [EXPECT_USB [EXPECT_MOUSE [EXPECT_AUDIO \
    [EXPECT_GFX [EXPECT_WB [EXPECT_MENU [EXPECT_SHELL [EXPECT_KEYS [EXPECT_DESK \
    [EXPECT_ICONS [EXPECT_DRAWER [EXPECT_HEAP]]]]]]]]]]]]]]]]]]]]]]] \
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
    "CRUGOS: phase h3 boot ok",
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
    argv = []
    for arg in sys.argv[1:]:
        if arg == "--storage=none":
            storage_mode = "none"
        elif arg.startswith("--storage="):
            print(f"FAIL: invalid storage mode {arg!r}", file=sys.stderr)
            return 2
        else:
            argv.append(arg)
    sys.argv = [sys.argv[0]] + argv

    if len(sys.argv) not in tuple(range(3, 28)):
        print("usage: boot-check.py SERIAL_LOG EXIT_CODE_FILE "
              "[VERSION_FILE [EXPECT_AVX2 [EXPECT_RAID [EXPECT_NVME "
              "[EXPECT_REBUILD [EXPECT_AHCI [EXPECT_CRYPT [EXPECT_AES "
              "[EXPECT_NET [EXPECT_TCP [EXPECT_DNS [EXPECT_USB "
              "[EXPECT_MOUSE [EXPECT_AUDIO [EXPECT_GFX [EXPECT_WB "
              "[EXPECT_MENU [EXPECT_SHELL [EXPECT_KEYS [EXPECT_DESK "
              "[EXPECT_ICONS [EXPECT_DRAWER [EXPECT_SELECT]]]]]]]]]]]]]]]]]]]]]]] "
              "[--storage=none]",
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
          "select", expect_select, "heap", expect_heap,
          "storage", storage_mode,
          "QEMU exit status", status)
    for line in boot_report:
        print("  " + line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
