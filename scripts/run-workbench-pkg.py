#!/usr/bin/env python3
"""Verify the Workbench Packages window (phase 7C).

The driver boots QEMU with an xHCI controller but no USB device, hotplugs a
usb-mouse over QMP once the guest has opened its windows, and clicks the
Packages window body three times: install, remove, install.  The guest prints a
CRUGOS marker for each transition, the desktop selftest waits for all three
(a wb.acts=3 boot token), and a screendump shows the "installed" line come back
in the window body.

usage: run-workbench-pkg.py --disk IMG --serial-log LOG --exit-code FILE [options]
"""

import argparse
import json
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

DESKTOP = b"CRUGOS: wb desktop ready"
PKG_READY = b"CRUGOS: wb pkg ready"
MOUSE = b"CRUGOS: usb mouse ready"
GFX_READY = b"CRUGOS: gfx ready"
CURSOR = b"CRUGOS: wb cursor at ("
INSTALLED = b"CRUGOS: wb pkg installed /System/packages/captest.crpkg"
REMOVED = b"CRUGOS: wb pkg removed captest"
PKG_OK = b"CRUGOS: wb pkg selftest ok"
OK = b"CRUGOS: wb selftest ok"
HOTPLUG_ID = "mouse0"

GFX_READY_RE = re.compile("CRUGOS: gfx ready \\(\\d+x\\d+, cursor at (-?\\d+),(-?\\d+)\\)")
CURSOR_RE = re.compile("CRUGOS: wb cursor at \\((-?\\d+), (-?\\d+)\\)")

# The Packages window opens at (40, 330) and is 420x200, so a press deep in
# its body is far from the title bar, the close gadget and the border.
BODY_CLICK = (250, 430)
FRAME_PIXEL = (40, 330)
# Scan the window body only: the "installed" line is drawn at its bottom edge,
# while the title bar above it is amber whenever the window is active.
SCAN = (44, 360, 456, 526)
HIGHLIGHT = (255, 176, 0)
FRAME = (255, 255, 255)


def fail(message):
    print("FAIL: " + message)
    return 1


def read_log(path):
    try:
        return Path(path).read_bytes()
    except OSError:
        return b""


def wait_for(path, needle, count, proc, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if read_log(path).count(needle) >= count:
            return True
        if proc.poll() is not None:
            return read_log(path).count(needle) >= count
        time.sleep(0.05)
    return False


class Qmp:
    """A minimal QMP client: capabilities, device_add, input events, screendump."""

    def __init__(self, path, timeout=30.0):
        deadline = time.monotonic() + timeout
        while True:
            try:
                self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                self.sock.connect(path)
                break
            except OSError:
                self.sock.close()
                if time.monotonic() >= deadline:
                    raise
                time.sleep(0.05)
        self.file = self.sock.makefile("rwb")
        self._read()  # QMP greeting
        self._cmd({"execute": "qmp_capabilities"})

    def _read(self):
        line = self.file.readline()
        if not line:
            raise RuntimeError("QMP connection closed")
        return json.loads(line.decode())

    def _cmd(self, message):
        self.file.write((json.dumps(message) + chr(10)).encode())
        self.file.flush()
        while True:
            reply = self._read()
            if "event" in reply:
                continue
            if "error" in reply:
                raise RuntimeError("QMP error: %s" % reply["error"])
            return reply

    def device_add(self, driver, ident, bus):
        self._cmd({"execute": "device_add",
                   "arguments": {"driver": driver, "id": ident, "bus": bus}})

    def send_events(self, events):
        # No "device": input-send-event resolves it with qdev_find_recursive
        # (sysbus only) and a USB device is not reachable from there. With
        # usb=off and i8042=off the hotplugged mouse is the only handler.
        self._cmd({"execute": "input-send-event",
                   "arguments": {"events": events}})

    def move(self, dx, dy):
        events = []
        if dx:
            events.append({"type": "rel", "data": {"axis": "x", "value": dx}})
        if dy:
            events.append({"type": "rel", "data": {"axis": "y", "value": dy}})
        self.send_events(events)

    def button(self, down, name="left"):
        self.send_events([{"type": "btn",
                           "data": {"down": bool(down), "button": name}}])

    def screendump(self, path):
        try:
            self._cmd({"execute": "screendump",
                       "arguments": {"filename": str(path)}})
        except OSError as exc:
            raise RuntimeError("QEMU monitor closed: %s" % exc) from exc

    def close(self):
        try:
            self.file.close()
            self.sock.close()
        except OSError:
            pass


def read_ppm(path):
    """Parse a binary P6 PPM and return (width, height, bytes of RGB data)."""
    data = Path(path).read_bytes()
    fields = []
    pos = 0
    while len(fields) < 4:
        while pos < len(data) and data[pos:pos + 1].isspace():
            pos += 1
        if pos < len(data) and data[pos:pos + 1] == b"#":
            while pos < len(data) and data[pos:pos + 1] != b"\n":
                pos += 1
            continue
        start = pos
        while pos < len(data) and not data[pos:pos + 1].isspace():
            pos += 1
        if start == pos:
            raise ValueError("truncated PPM header")
        fields.append(data[start:pos])
    pos += 1  # exactly one whitespace byte after maxval
    if fields[0] != b"P6":
        raise ValueError("not a P6 PPM: %r" % fields[0])
    width = int(fields[1])
    height = int(fields[2])
    if int(fields[3]) != 255:
        raise ValueError("unsupported maxval %r" % fields[3])
    pixels = data[pos:pos + width * height * 3]
    if len(pixels) != width * height * 3:
        raise ValueError("truncated PPM pixel data")
    return width, height, pixels


def pixel_at(path, x, y):
    width, height, pixels = read_ppm(path)
    if x < 0 or y < 0 or x >= width or y >= height:
        raise ValueError("pixel (%d, %d) outside %dx%d" % (x, y, width, height))
    off = (y * width + x) * 3
    return (pixels[off], pixels[off + 1], pixels[off + 2])


def count_color(path, region, colour):
    """Count pixels of one exact colour inside (x0, y0, x1, y1)."""
    width, height, pixels = read_ppm(path)
    x0, y0, x1, y1 = region
    found = 0
    for y in range(max(0, y0), min(height, y1)):
        row = y * width
        for x in range(max(0, x0), min(width, x1)):
            off = (row + x) * 3
            if (pixels[off], pixels[off + 1], pixels[off + 2]) == colour:
                found += 1
    return found


def last_position(path, pattern, fallback):
    text = read_log(path).decode("utf-8", "replace")
    found = pattern.findall(text)
    if not found:
        return fallback
    return int(found[-1][0]), int(found[-1][1])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--make", default="make")
    ap.add_argument("--repo", default=str(Path(__file__).resolve().parent.parent))
    ap.add_argument("--disk", required=True)
    ap.add_argument("--serial-log", required=True)
    ap.add_argument("--exit-code", required=True)
    ap.add_argument("--wb-wait", type=int, default=30)
    ap.add_argument("--boot-timeout", type=float, default=60.0)
    ap.add_argument("--hotplug-timeout", type=float, default=30.0)
    ap.add_argument("--event-timeout", type=float, default=15.0)
    ap.add_argument("--run-timeout", type=float, default=240.0)
    ap.add_argument("--qemu-extra", default="-device qemu-xhci,id=xhci")
    args = ap.parse_args()

    if args.wb_wait < 1 or args.wb_wait > 120:
        return fail("--wb-wait must be between 1 and 120")

    serial_log = Path(args.serial_log).resolve()
    serial_log.parent.mkdir(parents=True, exist_ok=True)
    serial_log.write_bytes(b"")
    exit_code_path = Path(args.exit_code).resolve()
    exit_code_path.parent.mkdir(parents=True, exist_ok=True)
    try:
        exit_code_path.unlink()
    except OSError:
        pass

    tmpdir = Path(tempfile.mkdtemp(prefix=".wbp.",
                                   dir=str(Path(args.disk).resolve().parent)))
    sock_path = str(tmpdir / "qmp.sock")

    # i8042=off removes the PS/2 controller and usb=off the machine USB
    # controllers, so the hotplugged USB mouse is the only pointer device.
    # wb.acts=3 makes the desktop wait for the install, the removal and the
    # second install before its selftest reports.
    cmdline = "wb.enable=1 wb.wait=%d wb.pkg=1 wb.acts=3" % args.wb_wait
    cmd = [
        args.make,
        "--no-print-directory",
        "run",
        "DISK_IMG=" + str(Path(args.disk).resolve()),
        "CRYPT_PASS=",
        "NET_MODE=none",
        "QEMU_DISPLAY=none",
        "SERIAL_TCP=",
        "SERIAL_LOG=" + str(serial_log),
        "QEMU_EXIT=" + str(exit_code_path),
        "QEMU_EXTRA=-qmp unix:%s,server=on,wait=off %s"
        % (sock_path, args.qemu_extra),
        "MACHINE_EXTRA=usb=off,i8042=off",
        "CMDLINE_EXTRA=" + cmdline + " wb.theme=classic",
        "TIMEOUT_SEC=" + str(int(args.run_timeout)),
    ]
    print("wbp: starting " + " ".join(cmd))
    proc = subprocess.Popen(cmd, cwd=args.repo)

    qmp = None
    state = {"x": 0, "y": 0}
    try:
        if not wait_for(serial_log, DESKTOP, 1, proc, args.boot_timeout):
            return fail("guest never reported the Workbench desktop")
        if not wait_for(serial_log, PKG_READY, 1, proc, args.event_timeout):
            return fail("guest never reported the Packages window")
        qmp = Qmp(sock_path)
        qmp.device_add("usb-mouse", HOTPLUG_ID, "xhci.0")
        if not wait_for(serial_log, MOUSE, 1, proc, args.hotplug_timeout):
            return fail("hotplugged mouse was never enumerated")
        if not wait_for(serial_log, GFX_READY, 1, proc, args.hotplug_timeout):
            return fail("guest never reported a framebuffer cursor")
        started = last_position(serial_log, GFX_READY_RE, (0, 0))
        state["x"], state["y"] = started
        print("wbp: cursor starts at (%d, %d)" % started)

        def move_to(target, timeout):
            # The USB HID boot mouse reports 8-bit deltas, so a long jump is
            # split into several reports and the guest can drop one. Move in
            # <=100 px steps and follow the position the guest reports.
            for _ in range(200):
                cx, cy = state["x"], state["y"]
                if cx == target[0] and cy == target[1]:
                    print("wbp: pointer at (%d, %d)" % (cx, cy))
                    return
                dx = max(-100, min(100, target[0] - cx))
                dy = max(-100, min(100, target[1] - cy))
                markers = read_log(serial_log).count(CURSOR)
                qmp.move(dx, dy)
                if not wait_for(serial_log, CURSOR, markers + 1, proc, timeout):
                    raise RuntimeError("the guest stopped reporting the cursor")
                state["x"], state["y"] = last_position(serial_log, CURSOR_RE,
                                                       (cx, cy))
            raise RuntimeError("the pointer never reached %r" % (target,))

        def press(target):
            move_to(target, args.event_timeout)
            qmp.button(True)
            time.sleep(0.15)
            qmp.button(False)

        # 1. The window is up before the click, and its body has no
        #    highlight yet: the "installed" line only appears afterwards.
        before = tmpdir / "before.ppm"
        frame = None
        for attempt in range(40):
            qmp.screendump(before)
            frame = pixel_at(before, FRAME_PIXEL[0], FRAME_PIXEL[1])
            if frame == FRAME:
                break
            time.sleep(0.05)
        marks = count_color(before, SCAN, HIGHLIGHT)
        print("wbp: frame %s, highlight pixels before the click %d"
              % (frame, marks))
        if frame != FRAME:
            return fail("the Packages window frame pixel is %s" % (frame,))
        if marks != 0:
            return fail("the window body is already highlighted: %d pixel(s)"
                        % marks)

        # 2. One click on the body installs the signed package.
        press(BODY_CLICK)
        if not wait_for(serial_log, INSTALLED, 1, proc, args.event_timeout):
            print("wbp: no install marker yet, clicking again")
            press(BODY_CLICK)
            if not wait_for(serial_log, INSTALLED, 1, proc,
                            args.event_timeout):
                return fail("the Packages window never installed the package")

        # 3. A second click removes it again: bundle, record and registry.
        press(BODY_CLICK)
        if not wait_for(serial_log, REMOVED, 1, proc, args.event_timeout):
            return fail("the Packages window never removed the package")

        # 4. A third click installs it again, which proves the window can go
        #    both ways without a reboot.
        press(BODY_CLICK)
        if not wait_for(serial_log, INSTALLED, 2, proc, args.event_timeout):
            return fail("the Packages window never reinstalled the package")

        # 5. The body now paints the result line and the registry holds the
        #    package once more.
        # The host display update is asynchronous, so a screendump taken right
        # after the marker can still show the frame from before the repaint:
        # retry for a bounded time and report how many attempts it took.
        after = tmpdir / "after.ppm"
        marks = 0
        for attempt in range(40):
            qmp.screendump(after)
            marks = count_color(after, SCAN, HIGHLIGHT)
            if marks:
                if attempt:
                    print("wbp: result paint settled after %d retry(ies)"
                          % attempt)
                break
            time.sleep(0.05)
        print("wbp: highlight pixels after the reinstall %d" % marks)
        if marks == 0:
            return fail("the installed line is missing from the window body")

        if not wait_for(serial_log, PKG_OK, 1, proc, args.event_timeout):
            return fail("guest never finished the Packages selftest")
        if not wait_for(serial_log, OK, 1, proc, args.event_timeout):
            return fail("guest never finished the Workbench selftest")

        try:
            proc.wait(timeout=args.run_timeout)
        except subprocess.TimeoutExpired:
            proc.kill()
            return fail("run timed out")
    except (RuntimeError, ValueError) as exc:
        return fail(str(exc))
    finally:
        if qmp is not None:
            qmp.close()
        if proc.poll() is None:
            proc.kill()
        shutil.rmtree(str(tmpdir), ignore_errors=True)

    data = read_log(serial_log)
    if not exit_code_path.exists():
        return fail("missing QEMU exit code file " + str(exit_code_path))
    try:
        status = int(exit_code_path.read_text().strip())
    except ValueError:
        return fail("invalid QEMU exit code file " + str(exit_code_path))
    if status != 33:
        return fail("guest exit status %d != expected 33" % status)
    if OK not in data:
        return fail("guest did not report the Workbench selftest")
    if PKG_OK not in data:
        return fail("guest did not report the Packages selftest")

    if REMOVED not in data:
        return fail("guest did not report the package removal")
    print("ok: workbench package toggle (2 installs, 1 remove, exit %d)"
          % status)
    return 0


if __name__ == "__main__":
    sys.exit(main())
