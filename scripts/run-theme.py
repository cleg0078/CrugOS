#!/usr/bin/env python3
"""Gate 69/70/71: theme and wallpaper check.

Boots the Workbench with wb.theme= and wb.wallpaper=, takes a QMP screendump of
the virtio-gpu scanout and checks the window chrome pixel by pixel: the glass
theme paints a light gradient title bar over a photographic wallpaper, the
classic theme keeps the phase 6 amber title on the flat navy desktop.
"""
import argparse
import json
import re
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

DESKTOP = b"CRUGOS: wb desktop ready"
THEME_GLASS = b"CRUGOS: theme glass active"
THEME_CLASSIC = b"CRUGOS: theme classic active"
WALL_SUNSET = b"CRUGOS: wallpaper sunset"
WALL_OFF = b"CRUGOS: wallpaper off"

GLASS_BODY = (244, 244, 245)   # theme window colour
GLASS_BORDER = (138, 138, 143) # theme frame colour
GLASS_MENUBAR = (217, 217, 217)
CLASSIC_TITLE = (255, 176, 0)
CLASSIC_FRAME = (255, 255, 255)
CLASSIC_IDLE = (128, 128, 128)  # the idle title bar of the phase 6 look
CLASSIC_DESKTOP = (16, 32, 48)
GLASS_DESKTOP = (27, 42, 58)  # 0x001B2A3A


def fail(message):
    print("FAIL: " + message, file=sys.stderr)
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
            return False
        time.sleep(0.1)
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
        self._read()
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
        self._cmd({"execute": "screendump",
                   "arguments": {"filename": str(path),
                                 "device": device}})

    def close(self):
        try:
            self.file.close()
            self.sock.close()
        except OSError:
            pass


def read_ppm(path):
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
    pos += 1
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


def near(got, want, slack=6):
    return all(abs(a - b) <= slack for a, b in zip(got, want))


def check_glass(shot, text, width, height):
    body = pixel_at(shot, 200, 150)
    if not near(body, GLASS_BODY):
        return fail("glass window body is %r, expected %r" % (body, GLASS_BODY))
    top = pixel_at(shot, 160, 92)
    bottom = pixel_at(shot, 160, 110)
    if sum(top) <= sum(bottom):
        return fail("glass title gradient runs the wrong way: %r -> %r"
                    % (top, bottom))
    if min(top) < 200:
        return fail("glass title top is %r, expected a light gradient" % (top,))
    border = pixel_at(shot, 80, 150)
    if not near(border, GLASS_BORDER):
        return fail("glass window border is %r, expected %r"
                    % (border, GLASS_BORDER))
    menubar = pixel_at(shot, 600, 10)
    if not near(menubar, GLASS_MENUBAR):
        return fail("glass menu bar is %r, expected %r"
                    % (menubar, GLASS_MENUBAR))
    print("theme: glass body %r, border %r, title %r -> %r"
          % (body, border, top, bottom))
    return 0


def check_classic(shot, text, width, height):
    title = pixel_at(shot, 160, 96)
    if title not in (CLASSIC_TITLE, CLASSIC_IDLE):
        return fail("classic title is %r, expected the amber active bar or the"
                    " grey idle bar" % (title,))
    frame = pixel_at(shot, 80, 150)
    if frame != CLASSIC_FRAME:
        return fail("classic frame is %r, expected %r"
                    % (frame, CLASSIC_FRAME))
    desktop = pixel_at(shot, width - 80, height - 80)
    if desktop != CLASSIC_DESKTOP:
        return fail("classic desktop is %r, expected %r"
                    % (desktop, CLASSIC_DESKTOP))
    print("theme: classic title %r, frame %r, desktop %r"
          % (title, frame, desktop))
    return 0


def check_wallpaper(shot, text, kind, width, height, theme):
    # The wallpaper can be switched off under either theme, so the flat desktop
    # colour it falls back to is the one of the theme under test.
    flat = GLASS_DESKTOP if theme == "glass" else CLASSIC_DESKTOP
    if kind == "sunset":
        if WALL_SUNSET not in text:
            return fail("the guest never rendered the sunset wallpaper")
        match = re.search(rb"CRUGOS: wallpaper sunset (\d+)x(\d+)", text)
        if match is None:
            return fail("the guest never reported the wallpaper size")
        if (int(match.group(1)), int(match.group(2))) != (width, height):
            return fail("the wallpaper buffer is %sx%s but the scanout is %dx%d"
                        % (match.group(1).decode(), match.group(2).decode(),
                           width, height))
        sky = pixel_at(shot, width // 2, height // 4)
        sea = pixel_at(shot, width // 2, height - 40)
        desktop = pixel_at(shot, width - 80, height - 80)
        if desktop == flat:
            return fail("the desktop is the flat theme colour, not the wallpaper")
        if sum(sky) <= sum(sea):
            return fail("wallpaper horizon is upside down: sky %r, sea %r"
                        % (sky, sea))
        if sky[2] < sky[0]:
            return fail("wallpaper sky is not warm at the horizon: %r" % (sky,))
        print("wallpaper: sunset sky %r, sea %r, desktop %r"
              % (sky, sea, desktop))
    else:
        if WALL_OFF not in text:
            return fail("the guest never reported the wallpaper as off")
        if WALL_SUNSET + b" " in text:
            return fail("the wallpaper was rendered although it was off")
        desktop = pixel_at(shot, width - 80, height - 80)
        if desktop != flat:
            return fail("off desktop is %r, expected the theme colour %r"
                        % (desktop, flat))
        print("wallpaper: off keeps the theme desktop %r" % (desktop,))
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--make", default="make")
    parser.add_argument("--repo", default=".")
    parser.add_argument("--disk", default="build/x86_64/disk.img")
    parser.add_argument("--serial-log", default="build/x86_64/serial.log")
    parser.add_argument("--exit-code", default="build/x86_64/qemu-exit.txt")
    parser.add_argument("--theme", default="glass", choices=("glass", "classic"))
    parser.add_argument("--wallpaper", default="sunset",
                        choices=("sunset", "off"))
    parser.add_argument("--wb-hold", type=int, default=15)
    parser.add_argument("--boot-timeout", type=float, default=180.0)
    parser.add_argument("--run-timeout", type=float, default=300.0)
    parser.add_argument("--qemu-extra",
                        default="-vga none -device virtio-gpu-pci,id=gpu0")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()

    if args.wb_hold < 1 or args.wb_hold > 120:
        return fail("--wb-hold must be between 1 and 120")

    serial_log = Path(args.serial_log).resolve()
    serial_log.parent.mkdir(parents=True, exist_ok=True)
    serial_log.write_bytes(b"")
    exit_code_path = Path(args.exit_code).resolve()
    exit_code_path.parent.mkdir(parents=True, exist_ok=True)
    try:
        exit_code_path.unlink()
    except OSError:
        pass

    tmpdir = Path(tempfile.mkdtemp(prefix=".theme.",
                                   dir=str(Path(args.disk).resolve().parent)))
    sock_path = str(tmpdir / "qmp.sock")
    # wb.wait=0 disarms the action selftest: this gate injects no input and only
    # reads pixels, so wb.hold= keeps the finished desktop on screen instead.
    cmdline = ("wb.enable=1 wb.wait=0 wb.hold=%d wb.theme=%s wb.wallpaper=%s"
               % (args.wb_hold, args.theme, args.wallpaper))
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
        "MACHINE_EXTRA=usb=off,i8042=off",
        "TIMEOUT_SEC=" + str(int(args.run_timeout)),
    ]
    print("theme: starting " + " ".join(cmd))
    proc = subprocess.Popen(cmd, cwd=args.repo)

    qmp = None
    try:
        if not wait_for(serial_log, DESKTOP, proc, args.boot_timeout):
            return fail("the desktop never came up")
        time.sleep(0.5)
        qmp = Qmp(sock_path, timeout=30.0)
        shot = tmpdir / "shot.ppm"
        qmp.screendump(shot, "gpu0")
        width, height, _ = read_ppm(shot)
        if args.verbose:
            print("theme: screendump %dx%d" % (width, height))
        text = read_log(serial_log)
        if args.theme == "glass":
            if THEME_GLASS not in text:
                return fail("the guest did not report the glass theme active")
            rc = check_glass(shot, text, width, height)
        else:
            if THEME_CLASSIC not in text:
                return fail("the guest did not report the classic theme active")
            rc = check_classic(shot, text, width, height)
        if rc != 0:
            return rc
        rc = check_wallpaper(shot, text, args.wallpaper, width, height,
                             args.theme)
        if qmp is not None:
            qmp.close()
            qmp = None
        if rc == 0:
            # Let the guest end its own hold: run-qemu writes the real exit
            # code only when QEMU stops by itself, and boot-check asserts it is
            # the isa-debug-exit value 33.  Killing QEMU here would leave the
            # gate with no exit status to check.
            deadline = time.monotonic() + args.run_timeout
            while proc.poll() is None and time.monotonic() < deadline:
                time.sleep(0.2)
            if proc.poll() is None:
                print("theme: the guest never ended its hold", file=sys.stderr)
                return fail("the guest never ended its hold")
        return rc
    finally:
        if qmp is not None:
            qmp.close()
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
        for leftover in tmpdir.glob("*"):
            try:
                leftover.unlink()
            except OSError:
                pass
        try:
            tmpdir.rmdir()
        except OSError:
            pass


if __name__ == "__main__":
    sys.exit(main())
