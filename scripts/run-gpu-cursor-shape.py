#!/usr/bin/env python3
"""Prove the virtio-gpu hardware cursor carries real pixels (phase 8B).

The 8B regression the demo showed: the guest created the cursor resource,
attached its backing pages and moved the plane, but never transferred the
pixels, so the host-side resource image stayed fully transparent and the
pointer disappeared from the screen. A QMP screendump cannot see the cursor
plane at all, so this driver boots the guest on a VNC display and speaks just
enough RFB to read the alpha-cursor pseudo-encoding (VNC_ENCODING_ALPHA_CURSOR):
QEMU only sends that rectangle once the guest has defined a cursor image, and
the pixels are the resource bytes verbatim, so a transparent cursor produces
an all-zero alpha channel while the real sprite carries white border pixels
and amber body pixels.

usage: run-gpu-cursor-shape.py --disk IMG --serial-log LOG --exit-code FILE
"""

import argparse
import socket
import struct
import subprocess
import sys
import threading
import time
from pathlib import Path

NL = bytes([10])

CURSOR_W = 64
CURSOR_H = 64
BORDER = (255, 255, 255)
BODY = (255, 208, 64)
VNC_ALPHA_CURSOR = -314
VNC_DESKTOP_SIZE = -223
UPLOAD = b"CRUGOS: gpu cursor uploaded"
READY = b"CRUGOS: gpu cursor ready"
SELFTEST = b"CRUGOS: gpu cursor selftest ok"
VERBOSE = False


def fail(message):
    print("FAIL: " + message)
    return 1


def read_log(path):
    try:
        return Path(path).read_bytes()
    except OSError:
        return b""


def wait_for(path, needle, proc, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if needle in read_log(path):
            return True
        if proc.poll() is not None:
            return needle in read_log(path)
        time.sleep(0.05)
    return False


def find_port():
    for port in range(6000, 6400):
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
            try:
                probe.bind(("127.0.0.1", port))
            except OSError:
                continue
            return port
    raise RuntimeError("no free VNC port")


class Rfb:
    def __init__(self, sock, deadline):
        self.sock = sock
        self.deadline = deadline
        self.bpp = 4

    def recv(self, count):
        out = b""
        while len(out) < count:
            left = self.deadline - time.monotonic()
            if left <= 0:
                raise TimeoutError("RFB read timed out")
            self.sock.settimeout(min(left, 5.0))
            try:
                chunk = self.sock.recv(count - len(out))
            except socket.timeout:
                continue
            if not chunk:
                raise ConnectionError("the VNC server closed the connection")
            out += chunk
        return out

    def handshake(self):
        banner = self.recv(12)
        if not banner.startswith(b"RFB 003.00"):
            raise ValueError("unexpected RFB banner %r" % banner)
        minor = int(banner[8:11])
        self.sock.sendall(b"RFB 003.008" + NL)
        if minor >= 8:
            count = self.recv(1)[0]
            types = self.recv(count)
            if 1 not in types:
                raise ValueError("VNC offers no unauthenticated security type")
            self.sock.sendall(bytes([1]))
            result = struct.unpack(">I", self.recv(4))[0]
            if result != 0:
                raise ValueError("VNC security handshake failed (%d)" % result)
        self.sock.sendall(bytes([1]))  # ClientInit, shared session
        head = self.recv(4)
        width, height = struct.unpack(">HH", head)
        pf = self.recv(16)
        self.bpp = max(1, (pf[0] + 7) // 8)
        name_len = struct.unpack(">I", self.recv(4))[0]
        self.recv(name_len)
        self.width = width
        self.height = height
        encodings = (struct.pack(">BBH", 2, 0, 2)
                     + struct.pack(">i", VNC_DESKTOP_SIZE)
                     + struct.pack(">i", VNC_ALPHA_CURSOR))
        self.sock.sendall(encodings)
        return width, height

    def request(self, x, y, w, h, incremental):
        self.sock.sendall(struct.pack(">BBHHHH", 3, 1 if incremental else 0,
                                    x, y, w, h))

    def pump_requests(self, stop):
        """VNC only sends framebuffer data when the client asks for it."""
        while not stop.wait(0.2):
            try:
                self.request(0, 0, 2, 2, True)
            except OSError:
                return

    def cursor_shape(self):
        while time.monotonic() < self.deadline:
            kind = self.recv(1)[0]
            if kind == 0:
                pad, rects = struct.unpack(">BH", self.recv(3))
                if VERBOSE:
                    print("gpu: framebuffer update with %d rect(s)" % rects)
                for _ in range(rects):
                    x, y, w, h, enc = struct.unpack(">HHHHi", self.recv(12))
                    if VERBOSE:
                        print("gpu: rect %d,%d %dx%d encoding %d"
                              % (x, y, w, h, enc))
                    if enc == VNC_ALPHA_CURSOR:
                        return x, y, w, h, self.recv(w * h * 4)
                    if enc == VNC_DESKTOP_SIZE:
                        self.width, self.height = w, h
                        if VERBOSE:
                            print("gpu: desktop resized to %dx%d" % (w, h))
                        continue
                    if enc == 0:
                        self.recv(w * h * self.bpp)
                    elif enc == 1:
                        self.recv(12)
                    else:
                        raise ValueError("unexpected rectangle encoding %d" % enc)
            elif kind == 1:
                pad, first, count = struct.unpack(">BHH", self.recv(5))
                self.recv(count * 6)
            elif kind == 2:
                continue
            elif kind == 3:
                self.recv(3)
                length = struct.unpack(">I", self.recv(4))[0]
                self.recv(length)
            else:
                raise ValueError("unexpected VNC message type %d" % kind)
        raise TimeoutError("the VNC server never sent a cursor shape")


def check_shape(width, height, data):
    if (width, height) != (CURSOR_W, CURSOR_H):
        return None, "the cursor shape is %dx%d, expected %dx%d" % (
            width, height, CURSOR_W, CURSOR_H)
    opaque = 0
    colours = set()
    for off in range(0, len(data), 4):
        blue, green, red, alpha = data[off:off + 4]
        if alpha == 0:
            continue
        opaque += 1
        colours.add((red, green, blue))
    if opaque < 100:
        return None, ("the cursor alpha channel has %d opaque pixel(s): the "
                      "sprite was never uploaded" % opaque)
    if BORDER not in colours or BODY not in colours:
        return None, ("the cursor colours are %s, expected the white border and "
                      "the amber body" % sorted(colours)[:6])
    return (opaque, len(colours)), None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--make", default="make")
    ap.add_argument("--repo", default=str(Path(__file__).resolve().parent.parent))
    ap.add_argument("--disk", required=True)
    ap.add_argument("--serial-log", required=True)
    ap.add_argument("--exit-code", required=True)
    ap.add_argument("--qemu-extra",
                    default="-vga none -device virtio-gpu-pci,id=gpu0")
    ap.add_argument("--wb-wait", type=int, default=30)
    ap.add_argument("--cmdline", default=None,
                    help="kernel command line, default wb.enable=1 wb.wait=N gpu.enable=1 gpu.test=1")
    ap.add_argument("--boot-timeout", type=float, default=180.0)
    ap.add_argument("--shape-timeout", type=float, default=240.0)
    ap.add_argument("--run-timeout", type=float, default=300.0)
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()
    global VERBOSE
    VERBOSE = args.verbose

    serial_log = Path(args.serial_log).resolve()
    serial_log.parent.mkdir(parents=True, exist_ok=True)
    serial_log.write_bytes(b"")
    exit_code_path = Path(args.exit_code).resolve()
    exit_code_path.parent.mkdir(parents=True, exist_ok=True)
    try:
        exit_code_path.unlink()
    except OSError:
        pass

    port = find_port()
    display = port - 5900
    if args.cmdline is not None:
        cmdline = args.cmdline
    else:
        cmdline = "wb.enable=1 wb.wait=%d gpu.enable=1 gpu.test=1" % args.wb_wait
    cmd = [
        args.make,
        "--no-print-directory",
        "run",
        "DISK_IMG=" + str(Path(args.disk).resolve()),
        "CRYPT_PASS=",
        "NET_MODE=none",
        "QEMU_DISPLAY=vnc=127.0.0.1:%d" % display,
        "SERIAL_TCP=",
        "SERIAL_LOG=" + str(serial_log),
        "QEMU_EXIT=" + str(exit_code_path),
        "QEMU_EXTRA=" + args.qemu_extra,
        "CMDLINE_EXTRA=" + cmdline,
        "TIMEOUT_SEC=" + str(int(args.run_timeout)),
    ]
    print("gpu: starting " + " ".join(cmd))
    proc = subprocess.Popen(cmd, cwd=args.repo)

    sock = None
    try:
        deadline = time.monotonic() + args.boot_timeout
        while True:
            try:
                sock = socket.create_connection(("127.0.0.1", port), timeout=1.0)
                break
            except OSError:
                if proc.poll() is not None:
                    return fail("QEMU exited before the VNC server accepted")
                if time.monotonic() > deadline:
                    return fail("the VNC server never accepted a connection")
                time.sleep(0.1)
        rfb = Rfb(sock, time.monotonic() + args.shape_timeout)
        width, height = rfb.handshake()
        rfb.request(0, 0, 2, 2, False)
        stop = threading.Event()
        pump = threading.Thread(target=rfb.pump_requests, args=(stop,), daemon=True)
        pump.start()
        print("gpu: vnc desktop %dx%d, waiting for the cursor shape"
              % (width, height))
        x, y, w, h, data = rfb.cursor_shape()
        summary, error = check_shape(w, h, data)
        if error is not None:
            return fail(error)
        opaque, colours = summary
        if not wait_for(serial_log, UPLOAD, proc, 5.0):
            return fail("the guest never reported the cursor upload")
        if not wait_for(serial_log, SELFTEST, proc, args.boot_timeout):
            return fail("the guest never completed the cursor selftest")
        print("gpu: cursor shape %dx%d at %d,%d (%d opaque pixel(s), %d colour(s))"
              % (w, h, x, y, opaque, colours))
    finally:
        stop.set()
        if sock is not None:
            sock.close()

    try:
        code = proc.wait(timeout=args.run_timeout)
    except subprocess.TimeoutExpired:
        proc.kill()
        return fail("the guest did not exit after the selftest")
    text = read_log(serial_log)
    for marker in (UPLOAD, READY, SELFTEST):
        if marker not in text:
            return fail("missing serial marker " + marker.decode())
    exit_text = "none"
    if exit_code_path.exists():
        exit_text = exit_code_path.read_text().strip()
    print("ok: hardware cursor shape %dx%d carries %d opaque pixel(s) in "
          "%d colour(s), exit %s" % (w, h, opaque, colours, exit_text))
    return 0


if __name__ == "__main__":
    sys.exit(main())
