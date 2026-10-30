#!/usr/bin/env python3
"""Verify the Workbench screens and a window moving between them (phase 6I).

The driver boots QEMU with an xHCI controller and no USB device, waits for
screens mode (wb.screens=1) and hotplugs a mouse.  A click on the Tools box in
the menu bar swaps the front screen; a screendump proves the Workbench screen
is gone and the Tools screen paints its own background.  A click on the Tools
window's body gadget moves the window back to the Workbench screen; a second
Screendump proves the Tools screen is now empty.  A last switch draws the
Workbench screen again with the moved window as its active window.

usage: run-workbench-screens.py --disk IMG --serial-log LOG --exit-code FILE [options]
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
SCREENS_READY = b"CRUGOS: wb screens ready"
SWITCH1 = b"CRUGOS: wb screen 1 front"
SWITCH2 = b"CRUGOS: wb screen 0 front"
ACTION1 = b"CRUGOS: wb action 1 screen 1 front"
ACTION2 = b"CRUGOS: wb action 2 gadget window"
ACTION3 = b"CRUGOS: wb action 3 screen-move window"
ACTION4 = b"CRUGOS: wb action 4 screen 0 front"
GADGET = b"CRUGOS: wb gadget 'Send to Workbench' clicked in window"
SCREENS_OK = b"CRUGOS: wb screens selftest ok"
OK = b"CRUGOS: wb selftest ok"
MOUSE_ID = "mouse0"

GFX_READY_RE = re.compile(r"CRUGOS: gfx ready \(\d+x\d+, cursor at (-?\d+),(-?\d+)\)")
CURSOR_RE = re.compile(r"CRUGOS: wb cursor at \((-?\d+), (-?\d+)\)")
DESKTOP_RE = re.compile(r"CRUGOS: wb desktop ready \(([0-9]+)x([0-9]+), ([0-9]+) window\(s\)\)")
READY_RE = re.compile(r"CRUGOS: wb screens ready \(([0-9]+) screen\(s\), front ([0-9]+)\)")
SWITCH_RE = re.compile(r"CRUGOS: wb screen ([0-9]+) front \(([0-9]+) window\(s\)\)")
GADGET_RE = re.compile(r"CRUGOS: wb gadget 'Send to Workbench' clicked in window ([0-9]+)")
MOVE_RE = re.compile(r"CRUGOS: wb window ([0-9]+) moved to screen ([0-9]+)")
SELFTEST_RE = re.compile(r"CRUGOS: wb screens selftest ok \(([0-9]+) switch\(es\), ([0-9]+) move\(s\)\)")

# The screen boxes are right aligned in the menu bar and their width follows the
# framebuffer, so the strip position is computed from the desktop size the guest
# reports (the OVMF GOP mode is not fixed).  A box is name_length * 8 + 12 px
# wide: "Workbench" is 84 px, "Tools" is 52 px, and the strip keeps a 6 px pad.
WORK_BOX_W = 84
TOOLS_BOX_W = 52
SCREEN_PAD = 6
# The Tools window opens at (160, 140) with a 340x200 size (border 2, title 20):
# its body is x 162..498, y 162..338, and the body-relative gadget
# (12, 60, 160, 22) is screen (174, 222)-(334, 244), centre (254, 233).
TOOLS_GADGET = (254, 233)

# Probes: background of each screen, the window body fill and the active title.
SCREEN0_BG = (500, 600)
SCREEN0_BG2 = (20, 600)
SCREEN1_BG = (500, 400)
SCREEN1_BG2 = (100, 100)
WINDOW_BODY = (200, 300)
WINDOW_TITLE = (300, 150)

WORK_RGB = (16, 32, 48)
TOOLS_RGB = (32, 48, 16)
WINDOW_RGB = (8, 16, 24)
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
    """A minimal QMP client: device_add, pointer events and screendump."""

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
    ap.add_argument("--acts", type=int, default=4)
    ap.add_argument("--wb-wait", type=int, default=30)
    ap.add_argument("--boot-timeout", type=float, default=60.0)
    ap.add_argument("--hotplug-timeout", type=float, default=30.0)
    ap.add_argument("--event-timeout", type=float, default=15.0)
    ap.add_argument("--run-timeout", type=float, default=240.0)
    ap.add_argument("--qemu-extra", default="-device qemu-xhci,id=xhci")
    args = ap.parse_args()

    if args.acts != 4:
        return fail("the scripted sequence performs exactly 4 screen actions")
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
    tmpdir = Path(tempfile.mkdtemp(prefix=".wbscreens.",
                                   dir=str(Path(args.disk).resolve().parent)))
    sock_path = str(tmpdir / "qmp.sock")

    cmdline = "wb.enable=1 wb.wait=%d wb.screens=1 wb.acts=%d" % (
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
        "CMDLINE_EXTRA=" + cmdline + " wb.theme=classic",
        "TIMEOUT_SEC=" + str(int(args.run_timeout)),
    ]
    print("wbscreens: starting " + " ".join(cmd))
    proc = subprocess.Popen(cmd, cwd=args.repo)

    qmp = None
    state = {"x": 0, "y": 0}
    try:
        if not wait_for(serial_log, DESKTOP, 1, proc, args.boot_timeout):
            return fail("guest never reported the Workbench desktop")
        text = read_log(serial_log).decode("utf-8", "replace")
        desktop = DESKTOP_RE.search(text)
        if not desktop:
            return fail("the desktop marker carries no size")
        width = int(desktop.group(1))
        strip_x = width - WORK_BOX_W - TOOLS_BOX_W - SCREEN_PAD
        work_box = (strip_x + WORK_BOX_W // 2, 9)
        tools_box = (strip_x + WORK_BOX_W + TOOLS_BOX_W // 2, 9)
        print("wbscreens: desktop %sx%s, strip at %d, Tools box %s"
              % (desktop.group(1), desktop.group(2), strip_x, (tools_box,)))
        qmp = Qmp(sock_path)
        qmp.device_add("usb-mouse", MOUSE_ID, "xhci.0")
        if not wait_for(serial_log, MOUSE, 1, proc, args.hotplug_timeout):
            return fail("hotplugged mouse was never enumerated")
        if not wait_for(serial_log, SCREENS_READY, 1, proc, args.hotplug_timeout):
            return fail("guest never reported the second screen")
        text = read_log(serial_log).decode("utf-8", "replace")
        match = READY_RE.search(text)
        if not match:
            return fail("the screens marker carries no screen count")
        count = int(match.group(1))
        front = int(match.group(2))
        print("wbscreens: %d screen(s), front %d" % (count, front))
        if count != 2:
            return fail("screens mode created %d screen(s), expected 2" % count)
        if front != 0:
            return fail("the Workbench screen is not front (%d)" % front)
        if not wait_for(serial_log, GFX_READY, 1, proc, args.hotplug_timeout):
            return fail("guest never reported a framebuffer cursor")
        shown = last_position(serial_log, GFX_READY_RE, (0, 0))
        state["x"], state["y"] = shown
        print("wbscreens: cursor starts at (%d, %d)" % shown)

        def move_to(target, timeout):
            # USB HID boot mice send 8-bit deltas, so long jumps are split into
            # several reports and the guest can drop one.  Move in <=100 px steps
            # and follow the position the guest reports.
            stall = 0
            for _ in range(200):
                cx, cy = state["x"], state["y"]
                if cx == target[0] and cy == target[1]:
                    print("wbscreens: pointer at (%d, %d)" % (cx, cy))
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
            print("wbscreens: " + what)

        def check_shot(name, probes):
            """Screendump until every probe matches; retries beat a repaint race."""
            last = ""
            for _ in range(20):
                shot = tmpdir / (name + ".ppm")
                qmp.screendump(shot)
                misses = []
                hits = []
                for point, want, label in probes:
                    got = pixel_at(shot, *point)
                    hits.append((label, got))
                    if got != want:
                        misses.append("%s is %s at (%d, %d), expected %s"
                                      % (label, (got,), point[0], point[1],
                                         (want,)))
                if not misses:
                    print("wbscreens: " + name + " matches")
                    for label, got in hits:
                        print("wbscreens: %s pixel %s" % (label, (got,)))
                    return
                last = "; ".join(misses)
                time.sleep(0.1)
            raise RuntimeError(name + ": " + last)

        # 1. Swap the front screen: the Tools screen paints its own background
        #    and the Workbench windows are not drawn at all.
        move_to(tools_box, args.event_timeout)
        qmp.click()
        expect(SWITCH1, "the Tools screen came to the front")
        expect(ACTION1, "the screen switch reached the action queue")
        check_shot("screens-tools", [
            (SCREEN1_BG, TOOLS_RGB, "the Tools screen background"),
            (SCREEN1_BG2, TOOLS_RGB, "the Workbench window corner"),
            (WINDOW_BODY, WINDOW_RGB, "the Tools window body"),
            (WINDOW_TITLE, TITLE_RGB, "the Tools window title"),
        ])
        text = read_log(serial_log).decode("utf-8", "replace")
        switches = SWITCH_RE.findall(text)
        if not switches:
            return fail("the screen marker carries no window count")
        if switches[-1][0] != "1" or switches[-1][1] != "1":
            return fail("the Tools screen holds %s window(s), expected 1"
                        % switches[-1][1])

        # 2. Click the only gadget in the Tools window: it moves that window
        #    back to the Workbench screen, leaving the Tools screen empty.
        move_to(TOOLS_GADGET, args.event_timeout)
        qmp.click()
        expect(GADGET, "the Send to Workbench gadget was pressed")
        expect(ACTION2, "the gadget reached the action queue")
        expect(ACTION3, "the screen move reached the action queue")
        text = read_log(serial_log).decode("utf-8", "replace")
        match = GADGET_RE.search(text)
        if not match:
            return fail("the gadget marker carries no window id")
        tools_id = int(match.group(1))
        moved = MOVE_RE.findall(text)
        if not moved:
            return fail("the move marker carries no window id")
        print("wbscreens: window %d moved to screen %s" % (int(moved[-1][0]),
                                                           moved[-1][1]))
        if int(moved[-1][0]) != tools_id:
            return fail("window %s moved, expected the Tools window %d"
                        % (moved[-1][0], tools_id))
        if moved[-1][1] != "0":
            return fail("the Tools window moved to screen %s, expected 0"
                        % moved[-1][1])
        check_shot("screens-left", [
            (WINDOW_BODY, TOOLS_RGB, "the Tools screen after the move"),
            (SCREEN1_BG, TOOLS_RGB, "the Tools screen background"),
        ])

        # 3. Swap back: the Workbench screen is drawn again and the moved window
        #    is its topmost window, so its title bar is the active one.
        move_to(work_box, args.event_timeout)
        qmp.click()
        expect(SWITCH2, "the Workbench screen came back to the front")
        expect(ACTION4, "the second switch reached the action queue")
        check_shot("screens-work", [
            (SCREEN0_BG, WORK_RGB, "the Workbench screen background"),
            (SCREEN0_BG2, WORK_RGB, "the left edge of the desktop"),
            (WINDOW_TITLE, TITLE_RGB, "the moved window title"),
            (WINDOW_BODY, WINDOW_RGB, "the moved window body"),
        ])
        text = read_log(serial_log).decode("utf-8", "replace")
        switches = SWITCH_RE.findall(text)
        # The Workbench screen starts with two windows (Workbench and Shell),
        # and the Tools window moved here, so three windows land on it.
        if switches[-1][0] != "0" or switches[-1][1] != "3":
            return fail("the Workbench screen holds %s window(s), expected 3"
                        % switches[-1][1])

        # 4. The selftest counters agree with the scripted switches and move.
        expect(SCREENS_OK, "guest finished the screens selftest")
        expect(OK, "guest finished the Workbench selftest")
        text = read_log(serial_log).decode("utf-8", "replace")
        match = SELFTEST_RE.search(text)
        if not match:
            return fail("the screens selftest marker carries no counters")
        print("wbscreens: selftest counted %s switch(es), %s move(s)"
              % (match.group(1), match.group(2)))
        if int(match.group(1)) < 2:
            return fail("the selftest counted %s screen switch(es), expected 2"
                        % match.group(1))
        if int(match.group(2)) < 1:
            return fail("the selftest counted %s screen move(s), expected 1"
                        % match.group(2))

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
    if b"dialog" in data:
        return fail("unexpected dialog marker in the serial log")

    print("ok: workbench screens and a window moving between them (exit %d)" % status)
    return 0


if __name__ == "__main__":
    sys.exit(main())
