#!/usr/bin/env python3
"""Verify the xHCI USB keyboard hotplug path.

The driver boots QEMU with a qemu-xhci controller and no USB device at all,
then plugs a usb-kbd in over QMP once the guest reports the controller, and
types keys at it through QMP. The kernel side waits for the key reports via
usb.wait= and usb.keys= on the command line, so the guest gates itself. Boot
verification stays in scripts/boot-check.py.

usage: run-usb-hotplug.py --disk IMG --serial-log LOG --exit-code FILE [options]
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
KEYBOARD = b"CRUGOS: usb keyboard ready"
HOTPLUG_ID = "kbd0"
KEY_NAMES = {
    " ": "spc", "-": "minus", "=": "equal", ".": "dot", ",": "comma",
    "/": "slash", ";": "semicolon", "'": "apostrophe", "[": "bracket_left",
    "]": "bracket_right", "\\": "backslash", "`": "grave_accent",
}


def key_name(ch):
    if ch in KEY_NAMES:
        return KEY_NAMES[ch]
    if ch.isalpha() and ch.islower():
        return ch
    if ch.isdigit():
        return ch
    raise SystemExit("FAIL: cannot type character %r" % ch)


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
    """A minimal QMP client: capabilities, device_add and key injection."""

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
        # The controller was created with id=xhci, so its bus is xhci.0 and
        # the hotplugged keyboard ends up under /machine/peripheral/<ident>.
        self._cmd({"execute": "device_add",
                   "arguments": {"driver": driver, "id": ident, "bus": bus}})

    def send_key(self, name):
        # QMP names the command "send-key" and takes an array of KeyValue.
        self._cmd({"execute": "send-key",
                   "arguments": {"keys": [{"type": "qcode", "data": name}],
                                 "hold-time": 40}})

    def send_key_event(self, path, name):
        # input-send-event addresses the device directly, bypassing the
        # console input routing that -display none leaves empty.
        self._cmd({"execute": "input-send-event",
                   "arguments": {
                       "device": path,
                       "events": [{"type": "key",
                                   "data": {"down": True,
                                            "key": {"type": "qcode",
                                                    "data": name}}},
                                  {"type": "key",
                                   "data": {"down": False,
                                            "key": {"type": "qcode",
                                                    "data": name}}}]}})

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
    ap.add_argument("--keys", default="abc")
    ap.add_argument("--wait-seconds", type=int, default=20)
    ap.add_argument("--controller-timeout", type=float, default=60.0)
    ap.add_argument("--hotplug-timeout", type=float, default=30.0)
    ap.add_argument("--key-timeout", type=float, default=10.0)
    ap.add_argument("--run-timeout", type=float, default=240.0)
    args = ap.parse_args()

    keys = args.keys
    if not keys or len(keys) > 16:
        return fail("--keys must hold 1 to 16 characters")
    if args.wait_seconds < 1 or args.wait_seconds > 120:
        return fail("--wait-seconds must be between 1 and 120")

    serial_log = Path(args.serial_log).resolve()
    serial_log.parent.mkdir(parents=True, exist_ok=True)
    serial_log.write_bytes(b"")
    exit_code_path = Path(args.exit_code).resolve()
    exit_code_path.parent.mkdir(parents=True, exist_ok=True)
    try:
        exit_code_path.unlink()
    except OSError:
        pass

    tmpdir = Path(tempfile.mkdtemp(prefix=".usb.",
                                   dir=str(Path(args.disk).resolve().parent)))
    sock_path = str(tmpdir / "qmp.sock")

    # i8042=off removes the PS/2 keyboard so the hotplugged USB keyboard is the
    # only keyboard: QMP key events cannot be served by the legacy controller.
    # usb=off keeps the machine's own USB controllers out of the picture.
    cmdline = "usb.wait=%d usb.keys=%d" % (args.wait_seconds, len(keys))
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
    print("usb: starting " + " ".join(cmd))
    proc = subprocess.Popen(cmd, cwd=args.repo)

    qmp = None
    typed = 0
    try:
        if not wait_for(serial_log, CONTROLLER, 1, proc, args.controller_timeout):
            return fail("guest never reported the xHCI controller")
        qmp = Qmp(sock_path)
        qmp.device_add("usb-kbd", HOTPLUG_ID, "xhci.0")
        if not wait_for(serial_log, KEYBOARD, 1, proc, args.hotplug_timeout):
            return fail("hotplugged keyboard was never enumerated")

        # The guest itself blocks in usb_selftest until usb.keys= key reports
        # arrive, so each marker confirms the decode round trip.
        direct = True
        for ch in keys:
            name = key_name(ch)
            marker = ("CRUGOS: usb key '%s'" % ch).encode()
            seen = read_log(serial_log).count(marker)
            if direct:
                qmp.send_key(name)
            else:
                qmp.send_key_event("/machine/peripheral/" + HOTPLUG_ID, name)
            typed += 1
            if not wait_for(serial_log, marker, seen + 1, proc,
                            args.key_timeout):
                if direct:
                    # -display none leaves the console routing without a
                    # keyboard handler, so try the direct device path once.
                    direct = False
                    qmp.send_key_event("/machine/peripheral/" + HOTPLUG_ID,
                                       name)
                    if not wait_for(serial_log, marker, seen + 1, proc,
                                    args.key_timeout):
                        return fail("guest never decoded key %r" % ch)
                else:
                    return fail("guest never decoded key %r" % ch)
            time.sleep(0.05)

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
    if b"CRUGOS: usb selftest ok" not in data:
        return fail("guest did not report the USB keyboard selftest")

    print("ok: usb hotplug keyboard (%d key(s) %r, port hotplug, exit %d)"
          % (typed, keys, status))
    return 0


if __name__ == "__main__":
    sys.exit(main())
