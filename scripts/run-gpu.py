#!/usr/bin/env python3
"""Verify virtio-gpu 2D scanout and the GPU-backed Workbench (phase 8A).

The driver boots QEMU with no VGA adapter at all (-vga none) plus a
virtio-gpu-pci device, so the GOP framebuffer is absent and the only display
the guest can use is the driver's own resource. The guest attaches that
resource as the linear framebuffer before the desktop is created, therefore
every Workbench pixel in the QMP screendump travelled through
TRANSFER_TO_HOST_2D plus RESOURCE_FLUSH. A screendump of the gpu0 device then
proves the compositor really ran on the GPU scanout.

usage: run-gpu.py --disk IMG --serial-log LOG --exit-code FILE [options]
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

SCANOUT = b"CRUGOS: gpu scanout ok"
DESKTOP = b"CRUGOS: wb desktop ready"
GPU_OK = b"CRUGOS: gpu selftest ok"
WB_OK = b"CRUGOS: wb selftest ok"

SCANOUT_RE = re.compile(r"CRUGOS: gpu scanout ok \((\d+)x(\d+), (\d+) transfer\(s\)")
GPU_OK_RE = re.compile(
    r"CRUGOS: gpu selftest ok \((\d+)x(\d+), (\d+) transfer\(s\), (\d+) flush\(es\)\)")

# The desktop paints these exact colours; the samples below are chosen away
# from text so a glyph can never be mistaken for a missing region.
DESKTOP_BG = (16, 32, 48)
TITLE_ACTIVE = (255, 176, 0)
TITLE_IDLE = (128, 128, 128)  # INTUITION_TITLE_IDLE_RGB
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
    """A minimal QMP client: greeting, capabilities and a device screendump."""

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

    def screendump(self, path, device):
        try:
            self._cmd({"execute": "screendump",
                       "arguments": {"filename": str(path),
                                     "device": device}})
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


def pixel_at(shot, x, y):
    width, height, pixels = read_ppm(shot)
    if x < 0 or y < 0 or x >= width or y >= height:
        raise ValueError("pixel (%d, %d) outside %dx%d" % (x, y, width, height))
    off = (y * width + x) * 3
    return (pixels[off], pixels[off + 1], pixels[off + 2])


def count_color(shot, colour):
    width, height, pixels = read_ppm(shot)
    want = bytes(colour)
    return sum(1 for off in range(0, width * height * 3, 3)
               if pixels[off:off + 3] == want)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--make", default="make")
    ap.add_argument("--repo", default=str(Path(__file__).resolve().parent.parent))
    ap.add_argument("--disk", required=True)
    ap.add_argument("--serial-log", required=True)
    ap.add_argument("--exit-code", required=True)
    ap.add_argument("--wb-wait", type=int, default=30)
    ap.add_argument("--boot-timeout", type=float, default=90.0)
    ap.add_argument("--run-timeout", type=float, default=300.0)
    ap.add_argument("--device", default="gpu0")
    ap.add_argument("--qemu-extra",
                    default="-vga none -device virtio-gpu-pci,id=gpu0")
    args = ap.parse_args()

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

    tmpdir = Path(tempfile.mkdtemp(prefix=".gpu.",
                                   dir=str(Path(args.disk).resolve().parent)))
    sock_path = str(tmpdir / "qmp.sock")

    # gpu.enable=1 forces the driver on; gpu.test=1 makes the selftest report
    # even when it would skip. wb.wait= gives the driver a window to take the
    # screendump while the desktop is complete and the guest is still alive.
    cmdline = "wb.enable=1 wb.wait=%d gpu.enable=1 gpu.test=1" % args.wb_wait
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
        "CMDLINE_EXTRA=" + cmdline,
        "TIMEOUT_SEC=" + str(int(args.run_timeout)),
    ]
    print("gpu: starting " + " ".join(cmd))
    proc = subprocess.Popen(cmd, cwd=args.repo)

    qmp = None
    try:
        if not wait_for(serial_log, SCANOUT, 1, proc, args.boot_timeout):
            return fail("guest never reported the GPU scanout")
        match = SCANOUT_RE.search(read_log(serial_log).decode("utf-8", "replace"))
        if match is None:
            return fail("gpu scanout line is malformed")
        want_w = int(match.group(1))
        want_h = int(match.group(2))

        if not wait_for(serial_log, DESKTOP, 1, proc, args.boot_timeout):
            return fail("guest never reported the Workbench desktop")
        if not wait_for(serial_log, GPU_OK, 1, proc, args.boot_timeout):
            return fail("guest never finished the GPU selftest")

        qmp = Qmp(sock_path)
        shot = tmpdir / "gpu.ppm"
        qmp.screendump(shot, args.device)
        width, height, _ = read_ppm(shot)
        print("gpu: device %s screendump %dx%d" % (args.device, width, height))
        if width != want_w or height != want_h:
            return fail("scanout is %dx%d but the resource is %dx%d"
                        % (width, height, want_w, want_h))

        # Window geometry comes from workbench.c: the Workbench window opens at
        # (80, 90) and the Shell at (480, 300); the Shell is created second and
        # is therefore the active (amber) window. Samples avoid the text.
        samples = [
            ("desktop background", 1200, 700, DESKTOP_BG),
            ("shell title bar", 600, 306, TITLE_ACTIVE),
            ("workbench title bar", 200, 96, TITLE_IDLE),
            ("workbench frame", 80, 150, FRAME),
        ]
        for label, x, y, want in samples:
            got = pixel_at(shot, x, y)
            print("gpu: %s pixel (%d, %d) -> %s" % (label, x, y, (got,)))
            if got != want:
                return fail("%s pixel (%d, %d) is %s, expected %s"
                            % (label, x, y, (got,), (want,)))

        amber = count_color(shot, TITLE_ACTIVE)
        house = count_color(shot, FRAME)
        print("gpu: amber %d px, frame %d px" % (amber, house))
        if amber < 1000:
            return fail("too few active title-bar pixels: %d" % amber)
        if house < 300:
            return fail("too few window-frame pixels: %d" % house)

        if not wait_for(serial_log, WB_OK, 1, proc, args.run_timeout):
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
    match = GPU_OK_RE.search(data.decode("utf-8", "replace"))
    if match is None:
        return fail("the GPU selftest did not report its counters")
    transfers = int(match.group(3))
    flushes = int(match.group(4))
    if transfers < 2 or flushes < 2:
        return fail("the GPU selftest transferred too little: %d/%d"
                    % (transfers, flushes))
    print("ok: virtio-gpu scanout %dx%d (%d transfer(s), %d flush(es), exit %d)"
          % (want_w, want_h, transfers, flushes, status))
    return 0


if __name__ == "__main__":
    sys.exit(main())
