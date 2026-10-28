#!/usr/bin/env python3
"""Verify the Workbench desktop and a title-bar drag (phase 6A).

The driver boots QEMU with an xHCI controller but no USB device, hotplugs a
usb-mouse over QMP once the guest has opened its windows, and drives a drag
through input-send-event relative moves plus a button press and release. The
guest gates itself through wb.wait= and wb.drag= and prints a marker for every
window event; a QEMU screendump then proves the active title bar moved in the
framebuffer pixels.

usage: run-workbench.py --disk IMG --serial-log LOG --exit-code FILE [options]
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
MOUSE = b"CRUGOS: usb mouse ready"
GFX_READY = b"CRUGOS: gfx ready"
CURSOR = b"CRUGOS: wb cursor at ("
ACTIVE = b"CRUGOS: wb window 2 active"
DRAG_START = b"CRUGOS: wb window 2 drag start"
MOVED = b"CRUGOS: wb window 2 moved to"
OK = b"CRUGOS: wb selftest ok"
HOTPLUG_ID = "mouse0"

GFX_READY_RE = re.compile(r"CRUGOS: gfx ready \(\d+x\d+, cursor at (-?\d+),(-?\d+)\)")
CURSOR_RE = re.compile(r"CRUGOS: wb cursor at \((-?\d+), (-?\d+)\)")
MOVED_RE = re.compile(r"CRUGOS: wb window 2 moved to \((-?\d+), (-?\d+)\)")

# Window 2 opens at (480, 300) with a 320x200 client area; the hit point below
# is inside its title bar (border 2, title height 20) so the press starts a
# drag and the pointer offset is (32, 12).
TITLE_DX = 32
TITLE_DY = 12
PRESS_MOVE = (0, -72)
DRAG_MOVES = [(45, 25), (45, 25)]

# Colours as they appear in the PPM: the active title bar and the window frame.
TITLE_ACTIVE = (255, 176, 0)
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
        # No "device": input-send-event resolves it with qdev_find_recursive
        # (sysbus only) and a USB device is not reachable from there. With
        # usb=off and i8042=off the hotplugged mouse is the only handler.
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
    ap.add_argument("--drag", type=int, default=1)
    ap.add_argument("--wb-wait", type=int, default=30)
    ap.add_argument("--boot-timeout", type=float, default=60.0)
    ap.add_argument("--hotplug-timeout", type=float, default=30.0)
    ap.add_argument("--event-timeout", type=float, default=15.0)
    ap.add_argument("--run-timeout", type=float, default=240.0)
    ap.add_argument("--qemu-extra", default="-device qemu-xhci,id=xhci")
    args = ap.parse_args()

    if args.drag < 1 or args.drag > 4:
        return fail("--drag must be between 1 and 4")
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

    tmpdir = Path(tempfile.mkdtemp(prefix=".wb.",
                                   dir=str(Path(args.disk).resolve().parent)))
    sock_path = str(tmpdir / "qmp.sock")

    # i8042=off removes the PS/2 controller and usb=off the machine USB
    # controllers, so the hotplugged USB mouse is the only pointer device.
    cmdline = "wb.enable=1 wb.wait=%d wb.drag=%d" % (args.wb_wait, args.drag)
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
    print("wb: starting " + " ".join(cmd))
    proc = subprocess.Popen(cmd, cwd=args.repo)

    qmp = None
    drags = 0
    bounds = (0, 0)
    try:
        if not wait_for(serial_log, DESKTOP, 1, proc, args.boot_timeout):
            return fail("guest never reported the Workbench desktop")
        qmp = Qmp(sock_path)
        qmp.device_add("usb-mouse", HOTPLUG_ID, "xhci.0")
        if not wait_for(serial_log, MOUSE, 1, proc, args.hotplug_timeout):
            return fail("hotplugged mouse was never enumerated")

        # wb_init() shows the sprite before the desktop exists and does not go
        # through gfx_selftest(), so the base position comes from the "gfx
        # ready" banner instead of the gfx gate's "cursor shown" marker.
        if not wait_for(serial_log, GFX_READY, 1, proc, args.hotplug_timeout):
            return fail("guest never reported a framebuffer cursor")
        shown = last_position(serial_log, GFX_READY_RE, (0, 0))
        press_x = shown[0] + PRESS_MOVE[0]
        press_y = shown[1] + PRESS_MOVE[1]
        print("wb: cursor starts at (%d, %d), pressing the title bar at (%d, %d)"
              % (shown[0], shown[1], press_x, press_y))

        markers = read_log(serial_log).count(CURSOR)
        qmp.move(PRESS_MOVE[0], PRESS_MOVE[1])
        if not wait_for(serial_log, CURSOR, markers + 1, proc, args.event_timeout):
            return fail("guest never reported the pointer move onto the title bar")
        got = last_position(serial_log, CURSOR_RE, (0, 0))
        if abs(got[0] - press_x) > 1 or abs(got[1] - press_y) > 1:
            return fail("cursor at (%d, %d), expected (%d, %d)"
                        % (got[0], got[1], press_x, press_y))

        active = read_log(serial_log).count(ACTIVE)
        qmp.button(True)
        if not wait_for(serial_log, ACTIVE, active + 1, proc, args.event_timeout):
            return fail("button press never activated window 2")
        if DRAG_START not in read_log(serial_log):
            return fail("button press did not start a title-bar drag")

        want_x = press_x
        want_y = press_y
        for index, (dx, dy) in enumerate(DRAG_MOVES):
            want_x += dx
            want_y += dy
            markers = read_log(serial_log).count(CURSOR)
            qmp.move(dx, dy)
            if not wait_for(serial_log, CURSOR, markers + 1, proc,
                            args.event_timeout):
                return fail("guest never reported drag move %d (%+d, %+d)"
                            % (index + 1, dx, dy))
            got = last_position(serial_log, CURSOR_RE, (0, 0))
            if abs(got[0] - want_x) > 1 or abs(got[1] - want_y) > 1:
                return fail("drag move %d: cursor at (%d, %d), expected (%d, %d)"
                            % (index + 1, got[0], got[1], want_x, want_y))
            print("wb: drag move %d (%+d, %+d) -> pointer (%d, %d)"
                  % (index + 1, dx, dy, got[0], got[1]))

        moved = read_log(serial_log).count(MOVED)
        qmp.button(False)
        if not wait_for(serial_log, MOVED, moved + 1, proc, args.event_timeout):
            return fail("button release never finished the drag")
        rect = last_position(serial_log, MOVED_RE, (0, 0))
        expect = (press_x - TITLE_DX + sum(m[0] for m in DRAG_MOVES),
                  press_y - TITLE_DY + sum(m[1] for m in DRAG_MOVES))
        if abs(rect[0] - expect[0]) > 1 or abs(rect[1] - expect[1]) > 1:
            return fail("window 2 moved to (%d, %d), expected (%d, %d)"
                        % (rect[0], rect[1], expect[0], expect[1]))
        bounds = (rect[0], rect[1])

        shot = tmpdir / "desktop.ppm"
        qmp.screendump(shot)

        if not wait_for(serial_log, OK, 1, proc, args.event_timeout):
            return fail("guest never finished the Workbench selftest")
        drags = 1

        # Pixel proof: the active title bar sits at the new window origin and
        # the old title-bar row is back to plain desktop. The sample sits well
        # right of the title text, which is drawn in black and would otherwise
        # be mistaken for a missing title bar.
        width, height, _ = read_ppm(shot)
        new_bar = pixel_at(shot, bounds[0] + 120, bounds[1] + 6)
        old_bar = pixel_at(shot, press_x - TITLE_DX + 120, press_y - TITLE_DY + 6)
        frame = pixel_at(shot, 80, 90)
        print("wb: %dx%d framebuffer, active title bar at (%d, %d) -> %s"
              % (width, height, bounds[0], bounds[1], new_bar))
        if new_bar != TITLE_ACTIVE:
            return fail("no active title bar at (%d, %d): pixel is %s"
                        % (bounds[0] + 120, bounds[1] + 6, new_bar))
        if old_bar == TITLE_ACTIVE:
            return fail("the old title bar is still drawn at (%d, %d)"
                        % (press_x - TITLE_DX + 120, press_y - TITLE_DY + 6))
        if frame != FRAME:
            return fail("window 1 frame pixel (80, 90) is %s" % (frame,))

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

    print("ok: workbench title-bar drag (%d drag(s), window 2 at (%d, %d), exit %d)"
          % (drags, bounds[0], bounds[1], status))
    return 0


if __name__ == "__main__":
    sys.exit(main())
