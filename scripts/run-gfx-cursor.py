#!/usr/bin/env python3
"""Verify the pointer-driven framebuffer cursor (phase 5D).

The driver boots QEMU with an xHCI controller but no USB device, hotplugs a
usb-mouse over QMP once the guest reports the controller, and moves it with
input-send-event relative events. The guest gates itself through gfx.wait= and
gfx.moves=; every move is checked twice: the guest prints a marker with the
cursor coordinates, and a QEMU screendump is parsed to prove the amber sprite
actually moved in the framebuffer pixels by the injected delta.

usage: run-gfx-cursor.py --disk IMG --serial-log LOG --exit-code FILE [options]
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

CONTROLLER = b"CRUGOS: usb controller ready"
MOUSE = b"CRUGOS: usb mouse ready"
SHOWN = b"CRUGOS: gfx cursor shown at"
AT = b"CRUGOS: gfx cursor at ("
OK = b"CRUGOS: gfx selftest ok"
HOTPLUG_ID = "mouse0"

SHOWN_RE = re.compile(r"CRUGOS: gfx cursor shown at \((-?\d+), (-?\d+)\)")
AT_RE = re.compile(r"CRUGOS: gfx cursor at \((-?\d+), (-?\d+)\)")

# Amber (#FFD040) is the cursor body colour; the white one-pixel outline is
# deliberately not used because console text is a light grey.
AMBER = (255, 208, 64)
AMBER_MIN_PIXELS = 20
CENTROID_TOLERANCE = 2.0

# Relative moves, cycled when --moves is larger than the table.
MOVES = [(60, 40), (-25, -10), (100, -70), (12, 9), (-40, 25)]


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


def amber_centroid(path):
    width, height, pixels = read_ppm(path)
    total = 0
    sum_x = 0
    sum_y = 0
    for y in range(height):
        row = y * width * 3
        for x in range(width):
            off = row + x * 3
            if (pixels[off], pixels[off + 1], pixels[off + 2]) == AMBER:
                total += 1
                sum_x += x
                sum_y += y
    if total == 0:
        return 0, 0.0, 0.0, width, height
    return total, sum_x / total, sum_y / total, width, height


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--make", default="make")
    ap.add_argument("--repo", default=str(Path(__file__).resolve().parent.parent))
    ap.add_argument("--disk", required=True)
    ap.add_argument("--serial-log", required=True)
    ap.add_argument("--exit-code", required=True)
    ap.add_argument("--moves", type=int, default=3)
    ap.add_argument("--gfx-wait", type=int, default=40)
    ap.add_argument("--controller-timeout", type=float, default=60.0)
    ap.add_argument("--hotplug-timeout", type=float, default=30.0)
    ap.add_argument("--event-timeout", type=float, default=15.0)
    ap.add_argument("--run-timeout", type=float, default=240.0)
    ap.add_argument("--qemu-extra", default="-device qemu-xhci,id=xhci")
    args = ap.parse_args()

    if args.moves < 1 or args.moves > 16:
        return fail("--moves must be between 1 and 16")
    if args.gfx_wait < 1 or args.gfx_wait > 120:
        return fail("--gfx-wait must be between 1 and 120")

    serial_log = Path(args.serial_log).resolve()
    serial_log.parent.mkdir(parents=True, exist_ok=True)
    serial_log.write_bytes(b"")
    exit_code_path = Path(args.exit_code).resolve()
    exit_code_path.parent.mkdir(parents=True, exist_ok=True)
    try:
        exit_code_path.unlink()
    except OSError:
        pass

    tmpdir = Path(tempfile.mkdtemp(prefix=".gfx.",
                                   dir=str(Path(args.disk).resolve().parent)))
    sock_path = str(tmpdir / "qmp.sock")

    # i8042=off removes the PS/2 controller and usb=off the machine USB
    # controllers, so the hotplugged USB mouse is the only pointer device.
    cmdline = "gfx.wait=%d gfx.moves=%d" % (args.gfx_wait, args.moves)
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
    print("gfx: starting " + " ".join(cmd))
    proc = subprocess.Popen(cmd, cwd=args.repo)

    qmp = None
    checked_moves = 0
    try:
        if not wait_for(serial_log, CONTROLLER, 1, proc, args.controller_timeout):
            return fail("guest never reported the xHCI controller")
        qmp = Qmp(sock_path)
        qmp.device_add("usb-mouse", HOTPLUG_ID, "xhci.0")
        if not wait_for(serial_log, MOUSE, 1, proc, args.hotplug_timeout):
            return fail("hotplugged mouse was never enumerated")
        if not wait_for(serial_log, SHOWN, 1, proc, args.hotplug_timeout):
            return fail("guest never showed the gfx cursor")

        text = read_log(serial_log).decode("utf-8", "replace")
        shown = SHOWN_RE.search(text)
        if not shown:
            return fail("could not parse the cursor start position")
        base_x = int(shown.group(1))
        base_y = int(shown.group(2))
        print("gfx: cursor starts at (%d, %d)" % (base_x, base_y))

        time.sleep(0.2)
        before = tmpdir / "shot-000.ppm"
        qmp.screendump(before)
        count, cx, cy, width, height = amber_centroid(before)
        if count < AMBER_MIN_PIXELS:
            return fail("baseline screendump has %d amber pixel(s) at %dx%d"
                        % (count, width, height))
        print("gfx: baseline %d amber pixel(s), centroid (%.1f, %.1f)"
              % (count, cx, cy))

        want_x = base_x
        want_y = base_y
        for index in range(args.moves):
            dx, dy = MOVES[index % len(MOVES)]
            want_x += dx
            want_y += dy
            marker_count = read_log(serial_log).count(AT)
            qmp.move(dx, dy)
            if not wait_for(serial_log, AT, marker_count + 1, proc,
                            args.event_timeout):
                return fail("guest never reported move %d (%+d, %+d)"
                            % (index + 1, dx, dy))
            time.sleep(0.2)
            shot = tmpdir / ("shot-%03d.ppm" % (index + 1))
            qmp.screendump(shot)
            after_count, ax, ay, _, _ = amber_centroid(shot)
            if after_count < AMBER_MIN_PIXELS:
                return fail("move %d: screendump lost the cursor (%d pixel(s))"
                            % (index + 1, after_count))
            moved_x = ax - cx
            moved_y = ay - cy
            # The sprite is clipped at the screen edges, so compare against the
            # guest position rather than the raw requested delta.
            if abs(moved_x - (want_x - base_x)) > CENTROID_TOLERANCE or \
               abs(moved_y - (want_y - base_y)) > CENTROID_TOLERANCE:
                return fail("move %d (%+d, %+d): pixels moved (%+.1f, %+.1f) "
                            "but the guest position moved (%+d, %+d)"
                            % (index + 1, dx, dy, moved_x, moved_y,
                               want_x - base_x, want_y - base_y))
            text = read_log(serial_log).decode("utf-8", "replace")
            positions = AT_RE.findall(text)
            if not positions:
                return fail("move %d: no cursor position marker" % (index + 1))
            got_x, got_y = int(positions[-1][0]), int(positions[-1][1])
            if abs(got_x - want_x) > CENTROID_TOLERANCE or \
               abs(got_y - want_y) > CENTROID_TOLERANCE:
                return fail("move %d: guest reported (%d, %d), expected (%d, %d)"
                            % (index + 1, got_x, got_y, want_x, want_y))
            checked_moves += 1
            print("gfx: move %d (%+d, %+d) -> cursor (%d, %d), pixels (%+.1f, %+.1f)"
                  % (index + 1, dx, dy, got_x, got_y, moved_x, moved_y))

        try:
            proc.wait(timeout=args.run_timeout)
        except subprocess.TimeoutExpired:
            proc.kill()
            return fail("run timed out")
    except RuntimeError as exc:
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
        return fail("guest did not report the gfx selftest")

    print("ok: gfx cursor (%d move(s) verified in the framebuffer, exit %d)"
          % (checked_moves, status))
    return 0


if __name__ == "__main__":
    sys.exit(main())
