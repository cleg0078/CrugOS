#!/usr/bin/env python3
"""Verify desktop icon dragging, send-to-front and keyboard icon focus (phase 6F).

The driver boots QEMU with an xHCI controller and no USB device, waits for the
Workbench to enter icon mode (wb.desk=1 wb.ikeys=1) and hotplugs a usb-mouse
and a usb-kbd. It first focuses the System: volume object from the keyboard
(Enter selects, Enter activates) which opens the drawer. It then drags the
object with the mouse to (168, 376) and proves the tile moved, that the old
place is desktop again and that the drawer still lists the root volume. Last it
clicks window 1 to raise it, presses the depth gadget (title turns idle grey)
and the send-to-front gadget (title turns amber) with screendumps in between.

usage: run-workbench-icons.py --disk IMG --serial-log LOG --exit-code FILE [options]
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
ICON_READY = b"CRUGOS: wb icon ready ('System:', volume)"
ICONS_READY = b"CRUGOS: wb icons ready"
MOUSE = b"CRUGOS: usb mouse ready"
KEYBOARD = b"CRUGOS: usb keyboard ready"
GFX_READY = b"CRUGOS: gfx ready"
FOCUSED = b"CRUGOS: wb icon 'System:' focused"
ACTIVATED = b"CRUGOS: wb icon activated (keyboard)"
DRAWER_OPEN = b"CRUGOS: wb drawer 'System:' opened"
DROPPED = b"CRUGOS: wb icon 'System:' dropped at (168, 376)"
SENT_BACK = b"CRUGOS: wb window 1 sent to back"
SENT_FRONT = b"CRUGOS: wb window 1 sent to front"
ICONS_OK = b"CRUGOS: wb icons selftest ok"
OK = b"CRUGOS: wb selftest ok"
MOUSE_ID = "mouse0"
KEYBOARD_ID = "kbd0"

GFX_READY_RE = re.compile(r"CRUGOS: gfx ready \(\d+x\d+, cursor at (-?\d+),(-?\d+)\)")
CURSOR_RE = re.compile(r"CRUGOS: wb cursor at \((-?\d+), (-?\d+)\)")
DRAWER_RE = re.compile(r"CRUGOS: wb drawer 'System:' opened \(([1-9]\d*) entries\)")

# The System: volume object is a 64x48 tile at (16, 240); its centre is
# (48, 264). The dragged tile lands at (168, 376) when the pointer is released
# at (200, 400): the grab offset inside the tile is (32, 24).
ICON_CENTRE = (48, 264)
DROP_POINTER = (200, 400)
DROP_FRAME = (168, 376)
DROP_INNER = (176, 384)
OLD_SPOT = (48, 264)
# Window 1 is (80, 90) 360x220: its body is at (200, 150), its depth gadget
# centre is (410, 102) and its send-to-front gadget centre is (394, 102).
WINDOW1_BODY = (200, 150)
DEPTH_GADGET = (410, 102)
FRONT_GADGET = (394, 102)
WINDOW1_TITLE = (90, 96)

FRAME_RGB = (255, 255, 255)
SELECTED_RGB = (255, 176, 0)
IDLE_RGB = (128, 128, 128)
WINDOW_RGB = (8, 16, 24)
DESKTOP_RGB = (16, 32, 48)


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
    """A minimal QMP client: device_add, pointer events, keys and screendump."""

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
        self.file.write((json.dumps(message) + "\n").encode())
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
        self._cmd({"execute": "input-send-event",
                   "arguments": {"events": events}})

    def move(self, dx, dy):
        events = []
        if dx:
            events.append({"type": "rel",
                           "data": {"axis": "x", "value": dx}})
        if dy:
            events.append({"type": "rel",
                           "data": {"axis": "y", "value": dy}})
        self.send_events(events)

    def button(self, down, name="left"):
        self.send_events([{"type": "btn",
                          "data": {"down": bool(down), "button": name}}])

    def click(self):
        self.button(True)
        time.sleep(0.05)
        self.button(False)

    def send_key(self, name, hold_ms=40):
        self._cmd({"execute": "send-key",
                   "arguments": {"keys": [{"type": "qcode", "data": name}],
                                 "hold-time": hold_ms}})

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
    ap.add_argument("--acts", type=int, default=5)
    ap.add_argument("--wb-wait", type=int, default=30)
    ap.add_argument("--boot-timeout", type=float, default=60.0)
    ap.add_argument("--hotplug-timeout", type=float, default=30.0)
    ap.add_argument("--event-timeout", type=float, default=15.0)
    ap.add_argument("--run-timeout", type=float, default=240.0)
    ap.add_argument("--qemu-extra", default="-device qemu-xhci,id=xhci")
    args = ap.parse_args()

    if args.acts < 1 or args.acts > 8:
        return fail("--acts must be between 1 and 8")
    if args.acts != 5:
        return fail("the scripted sequence performs exactly 5 actions")
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
    tmpdir = Path(tempfile.mkdtemp(prefix=".wbi.",
                                   dir=str(Path(args.disk).resolve().parent)))
    sock_path = str(tmpdir / "qmp.sock")

    # i8042=off removes the PS/2 controller and usb=off the machine USB
    # controllers, so the hotplugged mouse and keyboard are the only input
    # devices. The icon table owns the keyboard (wb.ikeys=1); the drawer and
    # the depth/raise gadgets need the mouse.
    cmdline = "wb.enable=1 wb.wait=%d wb.desk=1 wb.ikeys=1 wb.acts=%d" % (
        args.wb_wait, args.acts)
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
        "CMDLINE_EXTRA=" + cmdline,
        "TIMEOUT_SEC=" + str(int(args.run_timeout)),
    ]
    print("wbi: starting " + " ".join(cmd))
    proc = subprocess.Popen(cmd, cwd=args.repo)

    qmp = None
    state = {"x": 0, "y": 0}
    try:
        if not wait_for(serial_log, DESKTOP, 1, proc, args.boot_timeout):
            return fail("guest never reported the Workbench desktop")
        qmp = Qmp(sock_path)
        qmp.device_add("usb-mouse", MOUSE_ID, "xhci.0")
        qmp.device_add("usb-kbd", KEYBOARD_ID, "xhci.0")
        if not wait_for(serial_log, MOUSE, 1, proc, args.hotplug_timeout):
            return fail("hotplugged mouse was never enumerated")
        if not wait_for(serial_log, KEYBOARD, 1, proc, args.hotplug_timeout):
            return fail("hotplugged keyboard was never enumerated")
        if not wait_for(serial_log, ICON_READY, 1, proc, args.hotplug_timeout):
            return fail("guest never put the System: object on the desktop")
        if not wait_for(serial_log, ICONS_READY, 1, proc, args.hotplug_timeout):
            return fail("guest never entered icon keyboard mode")
        if not wait_for(serial_log, GFX_READY, 1, proc, args.hotplug_timeout):
            return fail("guest never reported a framebuffer cursor")
        shown = last_position(serial_log, GFX_READY_RE, (0, 0))
        state["x"], state["y"] = shown
        print("wbi: cursor starts at (%d, %d)" % shown)

        def move_to(target, timeout):
            # The USB HID boot mouse reports 8-bit deltas, so a long jump is
            # split into several reports and the guest can drop one. Move in
            # <=100 px steps and follow the position the guest reports.
            stall = 0
            for _ in range(200):
                cx, cy = state["x"], state["y"]
                if cx == target[0] and cy == target[1]:
                    print("wbi: pointer at (%d, %d)" % (cx, cy))
                    return
                dx = max(-100, min(100, target[0] - cx))
                dy = max(-100, min(100, target[1] - cy))
                markers = read_log(serial_log).count(b"CRUGOS: wb cursor at (")
                qmp.move(dx, dy)
                if not wait_for(serial_log, b"CRUGOS: wb cursor at (",
                                markers + 1, proc, timeout):
                    raise RuntimeError("guest never reported the pointer move")
                got = last_position(serial_log, CURSOR_RE, (cx, cy))
                state["x"], state["y"] = got
                if got == (cx, cy):
                    stall += 1
                    if stall >= 3:
                        raise RuntimeError("pointer stuck at (%d, %d)"
                                           % (got[0], got[1]))
                else:
                    stall = 0
            raise RuntimeError("pointer did not reach (%d, %d)" % target)

        def expect(needle, what, count=1):
            if not wait_for(serial_log, needle, count, proc, args.event_timeout):
                raise RuntimeError("guest never reported " + what)
            print("wbi: " + what)

        # 1. The keyboard focuses the icon table: the first Enter selects the
        #    System: volume and the second one activates it, opening the drawer.
        qmp.send_key("ret")
        expect(FOCUSED, "the System: object took keyboard focus")
        qmp.send_key("ret")
        expect(ACTIVATED, "the keyboard activated the System: object")
        expect(DRAWER_OPEN, "the drawer opened from the volume object")
        text = read_log(serial_log).decode("utf-8", "replace")
        match = DRAWER_RE.search(text)
        if not match:
            return fail("the drawer did not list any root entry")
        print("wbi: drawer lists %s root entries" % match.group(1))

        # 2. The pointer grabs the object and drags it to (168, 376).
        move_to(ICON_CENTRE, args.event_timeout)
        qmp.button(True)
        move_to(DROP_POINTER, args.event_timeout)
        qmp.button(False)
        expect(DROPPED, "the System: object was dropped")
        shot = tmpdir / "dragged.ppm"
        qmp.screendump(shot)
        frame = pixel_at(shot, *DROP_FRAME)
        inner = pixel_at(shot, *DROP_INNER)
        old = pixel_at(shot, *OLD_SPOT)
        print("wbi: dragged icon frame %s, inner %s, old spot %s"
              % (frame, inner, old))
        # The press that starts the drag also selects the object, so the
        # frame of the dragged tile is the amber selection colour.
        if frame != SELECTED_RGB:
            return fail("the dragged object frame is %s, expected selected %s"
                        % (frame, SELECTED_RGB))
        if inner != WINDOW_RGB:
            return fail("the dragged object inner tile is %s, expected %s"
                        % (inner, WINDOW_RGB))
        if old != DESKTOP_RGB:
            return fail("the old object position is %s, expected desktop %s"
                        % (old, DESKTOP_RGB))

        # 3. Raise window 1, then send it to the back with the depth gadget:
        #    its title turns idle grey.
        move_to(WINDOW1_BODY, args.event_timeout)
        qmp.click()
        time.sleep(0.2)
        move_to(DEPTH_GADGET, args.event_timeout)
        qmp.click()
        expect(SENT_BACK, "the depth gadget sent window 1 to the back")
        shot = tmpdir / "depth.ppm"
        qmp.screendump(shot)
        title = pixel_at(shot, *WINDOW1_TITLE)
        print("wbi: idle window 1 title %s" % (title,))
        if title != IDLE_RGB:
            return fail("window 1 title is %s, expected idle %s"
                        % (title, IDLE_RGB))

        # 4. The send-to-front gadget raises it again: the title turns amber.
        move_to(FRONT_GADGET, args.event_timeout)
        qmp.click()
        expect(SENT_FRONT, "the front gadget sent window 1 to the front")
        shot = tmpdir / "front.ppm"
        qmp.screendump(shot)
        title = pixel_at(shot, *WINDOW1_TITLE)
        print("wbi: active window 1 title %s" % (title,))
        if title != SELECTED_RGB:
            return fail("window 1 title is %s, expected active %s"
                        % (title, SELECTED_RGB))

        expect(ICONS_OK, "the icon selftest finished")
        expect(OK, "the Workbench selftest finished")

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
    if ICONS_OK not in data:
        return fail("guest did not report the icon selftest")
    if b"wb action timeout" in data or b"wb pointer timeout" in data:
        return fail("guest hit a Workbench action timeout")
    if b"dialog" in data:
        return fail("unexpected dialog marker in the serial log")

    print("ok: workbench icon dragging, send-to-front and keyboard focus (exit %d)"
          % status)
    return 0


if __name__ == "__main__":
    sys.exit(main())
