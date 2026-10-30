#!/usr/bin/env python3
"""Verify the Workbench desktop objects and the window depth gadget (phase 6E).

The driver boots QEMU with an xHCI controller and no USB device, waits for the
Workbench to enter desk mode (wb.desk=1) and then hotplugs a usb-mouse. It
clicks the System: volume icon twice: the first press selects the object, the
second one (inside the double-click window) opens the drawer window that lists
the root volume. A screendump proves the amber selection frame and the drawer
body. The driver then clicks the drawer's depth gadget, which sends the window
to the back: the Shell window becomes active and the drawer title turns idle
grey, which two more screendumps prove before the guest exits.

usage: run-workbench-desk.py --disk IMG --serial-log LOG --exit-code FILE [options]
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
DRAWER_OPEN = b"CRUGOS: wb drawer 'System:' opened"
SENT_BACK = b"CRUGOS: wb window 3 sent to back"
DESK_OK = b"CRUGOS: wb desk selftest ok"
OK = b"CRUGOS: wb selftest ok"
MOUSE_ID = "mouse0"

GFX_READY_RE = re.compile(r"CRUGOS: gfx ready \(\d+x\d+, cursor at (-?\d+),(-?\d+)\)")
CURSOR_RE = re.compile(r"CRUGOS: wb cursor at \((-?\d+), (-?\d+)\)")
DRAWER_RE = re.compile(r"CRUGOS: wb drawer 'System:' opened \(([1-9]\d*) entries\)")

# The System: volume object is a 64x48 tile at (16, 240); its centre is
# (48, 264). The drawer opens at (560, 60) with a 360x260 size, so its depth
# gadget centre is (890, 72) and its close gadget centre is (906, 72).
ICON_CENTRE = (48, 264)
ICON_FRAME = (16, 240)
ICON_INNER = (48, 264)
DEPTH_GADGET = (890, 72)
DRAWER_TITLE = (600, 72)
# (700, 310) lies in the drawer body before the depth click and in the Shell
# window title bar afterwards: the drawer is (560,60)-(920,320) and the Shell
# window is (480,300)-(800,500).
OVERLAP = (700, 310)

FRAME_RGB = (255, 255, 255)
SELECTED_RGB = (255, 176, 0)
IDLE_RGB = (128, 128, 128)
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
    ap.add_argument("--acts", type=int, default=3)
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
        return fail("the scripted sequence performs exactly 3 actions")
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
    tmpdir = Path(tempfile.mkdtemp(prefix=".wbd.",
                                   dir=str(Path(args.disk).resolve().parent)))
    sock_path = str(tmpdir / "qmp.sock")

    # i8042=off removes the PS/2 controller and usb=off the machine USB
    # controllers, so the hotplugged mouse is the only pointer device.
    cmdline = "wb.enable=1 wb.wait=%d wb.desk=1 wb.acts=%d" % (args.wb_wait,
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
    print("wbd: starting " + " ".join(cmd))
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
        print("wbd: cursor starts at (%d, %d)" % shown)

        def move_to(target, timeout):
            # The USB HID boot mouse reports 8-bit deltas, so a long jump is
            # split into several reports and the guest can drop one. Move in
            # <=100 px steps and follow the position the guest reports.
            stall = 0
            for _ in range(200):
                cx, cy = state["x"], state["y"]
                if cx == target[0] and cy == target[1]:
                    print("wbd: pointer at (%d, %d)" % (cx, cy))
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
            print("wbd: " + what)

        # 1. The object is drawn with the plain frame until it is selected.
        shot = tmpdir / "idle.ppm"
        qmp.screendump(shot)
        frame = pixel_at(shot, *ICON_FRAME)
        inner = pixel_at(shot, *ICON_INNER)
        print("wbd: idle icon frame %s, inner %s" % (frame, inner))
        if frame != FRAME_RGB:
            return fail("the volume object frame is %s, expected %s"
                        % (frame, FRAME_RGB))
        if inner != WINDOW_RGB:
            return fail("the volume object inner tile is %s, expected %s"
                        % (inner, WINDOW_RGB))

        # 2. First press selects the object.
        move_to(ICON_CENTRE, args.event_timeout)
        qmp.click()
        expect(SELECTED, "the System: object was selected")
        shot = tmpdir / "selected.ppm"
        qmp.screendump(shot)
        frame = pixel_at(shot, *ICON_FRAME)
        print("wbd: selected icon frame %s" % (frame,))
        if frame != SELECTED_RGB:
            return fail("the selected object frame is %s, expected %s"
                        % (frame, SELECTED_RGB))

        # 3. A second press inside the double-click window opens the drawer.
        qmp.click()
        expect(DRAWER_OPEN, "the drawer opened from the volume object")
        text = read_log(serial_log).decode("utf-8", "replace")
        match = DRAWER_RE.search(text)
        if not match:
            return fail("the drawer did not list any root entry")
        print("wbd: drawer lists %s root entries" % match.group(1))
        shot = tmpdir / "drawer.ppm"
        samples, retries = settle_pixels(qmp, shot, {
            "title": (*DRAWER_TITLE, SELECTED_RGB),
            "overlap": (*OVERLAP, WINDOW_RGB),
        })
        title = samples["title"]
        overlap = samples["overlap"]
        if retries:
            print("wbd: drawer paint settled after %d retr%s"
                  % (retries, "y" if retries == 1 else "ies"))
        print("wbd: drawer title %s, body at overlap %s" % (title, overlap))
        if title != SELECTED_RGB:
            return fail("the open drawer title is %s, expected the active %s"
                        % (title, SELECTED_RGB))
        if overlap != WINDOW_RGB:
            return fail("the drawer body does not cover the overlap: %s"
                        % (overlap,))

        # 4. The depth gadget sends the drawer to the back: the Shell window
        #    becomes active and the drawer title turns idle grey.
        move_to(DEPTH_GADGET, args.event_timeout)
        qmp.click()
        expect(SENT_BACK, "the depth gadget sent the drawer to the back")
        shot = tmpdir / "depth.ppm"
        samples, retries = settle_pixels(qmp, shot, {
            "title": (*DRAWER_TITLE, IDLE_RGB),
            "overlap": (*OVERLAP, SELECTED_RGB),
        })
        title = samples["title"]
        overlap = samples["overlap"]
        if retries:
            print("wbd: depth paint settled after %d retr%s"
                  % (retries, "y" if retries == 1 else "ies"))
        print("wbd: idle drawer title %s, Shell title at overlap %s"
              % (title, overlap))
        if title != IDLE_RGB:
            return fail("the drawer title is %s, expected idle %s"
                        % (title, IDLE_RGB))
        if overlap != SELECTED_RGB:
            return fail("the Shell title bar is %s, expected active %s"
                        % (overlap, SELECTED_RGB))

        if not wait_for(serial_log, DESK_OK, 1, proc, args.event_timeout):
            return fail("guest never finished the desktop object selftest")
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
    if b"dialog" in data:
        return fail("unexpected dialog marker in the serial log")

    print("ok: workbench desktop objects and the depth gadget (exit %d)" % status)
    return 0


if __name__ == "__main__":
    sys.exit(main())
