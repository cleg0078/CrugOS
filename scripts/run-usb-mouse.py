#!/usr/bin/env python3
"""Verify the xHCI USB HID boot mouse hotplug and pointer path.

The driver boots QEMU with a qemu-xhci controller and no USB device at all,
then plugs a usb-mouse in over QMP once the guest reports the controller, and
moves it through input-send-event relative events. The kernel side waits for
the pointer reports via usb.mouse= and usb.moves= on the command line, so the
guest gates itself. Boot verification stays in scripts/boot-check.py.

usage: run-usb-mouse.py --disk IMG --serial-log LOG --exit-code FILE [options]
"""

import argparse
import json
import shutil
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

CONTROLLER = b"CRUGOS: usb controller ready"
MOUSE = b"CRUGOS: usb mouse ready"
MOVE = b"CRUGOS: usb mouse move"
BUTTON = b"CRUGOS: usb mouse button"
HOTPLUG_ID = "mouse0"
DEVICE_PATH = "/machine/peripheral/" + HOTPLUG_ID


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
    """A minimal QMP client: capabilities, device_add and input events."""

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
        self.path = None

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
        # The controller was created with id=xhci, so its bus is xhci.0.
        self._cmd({"execute": "device_add",
                   "arguments": {"driver": driver, "id": ident, "bus": bus}})

    def resolve_device(self, ident):
        # QEMU exposes a hotplugged USB device below the bus, at a path such as
        # /machine/periph/xhci.0/mouse0, not at /machine/peripheral.  Ask qom
        # where it landed; a missing path still works (events go everywhere).
        for parent in ("/machine/peripheral", "/machine/periph/xhci.0",
                       "/machine/periph"):
            try:
                reply = self._cmd({"execute": "qom-list",
                                   "arguments": {"path": parent}})
            except RuntimeError:
                continue
            for child in reply["return"]:
                if child.get("name") == ident:
                    self.path = parent.rstrip("/") + "/" + ident
                    return self.path
        return None

    def send_events(self, events):
        # input-send-event's "device" argument is resolved with
        # qdev_find_recursive(sysbus), which never reaches a device on a USB
        # bus, so naming the hotplugged mouse fails with DeviceNotFound.  With
        # i8042=off and usb=off this mouse is the only pointer handler, so a
        # plain broadcast reaches it.
        self._cmd({"execute": "input-send-event",
                   "arguments": {"events": events}})

    def move(self, axis, value):
        self.send_events([{"type": "rel",
                          "data": {"axis": axis, "value": value}}])

    def button(self, down):
        self.send_events([{"type": "btn",
                          "data": {"down": down, "button": "left"}}])

    def close(self):
        try:
            self.file.close()
            self.sock.close()
        except OSError:
            pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--make", default="make")
    ap.add_argument("--repo", default=str(Path(__file__).resolve().parent.parent))
    ap.add_argument("--disk", required=True)
    ap.add_argument("--serial-log", required=True)
    ap.add_argument("--exit-code", required=True)
    ap.add_argument("--moves", type=int, default=2)
    ap.add_argument("--mouse-wait", type=int, default=20)
    ap.add_argument("--controller-timeout", type=float, default=60.0)
    ap.add_argument("--hotplug-timeout", type=float, default=30.0)
    ap.add_argument("--event-timeout", type=float, default=10.0)
    ap.add_argument("--run-timeout", type=float, default=240.0)
    args = ap.parse_args()

    if args.moves < 1 or args.moves > 16:
        return fail("--moves must be between 1 and 16")
    if args.mouse_wait < 1 or args.mouse_wait > 120:
        return fail("--mouse-wait must be between 1 and 120")

    serial_log = Path(args.serial_log).resolve()
    serial_log.parent.mkdir(parents=True, exist_ok=True)
    serial_log.write_bytes(b"")
    exit_code_path = Path(args.exit_code).resolve()
    exit_code_path.parent.mkdir(parents=True, exist_ok=True)
    try:
        exit_code_path.unlink()
    except OSError:
        pass

    tmpdir = Path(tempfile.mkdtemp(prefix=".usbmouse.",
                                   dir=str(Path(args.disk).resolve().parent)))
    sock_path = str(tmpdir / "qmp.sock")

    # i8042=off removes the PS/2 mouse and usb=off the machine's own USB
    # controllers, so the hotplugged USB mouse is the only pointer device.
    cmdline = "usb.mouse=%d usb.moves=%d" % (args.mouse_wait, args.moves)
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
        "QEMU_EXTRA=-qmp unix:%s,server=on,wait=off -device qemu-xhci,id=xhci" % sock_path,
        "MACHINE_EXTRA=usb=off,i8042=off",
        "CMDLINE_EXTRA=" + cmdline,
        "TIMEOUT_SEC=" + str(int(args.run_timeout)),
    ]
    print("usb-mouse: starting " + " ".join(cmd))
    proc = subprocess.Popen(cmd, cwd=args.repo)

    qmp = None
    moves = 0
    buttons = 0
    try:
        if not wait_for(serial_log, CONTROLLER, 1, proc, args.controller_timeout):
            return fail("guest never reported the xHCI controller")
        qmp = Qmp(sock_path)
        qmp.device_add("usb-mouse", HOTPLUG_ID, "xhci.0")
        qmp.resolve_device(HOTPLUG_ID)
        if not wait_for(serial_log, MOUSE, 1, proc, args.hotplug_timeout):
            return fail("hotplugged mouse was never enumerated")

        # Order matters, and every QMP call is its own HID report.  The driver
        # treats the first report after a reset as the button baseline
        # (changes == 0) exactly like a real boot mouse, so a lone press would
        # be invisible.  Move on one axis first: one report establishes the
        # baseline, and the press afterwards is a genuine change.  The press is
        # the last event the guest's usb.moves= budget needs, so nothing is
        # injected after the guest may already be leaving the selftest.
        button_seen = read_log(serial_log).count(BUTTON)
        want_moves = args.moves - 1 if args.moves > 1 else 1
        attempt = 0
        while read_log(serial_log).count(MOVE) < want_moves and attempt < 10:
            if proc.poll() is not None:
                break
            attempt += 1
            try:
                qmp.move("x", 3 * attempt if attempt % 2 else -3 * attempt)
            except (BrokenPipeError, OSError):
                break
            if not wait_for(serial_log, MOVE, read_log(serial_log).count(MOVE) + 1,
                            proc, args.event_timeout):
                break
            time.sleep(0.15)
        moves = read_log(serial_log).count(MOVE)
        if moves < 1:
            return fail("guest never reported a pointer move")

        qmp.button(True)
        if not wait_for(serial_log, BUTTON, button_seen + 1, proc,
                        args.event_timeout):
            return fail("guest never reported a mouse button")
        buttons += 1
        time.sleep(0.15)
        try:
            qmp.button(False)
        except (BrokenPipeError, OSError):
            pass

        try:
            proc.wait(timeout=args.run_timeout)
        except subprocess.TimeoutExpired:
            proc.kill()
            return fail("run timed out")
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
    if data.count(MOVE) < 1 or data.count(BUTTON) < 1:
        return fail("guest reported no pointer events")
    if MOUSE not in data:
        return fail("guest did not report the USB mouse endpoint")
    if b"CRUGOS: usb selftest ok" not in data:
        return fail("guest did not report the USB input selftest")

    print("ok: usb hotplug mouse (%d move(s), %d button(s), port hotplug, exit %d)"
          % (moves, buttons, status))
    return 0


if __name__ == "__main__":
    sys.exit(main())
