#!/usr/bin/env python3
"""Verify the Workbench keyboard navigation and window resizing (phase 6D).

The driver boots QEMU with an xHCI controller but no USB device and waits for
the Workbench to enter keys mode (wb.keys=1). It hotplugs a usb-mouse, drags
the Shell window's resize grip to shrink it, then hotplugs a usb-kbd and uses
Tab, the arrow keys and the menu popup: Tab focuses window 1, two Right keys
move it by 16 px, and Enter/Down/Enter opens the Workbench menu, highlights
Info and selects it. The guest gates itself through wb.acts= and prints a
marker per action; a screendump proves the area the shrink left went back to
the desktop.

usage: run-workbench-keys.py --disk IMG --serial-log LOG --exit-code FILE [options]
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
KEYS_READY = b"CRUGOS: wb keys ready (window 2 active)"
MOUSE = b"CRUGOS: usb mouse ready"
KEYBOARD = b"CRUGOS: usb keyboard ready"
GFX_READY = b"CRUGOS: gfx ready"
RESIZED = b"CRUGOS: wb window 2 resized to 260x140"
FOCUS = b"CRUGOS: wb window 1 focused (Tab)"
MOVE = b"CRUGOS: wb window 1 key move to (96, 90)"
MENU_OPEN = b"CRUGOS: wb menu 'Workbench' open (window 1)"
ITEM_HL = b"CRUGOS: wb menu 'Workbench' item 1 highlighted ('Info')"
SELECTED = b"CRUGOS: wb menu 'Workbench' selected ('Info')"
OK = b"CRUGOS: wb selftest ok"
MOUSE_ID = "mouse0"
KBD_ID = "kbd0"

GFX_READY_RE = re.compile(r"CRUGOS: gfx ready \(\d+x\d+, cursor at (-?\d+),(-?\d+)\)")
CURSOR_RE = re.compile(r"CRUGOS: wb cursor at \((-?\d+), (-?\d+)\)")

# Window 2 (Shell) is 320x200 at (480, 300) at boot; its resize grip is the
# 12x12 box in the inner bottom-right corner, so its centre is (790, 490).
# Dragging it by (-60, -60) leaves a 260x140 window.
GRIP = (790, 490)
GRIP_END = (730, 430)
# The old bottom-right corner of window 2 is desktop again after the shrink.
OLD_CORNER = (798, 498)
DESKTOP_PIXEL = (16, 32, 48)


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
    """A minimal QMP client: device_add, pointer events, key events, screendump."""

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

    def send_key(self, name):
        self._cmd({"execute": "send-key",
                   "arguments": {"keys": [{"type": "qcode", "data": name}],
                                 "hold-time": 40}})

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
    ap.add_argument("--acts", type=int, default=2)
    ap.add_argument("--wb-wait", type=int, default=30)
    ap.add_argument("--boot-timeout", type=float, default=60.0)
    ap.add_argument("--hotplug-timeout", type=float, default=30.0)
    ap.add_argument("--event-timeout", type=float, default=15.0)
    ap.add_argument("--run-timeout", type=float, default=240.0)
    ap.add_argument("--qemu-extra", default="-device qemu-xhci,id=xhci")
    args = ap.parse_args()

    if args.acts < 1 or args.acts > 8:
        return fail("--acts must be between 1 and 8")
    if args.acts != 2:
        return fail("the scripted sequence performs exactly 2 actions")
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
    tmpdir = Path(tempfile.mkdtemp(prefix=".wbk.",
                                   dir=str(Path(args.disk).resolve().parent)))
    sock_path = str(tmpdir / "qmp.sock")

    # i8042=off removes the PS/2 controller and usb=off the machine USB
    # controllers, so the hotplugged devices are the only pointer and keyboard.
    cmdline = "wb.enable=1 wb.wait=%d wb.keys=1 wb.acts=%d" % (args.wb_wait,
                                                             args.acts)
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
    print("wbk: starting " + " ".join(cmd))
    proc = subprocess.Popen(cmd, cwd=args.repo)

    qmp = None
    state = {"x": 0, "y": 0}
    try:
        if not wait_for(serial_log, DESKTOP, 1, proc, args.boot_timeout):
            return fail("guest never reported the Workbench desktop")
        qmp = Qmp(sock_path)
        qmp.device_add("usb-mouse", MOUSE_ID, "xhci.0")
        if not wait_for(serial_log, MOUSE, 1, proc, args.hotplug_timeout):
            return fail("hotplugged mouse was never enumerated")
        if not wait_for(serial_log, GFX_READY, 1, proc, args.hotplug_timeout):
            return fail("guest never reported a framebuffer cursor")
        shown = last_position(serial_log, GFX_READY_RE, (0, 0))
        state["x"], state["y"] = shown
        print("wbk: cursor starts at (%d, %d)" % shown)

        def move_to(target, timeout):
            # The USB HID boot mouse reports 8-bit deltas, so a long jump is
            # split into several reports and the guest can drop one. Move in
            # <=100 px steps and follow the position the guest reports.
            stall = 0
            for _ in range(200):
                cx, cy = state["x"], state["y"]
                if cx == target[0] and cy == target[1]:
                    print("wbk: pointer at (%d, %d)" % (cx, cy))
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

        def expect(needle, what):
            if not wait_for(serial_log, needle, 1, proc, args.event_timeout):
                raise RuntimeError("guest never reported " + what)
            print("wbk: " + what)

        # 1. Shrink window 2 by dragging its resize grip.
        move_to(GRIP, args.event_timeout)
        qmp.button(True)
        expect(b"CRUGOS: wb window 2 resize start (790, 490)",
               "the resize grip was pressed")
        move_to(GRIP_END, args.event_timeout)
        qmp.button(False)
        expect(RESIZED, "window 2 shrank to 260x140")

        shot = tmpdir / "resized.ppm"
        qmp.screendump(shot)
        corner = pixel_at(shot, *OLD_CORNER)
        print("wbk: old corner %s -> %s" % (OLD_CORNER, corner))
        if corner != DESKTOP_PIXEL:
            return fail("the shrunk window still covers its old corner: pixel is %s"
                        % (corner,))

        # 2. Hotplug a keyboard and drive the desktop with it.
        qmp.device_add("usb-kbd", KBD_ID, "xhci.0")
        expect(KEYBOARD, "the hotplugged keyboard enumerated")
        qmp.send_key("tab")
        expect(FOCUS, "Tab focused window 1")
        qmp.send_key("right")
        time.sleep(0.06)
        qmp.send_key("right")
        expect(MOVE, "two Right keys moved window 1 by 16 px")
        qmp.send_key("ret")
        expect(MENU_OPEN, "Enter opened the Workbench menu")
        qmp.send_key("down")
        expect(ITEM_HL, "Down highlighted Info")
        qmp.send_key("ret")
        expect(SELECTED, "Enter selected Info")

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

    print("ok: workbench resize and keyboard navigation (exit %d)" % status)
    return 0


if __name__ == "__main__":
    sys.exit(main())
