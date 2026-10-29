#!/usr/bin/env python3
"""Verify the Commodities Exchange (phase 6J).

The driver boots QEMU with an xHCI controller but no USB device and waits for
the Workbench to enter exchange mode (wb.cx=1). It hotplugs a usb-mouse and a usb-kbd, clicks
the Mute row of the manager window to switch that hotkey off, presses F4 to
prove the disabled hotkey is blocked rather than run, clicks the row again to
switch it back on, then presses F4 and F6 so the mute and clock utilities fire.
The guest gates itself through wb.acts= and prints a marker per action; a
screendump proves the manager window is really painted on screen.

usage: run-workbench-cx.py --disk IMG --serial-log LOG --exit-code FILE [options]
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
CX_READY = b"CRUGOS: wb cx ready ("
MOUSE = b"CRUGOS: usb mouse ready"
KEYBOARD = b"CRUGOS: usb keyboard ready"
GFX_READY = b"CRUGOS: gfx ready"
OFF = b"commodity Mute off"
ON = b"commodity Mute on"
BLOCKED = b"CRUGOS: wb commodity Mute blocked (hotkey off)"
MUTED = b"commodity Mute fired (muted)"
CLOCK = b"commodity Clock fired (uptime "
OK = b"CRUGOS: wb selftest ok"
MOUSE_ID = "mouse0"
KBD_ID = "kbd0"

GFX_READY_RE = re.compile(r"CRUGOS: gfx ready \(\d+x\d+, cursor at (-?\d+),(-?\d+)\)")
CURSOR_RE = re.compile(r"CRUGOS: wb cursor at \((-?\d+), (-?\d+)\)")

# The Exchange window is 344x196 at (560, 120): its body starts at (562, 142)
# and row i starts 28 + 26*i pixels below that, so the Mute row (index 2) is
# centred on y = 142 + 28 + 52 + 13 = 235.
BODY_X = 600
MUTE_ROW = (BODY_X, 235)
# Body fill and active title bar, for the screendump proof.
BODY_PIXEL = (700, 250)
BODY_RGB = (8, 16, 24)
TITLE_PIXEL = (600, 130)
TITLE_RGB = (255, 176, 0)


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
    if not data.startswith(b"P6"):
        raise ValueError("not a P6 PPM")
    fields = []
    pos = 2
    while len(fields) < 3:
        while pos < len(data) and data[pos:pos + 1].isspace():
            pos += 1
        start = pos
        while pos < len(data) and not data[pos:pos + 1].isspace():
            pos += 1
        if start == pos:
            raise ValueError("truncated PPM header")
        fields.append(data[start:pos])
    pos += 1
    width = int(fields[0])
    height = int(fields[1])
    if int(fields[2]) != 255:
        raise ValueError("unsupported maxval %r" % fields[2])
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
    ap.add_argument("--acts", type=int, default=4)
    ap.add_argument("--wb-wait", type=int, default=30)
    ap.add_argument("--boot-timeout", type=float, default=60.0)
    ap.add_argument("--hotplug-timeout", type=float, default=30.0)
    ap.add_argument("--event-timeout", type=float, default=15.0)
    ap.add_argument("--run-timeout", type=float, default=240.0)
    ap.add_argument("--qemu-extra", default="-device qemu-xhci,id=xhci")
    args = ap.parse_args()

    if args.acts < 1 or args.acts > 8:
        return fail("--acts must be between 1 and 8")
    if args.acts != 4:
        return fail("the scripted sequence performs exactly 4 actions")
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
    tmpdir = Path(tempfile.mkdtemp(prefix=".wbc.",
                                   dir=str(Path(args.disk).resolve().parent)))
    sock_path = str(tmpdir / "qmp.sock")

    # i8042=off removes the PS/2 controller and usb=off the machine USB
    # controllers, so the hotplugged devices are the only pointer and keyboard.
    cmdline = ("wb.enable=1 wb.wait=%d wb.cx=1 wb.acts=%d"
               % (args.wb_wait, args.acts))
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
    print("wbc: starting " + " ".join(cmd))
    proc = subprocess.Popen(cmd, cwd=args.repo)

    qmp = None
    state = {"x": 0, "y": 0}
    try:
        if not wait_for(serial_log, DESKTOP, 1, proc, args.boot_timeout):
            return fail("guest never reported the Workbench desktop")
        if not wait_for(serial_log, CX_READY, 1, proc, args.boot_timeout):
            return fail("guest never reported the commodities exchange")
        qmp = Qmp(sock_path)
        qmp.device_add("usb-mouse", MOUSE_ID, "xhci.0")
        if not wait_for(serial_log, MOUSE, 1, proc, args.hotplug_timeout):
            return fail("hotplugged mouse was never enumerated")
        qmp.device_add("usb-kbd", KBD_ID, "xhci.0")
        if not wait_for(serial_log, KEYBOARD, 1, proc, args.hotplug_timeout):
            return fail("hotplugged keyboard was never enumerated")
        if not wait_for(serial_log, GFX_READY, 1, proc, args.hotplug_timeout):
            return fail("guest never reported a framebuffer cursor")
        shown = last_position(serial_log, GFX_READY_RE, (0, 0))
        state["x"], state["y"] = shown
        print("wbc: cursor starts at (%d, %d)" % shown)

        def move_to(target, timeout):
            # The USB HID boot mouse reports 8-bit deltas, so a long jump is
            # split into several reports and the guest can drop one. Move in
            # <=100 px steps and follow the position the guest reports.
            stall = 0
            for _ in range(200):
                cx, cy = state["x"], state["y"]
                if cx == target[0] and cy == target[1]:
                    print("wbc: pointer at (%d, %d)" % (cx, cy))
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
            print("wbc: " + what)

        def click(target, needle, what):
            move_to(target, args.event_timeout)
            qmp.button(True)
            time.sleep(0.05)
            qmp.button(False)
            expect(needle, what)

        # 1. Switch the Mute hotkey off with a row click.
        click(MUTE_ROW, OFF, "the Mute row switched the hotkey off")

        # 2. A press while it is off is blocked, not run.
        qmp.send_key("f4")
        expect(BLOCKED, "F4 was refused while the hotkey was off")

        # 3. Switch it back on.
        click(MUTE_ROW, ON, "the Mute row switched the hotkey on")

        # 4. F4 now runs the mute utility, and F6 runs the clock utility.
        qmp.send_key("f4")
        expect(MUTED, "F4 muted the audio mixer")
        qmp.send_key("f6")
        expect(CLOCK, "F6 reported the system uptime")

        shot = tmpdir / "exchange.ppm"
        qmp.screendump(shot)
        body = pixel_at(shot, BODY_PIXEL[0], BODY_PIXEL[1])
        title = pixel_at(shot, TITLE_PIXEL[0], TITLE_PIXEL[1])
        print("wbc: body %s -> %s, title %s -> %s"
              % (BODY_PIXEL, body, TITLE_PIXEL, title))
        if body != BODY_RGB:
            return fail("the Exchange body is %s, expected %s"
                        % (body, BODY_RGB))
        if title != TITLE_RGB:
            return fail("the Exchange title bar is %s, expected the active %s"
                        % (title, TITLE_RGB))

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

    print("ok: workbench commodities exchange (exit %d)" % status)
    return 0


if __name__ == "__main__":
    sys.exit(main())
