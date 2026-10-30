#!/usr/bin/env python3
"""Verify the Workbench menus and gadgets (phase 6B).

The driver boots QEMU with an xHCI controller but no USB device, hotplugs a
usb-mouse over QMP once the guest has opened its windows, and drives a scripted
sequence of clicks: open a menu, hover an item, select it, click a body gadget,
close a window with its close gadget and select another menu item. The guest
gates itself through wb.menu= and wb.acts= and prints a marker for every action;
QEMU screendumps then prove the popup highlight and that the closed window area
went back to the desktop.

usage: run-workbench-menu.py --disk IMG --serial-log LOG --exit-code FILE [options]
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
OK = b"CRUGOS: wb selftest ok"
SHELL_OPEN = b"CRUGOS: wb menu 'Shell' open (window 2)"
SHELL_HOVER = b"CRUGOS: wb menu 'Shell' item 0 highlighted ('New')"
SHELL_PICK = b"CRUGOS: wb menu 'Shell' selected ('New')"
GADGET = b"CRUGOS: wb gadget 'Run' clicked in window 1"
CLOSED = b"CRUGOS: wb window 2 closed (gadget)"
TOP_OPEN = b"CRUGOS: wb menu 'Workbench' open (window 1)"
TOP_PICK = b"CRUGOS: wb menu 'Workbench' selected ('Quit')"
HOTPLUG_ID = "mouse0"

GFX_READY_RE = re.compile(r"CRUGOS: gfx ready \(\d+x\d+, cursor at (-?\d+),(-?\d+)\)")
CURSOR_RE = re.compile(r"CRUGOS: wb cursor at \((-?\d+), (-?\d+)\)")

# Screen geometry of the scripted clicks. Window 2 (Shell) is active at boot, so
# the menu bar shows the Shell strips first: 'Shell' spans x 4..60 and
# 'Workbench' x 60..148. The popup drops at the strip origin with a 2 px top pad
# and 18 px rows, so item N is centred at y = 29 + 18 * N.
MENU_SHELL = (30, 9)
SHELL_ITEM = (79, 29)
GADGET_RUN = (142, 183)
SHELL_CLOSE = (786, 312)
MENU_TOP = (30, 9)
TOP_ITEM = (79, 65)

# Colours as they appear in the PPM: the menu highlight, the popup background,
# the desktop and a window frame.
HIGHLIGHT = (255, 176, 0)
MENU_BG = (192, 192, 192)
DESKTOP_PIXEL = (16, 32, 48)
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


def settle_pixels(qmp, shot, wants, tries=40, delay=0.05):
    """Capture until every wanted pixel matches, and report the retries.

    A screendump requested right after a guest marker can still show the frame
    from before the paint: the host display update is asynchronous, so the
    first capture after an event occasionally lags one repaint.  Retry for a
    bounded time; a pixel that never settles still fails the gate.
    wants: name -> (x, y, expected RGB)."""
    samples = {}
    for attempt in range(tries):
        qmp.screendump(shot)
        samples = {name: pixel_at(shot, x, y)
                   for name, (x, y, _want) in wants.items()}
        if all(samples[name] == want for name, (_x, _y, want) in wants.items()):
            return samples, attempt
        time.sleep(delay)
    return samples, tries - 1

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

    tmpdir = Path(tempfile.mkdtemp(prefix=".wbm.",
                                   dir=str(Path(args.disk).resolve().parent)))
    sock_path = str(tmpdir / "qmp.sock")

    # i8042=off removes the PS/2 controller and usb=off the machine USB
    # controllers, so the hotplugged USB mouse is the only pointer device.
    cmdline = "wb.enable=1 wb.wait=%d wb.menu=1 wb.acts=%d" % (args.wb_wait,
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
    print("wbm: starting " + " ".join(cmd))
    proc = subprocess.Popen(cmd, cwd=args.repo)

    qmp = None
    actions = 0
    state = {"x": 0, "y": 0}
    try:
        if not wait_for(serial_log, DESKTOP, 1, proc, args.boot_timeout):
            return fail("guest never reported the Workbench desktop")
        qmp = Qmp(sock_path)
        qmp.device_add("usb-mouse", HOTPLUG_ID, "xhci.0")
        if not wait_for(serial_log, MOUSE, 1, proc, args.hotplug_timeout):
            return fail("hotplugged mouse was never enumerated")
        if not wait_for(serial_log, GFX_READY, 1, proc, args.hotplug_timeout):
            return fail("guest never reported a framebuffer cursor")
        shown = last_position(serial_log, GFX_READY_RE, (0, 0))
        state["x"], state["y"] = shown
        print("wbm: cursor starts at (%d, %d)" % shown)

        def move_to(target, timeout):
            # The USB HID boot mouse reports 8-bit deltas, so a long jump is
            # split into several reports and the guest can drop one when they
            # arrive faster than it polls. Move in <=100 px steps and follow
            # the position the guest actually reports.
            stall = 0
            for _ in range(200):
                cx, cy = state["x"], state["y"]
                if cx == target[0] and cy == target[1]:
                    print("wbm: pointer at (%d, %d)" % (cx, cy))
                    return
                dx = max(-100, min(100, target[0] - cx))
                dy = max(-100, min(100, target[1] - cy))
                markers = read_log(serial_log).count(CURSOR)
                qmp.move(dx, dy)
                if not wait_for(serial_log, CURSOR, markers + 1, proc, timeout):
                    raise RuntimeError("guest never reported the pointer move to (%d, %d)"
                                       % target)
                got = last_position(serial_log, CURSOR_RE, (cx, cy))
                state["x"], state["y"] = got
                if got == (cx, cy):
                    stall += 1
                    if stall >= 3:
                        raise RuntimeError("pointer stuck at (%d, %d) heading to (%d, %d)"
                                           % (got[0], got[1], target[0], target[1]))
                else:
                    stall = 0
            raise RuntimeError("pointer did not reach (%d, %d)" % target)

        def press(target):
            move_to(target, args.event_timeout)
            qmp.button(True)

        def release():
            qmp.button(False)

        def expect(needle, what):
            if not wait_for(serial_log, needle, 1, proc, args.event_timeout):
                raise RuntimeError("guest never reported " + what)
            print("wbm: " + what)

        # 1. Open the Shell menu and hover its first item.
        press(MENU_SHELL)
        expect(SHELL_OPEN, "the Shell menu opened")
        release()
        move_to(SHELL_ITEM, args.event_timeout)
        expect(SHELL_HOVER, "the first Shell item highlighted")

        shot_open = tmpdir / "menu.ppm"
        samples, retries = settle_pixels(qmp, shot_open, {
            "hover": (130, 29, HIGHLIGHT),
            "next": (130, 47, MENU_BG),
        })
        hover_pixel = samples["hover"]
        next_pixel = samples["next"]
        if retries:
            print("wbm: popup paint settled after %d retr%s"
                  % (retries, "y" if retries == 1 else "ies"))
        print("wbm: popup pixels at (130, 29) -> %s, (130, 47) -> %s"
              % (hover_pixel, next_pixel))
        if hover_pixel != HIGHLIGHT:
            return fail("the hovered item is not highlighted: pixel is %s"
                        % (hover_pixel,))
        if next_pixel != MENU_BG:
            return fail("the next menu row is not plain background: pixel is %s"
                        % (next_pixel,))

        # 2. Select the item.
        press(SHELL_ITEM)
        expect(SHELL_PICK, "the Shell item was selected")
        release()
        actions += 1

        # 3. Click the Run gadget in window 1.
        press(GADGET_RUN)
        release()
        expect(GADGET, "the Run gadget was clicked")
        actions += 1

        # 4. Close window 2 with its close gadget.
        press(SHELL_CLOSE)
        expect(CLOSED, "window 2 was closed by its gadget")
        release()
        actions += 1

        shot_closed = tmpdir / "closed.ppm"
        samples, retries = settle_pixels(qmp, shot_closed, {
            "body": (640, 400, DESKTOP_PIXEL),
            "frame": (80, 90, FRAME),
        })
        old_body = samples["body"]
        frame = samples["frame"]
        if retries:
            print("wbm: close paint settled after %d retr%s"
                  % (retries, "y" if retries == 1 else "ies"))
        print("wbm: closed area (640, 400) -> %s, window 1 frame (80, 90) -> %s"
              % (old_body, frame))
        if old_body != DESKTOP_PIXEL:
            return fail("the closed window area is not the desktop: pixel is %s"
                        % (old_body,))
        if frame != FRAME:
            return fail("window 1 frame pixel (80, 90) is %s" % (frame,))

        # 5. Window 1 is active again: open its first menu and select Quit.
        press(MENU_TOP)
        expect(TOP_OPEN, "the Workbench menu opened")
        release()
        move_to(TOP_ITEM, args.event_timeout)
        press(TOP_ITEM)
        expect(TOP_PICK, "the Workbench item was selected")
        release()
        actions += 1

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
    if actions < 1:
        return fail("no Workbench action was performed")

    print("ok: workbench menu and gadget actions (%d action(s), exit %d)"
          % (actions, status))
    return 0


if __name__ == "__main__":
    sys.exit(main())
