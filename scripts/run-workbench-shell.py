#!/usr/bin/env python3
"""Verify the Workbench shell window (phase 6C).

The driver boots QEMU with an xHCI controller but no USB device, hotplugs a
usb-kbd over QMP once the guest has opened the Workbench desktop and its shell
window, and types a scripted sequence of command lines at it. The guest gates
itself through wb.shell= and wb.sh= and prints a marker for every executed line
plus one marker per output line; QEMU screendumps then prove the scrollback
area repainted with body text.

usage: run-workbench-shell.py --disk IMG --serial-log LOG --exit-code FILE [options]
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
KEYBOARD = b"CRUGOS: usb keyboard ready"
SHELL_READY = b"CRUGOS: wb shell ready"
RUN = b"CRUGOS: sh run '"
OK = b"CRUGOS: sh selftest ok"
HOTPLUG_ID = "kbd0"

RUN_RE = re.compile(r"CRUGOS: sh run '(.+)'")

# The shell window is the second window: 320x200 at (480, 300), so its body
# rectangle starts after the 20 px title bar and the 2 px frame. Body text is
# drawn in the Workbench body colour on the Workbench window colour.
BODY_X = 482
BODY_Y = 322
BODY_W = 316
BODY_H = 176
BODY_TEXT = (224, 224, 224)
MIN_CHANGED = 200

# The command lines the gate types, in order. Each one is verified through the
# guest's own 'CRUGOS: sh run' marker before the next one is sent.
COMMANDS = (
    "help",
    "ls /",
    "ver",
    "echo hi",
)

# QEMU qcode names for the characters the scripted commands need.
KEY_NAMES = {
    " ": "spc",
    "/": "slash",
    ".": "dot",
    "-": "minus",
}
for _ch in "abcdefghijklmnopqrstuvwxyz0123456789":
    KEY_NAMES[_ch] = _ch


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
    """A minimal QMP client: capabilities, device_add, key events, screendump."""

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

    def send_key(self, name):
        # A real key press and release through the emulated USB HID keyboard,
        # exactly like run-crypt-keyboard.py drives the passphrase prompt.
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


def body_region(path):
    """Return the body rectangle pixels of a screendump as (x, y, rgb) tuples."""
    width, height, pixels = read_ppm(path)
    region = []
    for y in range(BODY_Y, min(BODY_Y + BODY_H, height)):
        for x in range(BODY_X, min(BODY_X + BODY_W, width)):
            off = (y * width + x) * 3
            region.append((x, y, (pixels[off], pixels[off + 1], pixels[off + 2])))
    return region


def compare_body(before, after):
    """Count changed body pixels and return (changed, body text pixels)."""
    changed = 0
    text = 0
    for (x, y, old), (ax, ay, new) in zip(before, after):
        if old != new:
            changed += 1
            if new == BODY_TEXT:
                text += 1
    return changed, text


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--make", default="make")
    ap.add_argument("--repo", default=str(Path(__file__).resolve().parent.parent))
    ap.add_argument("--disk", required=True)
    ap.add_argument("--serial-log", required=True)
    ap.add_argument("--exit-code", required=True)
    ap.add_argument("--cmds", type=int, default=4)
    ap.add_argument("--wb-wait", type=int, default=30)
    ap.add_argument("--boot-timeout", type=float, default=90.0)
    ap.add_argument("--hotplug-timeout", type=float, default=30.0)
    ap.add_argument("--event-timeout", type=float, default=20.0)
    ap.add_argument("--key-delay", type=float, default=0.06)
    ap.add_argument("--run-timeout", type=float, default=240.0)
    ap.add_argument("--qemu-extra", default="-device qemu-xhci,id=xhci")
    args = ap.parse_args()

    if args.cmds < 1 or args.cmds > 8:
        return fail("--cmds must be between 1 and 8")
    if args.cmds != len(COMMANDS):
        return fail("the scripted sequence types exactly %d command(s)" % len(COMMANDS))
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
    tmpdir = Path(tempfile.mkdtemp(prefix=".wbs.",
                                   dir=str(Path(args.disk).resolve().parent)))
    sock_path = str(tmpdir / "qmp.sock")

    # i8042=off removes the PS/2 controller and usb=off the machine USB
    # controllers, so the hotplugged USB keyboard is the only keyboard.
    cmdline = "wb.enable=1 wb.wait=%d wb.shell=1 wb.sh=%d" % (args.wb_wait,
                                                             args.cmds)
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
    print("wbs: starting " + " ".join(cmd))
    proc = subprocess.Popen(cmd, cwd=args.repo)

    qmp = None
    typed = 0
    try:
        if not wait_for(serial_log, DESKTOP, 1, proc, args.boot_timeout):
            return fail("guest never reported the Workbench desktop")
        qmp = Qmp(sock_path)
        qmp.device_add("usb-kbd", HOTPLUG_ID, "xhci.0")
        if not wait_for(serial_log, KEYBOARD, 1, proc, args.hotplug_timeout):
            return fail("hotplugged keyboard was never enumerated")
        if not wait_for(serial_log, SHELL_READY, 1, proc, args.hotplug_timeout):
            return fail("guest never reported the Workbench shell window")

        def type_line(line):
            for ch in line:
                name = KEY_NAMES.get(ch)
                if name is None:
                    raise RuntimeError("no key mapping for %r" % ch)
                qmp.send_key(name)
                time.sleep(args.key_delay)
            qmp.send_key("ret")
            print("wbs: typed %r" % line)

        def expect(needle, what, count=1):
            if not wait_for(serial_log, needle, count, proc, args.event_timeout):
                raise RuntimeError("guest never reported " + what)
            print("wbs: " + what)

        shot_before = tmpdir / "before.ppm"
        qmp.screendump(shot_before)
        before = body_region(shot_before)

        for line in COMMANDS:
            ran = read_log(serial_log).count(RUN)
            type_line(line)
            expect(RUN, 'the line %r ran' % line, ran + 1)
            typed += 1

        expect(b"CRUGOS: sh out CrugOS ", "the ver command printed the version")
        expect(b"CRUGOS: sh out hi", "the echo command printed its text")

        shot_after = tmpdir / "after.ppm"
        qmp.screendump(shot_after)
        after = body_region(shot_after)
        changed, text = compare_body(before, after)
        print("wbs: shell body changed %d pixel(s), %d body-text pixel(s)"
              % (changed, text))
        if changed < MIN_CHANGED:
            return fail("the shell scrollback did not repaint: %d changed pixel(s)"
                        % changed)
        if text < 1:
            return fail("the shell body has no body-colour text pixels")

        expect(OK, "the shell selftest finished")

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
        return fail("guest did not report the shell selftest")
    if typed < 1:
        return fail("no shell command was typed")
    lines = RUN_RE.findall(data.decode("utf-8", "replace"))
    print("ok: workbench shell commands (%d command(s), exit %d)"
          % (typed, status))
    print("wbs: ran " + repr(lines))
    return 0


if __name__ == "__main__":
    sys.exit(main())