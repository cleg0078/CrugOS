#!/usr/bin/env python3
"""Verify the Workbench drawer selection and its draggable scrollbar (phase 6H).

The driver boots QEMU with an xHCI controller and no USB device, fills the root
volume (wb.fill=), waits for select mode (wb.desk=1, wb.dr=1, wb.dsel=1) and
double clicks the System: volume icon to open the drawer.  A screendump proves
the idle row band; a click on the third row selects it and a second screendump
proves the amber band.  The scrollbar thumb is then dragged down the track: the
view scrolls (serial marker) and a screendump proves the selected band left the
body.  A final click on the first visible row selects index >= 18 of 29, which
only exists after the scroll, and a screendump proves the new amber band.

usage: run-workbench-select.py --disk IMG --serial-log LOG --exit-code FILE [options]
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
MOUSE = b"CRUGOS: usb mouse ready"
GFX_READY = b"CRUGOS: gfx ready"
SELECTED = b"CRUGOS: wb icon 'System:' selected"
FILLED = b"CRUGOS: wb fill "
DRAWER_OPEN = b"CRUGOS: wb drawer 'System:' opened"
DRAWER_READY = b"CRUGOS: wb drawer ready"
SELECT_MODE = b"CRUGOS: wb select mode"
ROW_SELECTED = b"CRUGOS: wb drawer selected"
GRABBED = b"CRUGOS: wb drawer scrollbar grabbed"
SCROLLBAR_TO = b"CRUGOS: wb drawer scrollbar to (top "
SELECT_OK = b"CRUGOS: wb select selftest ok"
OK = b"CRUGOS: wb selftest ok"
MOUSE_ID = "mouse0"

GFX_READY_RE = re.compile(r"CRUGOS: gfx ready \(\d+x\d+, cursor at (-?\d+),(-?\d+)\)")
CURSOR_RE = re.compile(r"CRUGOS: wb cursor at \((-?\d+), (-?\d+)\)")
READY_RE = re.compile(r"CRUGOS: wb drawer ready \(([1-9]\d*) entries, ([1-9]\d*) row\(s\)\)")
SELECTED_RE = re.compile(r"CRUGOS: wb drawer selected '[^']+' \(index ([0-9]+) of ([0-9]+)\)")
TO_RE = re.compile(r"CRUGOS: wb drawer scrollbar to \(top ([0-9]+) of ([0-9]+)\)")

# The System: volume object is a 64x48 tile at (16, 240); its centre is
# (48, 264).  The drawer opens at (560, 60) with a 360x260 size, so its body is
# x 562..918, y 82..318.  Rows are drawn at body y 6 + 20*i, so screen row 0 is
# 88..108 and row 2 is 128..148; x=890 is inside the row band and clear of the
# glyphs.  The scrollbar track is body x = 356-8 = 348, i.e. screen 910..914.
ICON_CENTRE = (48, 264)
ROW0_PROBE = (890, 96)
ROW2_PROBE = (890, 136)
ROW2_CLICK = (600, 136)
ROW0_CLICK = (600, 96)
TRACK_GRAB = (911, 100)
TRACK_END = (911, 280)

SELECTED_RGB = (255, 176, 0)
WINDOW_RGB = (8, 16, 24)


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

    def send_key(self, name):
        self._cmd({"execute": "send-key",
                   "arguments": {"keys": [{"type": "qcode",
                                           "data": name}],
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


def changed_pixels(first, second, box):
    """Count differing pixels inside a box between two PPM screendumps."""
    aw, ah, a = read_ppm(first)
    bw, bh, b = read_ppm(second)
    if (aw, ah) != (bw, bh):
        raise ValueError("screendumps differ in size")
    x0, y0, x1, y1 = box
    count = 0
    for y in range(max(0, y0), min(ah, y1)):
        row = y * aw
        for x in range(max(0, x0), min(aw, x1)):
            off = (row + x) * 3
            if a[off:off + 3] != b[off:off + 3]:
                count += 1
    return count


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
    ap.add_argument("--acts", type=int, default=3)
    ap.add_argument("--fill", type=int, default=24)
    ap.add_argument("--wb-wait", type=int, default=30)
    ap.add_argument("--boot-timeout", type=float, default=60.0)
    ap.add_argument("--hotplug-timeout", type=float, default=30.0)
    ap.add_argument("--event-timeout", type=float, default=15.0)
    ap.add_argument("--run-timeout", type=float, default=240.0)
    ap.add_argument("--qemu-extra", default="-device qemu-xhci,id=xhci")
    args = ap.parse_args()

    if args.acts < 1 or args.acts > 8:
        return fail("--acts must be between 1 and 8")
    if args.acts != 3:
        return fail("the scripted sequence performs exactly 3 drawer operations")
    if args.fill < 0 or args.fill > 24:
        return fail("--fill must be between 0 and 24")
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
    tmpdir = Path(tempfile.mkdtemp(prefix=".wbsel.",
                                   dir=str(Path(args.disk).resolve().parent)))
    sock_path = str(tmpdir / "qmp.sock")

    cmdline = ("wb.enable=1 wb.wait=%d wb.desk=1 wb.dr=1 wb.dsel=1 wb.fill=%d "
               "wb.acts=%d" % (args.wb_wait, args.fill, args.acts))
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
    print("wbsel: starting " + " ".join(cmd))
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
        if not wait_for(serial_log, ICON_READY, 1, proc, args.hotplug_timeout):
            return fail("guest never put the System: object on the desktop")
        if not wait_for(serial_log, GFX_READY, 1, proc, args.hotplug_timeout):
            return fail("guest never reported a framebuffer cursor")
        shown = last_position(serial_log, GFX_READY_RE, (0, 0))
        state["x"], state["y"] = shown
        print("wbsel: cursor starts at (%d, %d)" % shown)

        def move_to(target, timeout):
            # USB HID boot mice send 8-bit deltas, so long jumps are split into
            # several reports and the guest can drop one.  Move in <=100 px steps
            # and follow the position the guest reports.
            stall = 0
            for _ in range(200):
                cx, cy = state["x"], state["y"]
                if cx == target[0] and cy == target[1]:
                    print("wbsel: pointer at (%d, %d)" % (cx, cy))
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
            print("wbsel: " + what)

        # 1. Double click the volume object to open the filled drawer.
        move_to(ICON_CENTRE, args.event_timeout)
        qmp.click()
        expect(SELECTED, "the System: object was selected")
        qmp.click()
        expect(DRAWER_OPEN, "the drawer opened from the volume object")
        expect(DRAWER_READY, "the drawer reported its visible rows")
        text = read_log(serial_log).decode("utf-8", "replace")
        match = READY_RE.search(text)
        if not match:
            return fail("the drawer never reported entries and rows")
        entries = int(match.group(1))
        rows = int(match.group(2))
        print("wbsel: drawer holds %d entries, shows %d row(s)" % (entries, rows))
        if entries < 25:
            return fail("the filled drawer only holds %d entries" % entries)
        if rows != 11:
            return fail("the 236 px body shows %d rows, expected 11" % rows)
        if not wait_for(serial_log, FILLED, 1, proc, args.event_timeout):
            return fail("guest never filled the volume")
        expect(SELECT_MODE, "the guest reported select mode")

        shot_idle = tmpdir / "select-0.ppm"
        qmp.screendump(shot_idle)
        idle_row0 = pixel_at(shot_idle, *ROW0_PROBE)
        print("wbsel: idle row 0 pixel %s" % (idle_row0,))
        if idle_row0 != WINDOW_RGB:
            return fail("row 0 is %s before any click, expected the body %s"
                        % (idle_row0, WINDOW_RGB))

        # 2. Click the third visible row: it becomes the selected band (amber).
        move_to(ROW2_CLICK, args.event_timeout)
        qmp.click()
        expect(ROW_SELECTED, "the third row was selected")
        shot_selected = tmpdir / "select-1.ppm"
        qmp.screendump(shot_selected)
        band = pixel_at(shot_selected, *ROW2_PROBE)
        above = pixel_at(shot_selected, *ROW0_PROBE)
        print("wbsel: selected band %s, row 0 %s" % (band, above))
        if band != SELECTED_RGB:
            return fail("the selected band is %s, expected amber %s"
                        % (band, SELECTED_RGB))
        if above != WINDOW_RGB:
            return fail("row 0 became %s, expected only one amber band" % (above,))
        text = read_log(serial_log).decode("utf-8", "replace")
        match = SELECTED_RE.search(text)
        if not match:
            return fail("the selection marker carries no row index")
        first_index = int(match.group(1))
        print("wbsel: first selection is index %d of %s"
              % (first_index, match.group(2)))
        if first_index != 2:
            return fail("the third visible row selected index %d, expected 2"
                        % first_index)

        # 3. Drag the scrollbar thumb down the track: the view scrolls and the
        #    amber band of index 2 leaves the visible rows.
        move_to(TRACK_GRAB, args.event_timeout)
        qmp.button(True)
        expect(GRABBED, "the scrollbar thumb was grabbed")
        move_to(TRACK_END, args.event_timeout)
        qmp.button(False)
        expect(SCROLLBAR_TO, "the scrollbar drag moved the view")
        text = read_log(serial_log).decode("utf-8", "replace")
        match = TO_RE.search(text)
        if not match:
            return fail("the scrollbar marker carries no top row")
        top = int(match.group(1))
        print("wbsel: the scrollbar scrolled to top row %d of %s"
              % (top, match.group(2)))
        if top < 1:
            return fail("the scrollbar drag left the view at the top")
        shot_scrolled = tmpdir / "select-2.ppm"
        qmp.screendump(shot_scrolled)
        scrolled_row2 = pixel_at(shot_scrolled, *ROW2_PROBE)
        print("wbsel: row 2 pixel after the scroll %s" % (scrolled_row2,))
        if scrolled_row2 == SELECTED_RGB:
            return fail("the selected band stayed in the body after scrolling")

        # 4. Click the first visible row: it selects an index that only exists
        #    once the view has scrolled, and the amber band covers row 0.
        move_to(ROW0_CLICK, args.event_timeout)
        qmp.click()
        expect(ROW_SELECTED, "the first visible row was selected", 2)
        text = read_log(serial_log).decode("utf-8", "replace")
        matches = SELECTED_RE.findall(text)
        if len(matches) < 2:
            return fail("the guest reported %d selection(s), expected 2"
                        % len(matches))
        second_index = int(matches[1][0])
        print("wbsel: second selection is index %d of %s" % (second_index, matches[1][1]))
        if second_index < top:
            return fail("the first visible row selected index %d, below the top %d"
                        % (second_index, top))
        shot_second = tmpdir / "select-3.ppm"
        qmp.screendump(shot_second)
        new_band = pixel_at(shot_second, *ROW0_PROBE)
        print("wbsel: new selected band %s" % (new_band,))
        if new_band != SELECTED_RGB:
            return fail("the new band is %s, expected amber %s"
                        % (new_band, SELECTED_RGB))

        expect(SELECT_OK, "guest finished the select selftest")
        expect(OK, "guest finished the Workbench selftest")

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

    print("ok: workbench drawer selection and scrollbar dragging (exit %d)" % status)
    return 0


if __name__ == "__main__":
    sys.exit(main())
