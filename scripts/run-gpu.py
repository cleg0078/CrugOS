#!/usr/bin/env python3
"""Verify virtio-gpu 2D scanout and the GPU-backed Workbench (phase 8A).

The driver boots QEMU with no VGA adapter at all (-vga none) plus a
virtio-gpu-pci device, so the GOP framebuffer is absent and the only display
the guest can use is the driver's own resource. The guest attaches that
resource as the linear framebuffer before the desktop is created, therefore
every Workbench pixel in the QMP screendump travelled through
TRANSFER_TO_HOST_2D plus RESOURCE_FLUSH. A screendump of the gpu0 device then
proves the compositor really ran on the GPU scanout.

With --usb the run also adds a qemu-xhci controller, hotplugs a USB mouse and
requires pointer motion; that covers the 64-bit BAR decode, because OVMF
places the controller above 4 GiB whenever the virtio-gpu is present.

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
EDID = b"CRUGOS: gpu edid"
MODE_EDID = b"CRUGOS: gpu mode"
CURSOR_READY = b"CRUGOS: gpu cursor ready"
CURSOR_OK = b"CRUGOS: gpu cursor selftest ok"
USB_CTRL = b"CRUGOS: usb controller ready (xhci"
USB_MOUSE = b"CRUGOS: usb mouse ready"
USB_MOVE = b"CRUGOS: usb mouse move"

SCANOUT_RE = re.compile(r"CRUGOS: gpu scanout ok \((\d+)x(\d+), (\d+) transfer\(s\)")
EDID_RE = re.compile(
    r"CRUGOS: gpu edid (\d+)x(\d+) \(vendor ([A-Z?]{3}), product 0x([0-9a-f]{4}), "
    r"(\d+) mode\(s\)\)")
MODE_EDID_RE = re.compile(r"CRUGOS: gpu mode (\d+)x(\d+) \(edid preferred\)")
CURSOR_OK_RE = re.compile(
    r"CRUGOS: gpu cursor selftest ok \((\d+) show\(s\), (\d+) move\(s\)\)")
# gpu.cursor=sw bakes the 12x19 software sprite into the scanout resource, so
# the pointer is part of the framebuffer pixels the screendump carries.
SPRITE_MARK = b"CRUGOS: gpu cursor software sprite (gpu.cursor=sw)"
SPRITE_BODY = (255, 208, 64)  # FB_CURSOR_BODY_RGB 0x00FFD040 in fb.c
SPRITE_W = 12
SPRITE_H = 19
SPRITE_MIN_PX = 40  # the real arrow body covers 71 pixels
GFX_READY_RE = re.compile(
    r"CRUGOS: gfx ready \(\d+x\d+, cursor at (\d+),(\d+)\)")
GFX_CURSOR_RE = re.compile(r"CRUGOS: gfx cursor at \((\d+), (\d+)\)")
WB_CURSOR_RE = re.compile(r"CRUGOS: wb cursor at \((\d+), (-?\d+)\)")
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

    def device_add(self, driver, ident, bus):
        # The controller is created with id=xhci, so its bus is xhci.0.
        self._cmd({"execute": "device_add",
                   "arguments": {"driver": driver, "id": ident,
                                 "bus": bus}})

    def resolve_device(self, ident):
        # A device on a USB bus lands below /machine/periph/xhci.0 rather
        # than at /machine/peripheral; ask qom where it went.
        for parent in ("/machine/peripheral", "/machine/periph/xhci.0",
                       "/machine/periph"):
            try:
                reply = self._cmd({"execute": "qom-list",
                                   "arguments": {"path": parent}})
            except RuntimeError:
                continue
            for child in reply["return"]:
                if child.get("name") == ident:
                    return parent.rstrip("/") + "/" + ident
        return None

    def move(self, axis, value):
        # i8042=off and usb=off leave the hotplugged mouse as the only
        # pointer handler, so a broadcast event reaches it.
        self._cmd({"execute": "input-send-event",
                   "arguments": {"events": [
                       {"type": "rel", "data": {"axis": axis,
                                                "value": value}}]}})

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


def last_pointer(text):
    """Return the last pointer position the guest printed, or None."""
    best = None
    for rx in (GFX_READY_RE, GFX_CURSOR_RE, WB_CURSOR_RE):
        for match in rx.finditer(text):
            if best is None or match.start() > best[0]:
                best = (match.start(), int(match.group(1)), int(match.group(2)))
    if best is None:
        return None
    return best[1], best[2]


def count_in_box(shot, x, y, w, h, colour):
    width, height, pixels = read_ppm(shot)
    want = bytes(colour)
    found = 0
    for yy in range(max(0, y), min(height, y + h)):
        row = yy * width
        for xx in range(max(0, x), min(width, x + w)):
            off = (row + xx) * 3
            if pixels[off:off + 3] == want:
                found += 1
    return found


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
    ap.add_argument("--expect-edid", action="store_true",
                    help="require the display mode to come from the device EDID")
    ap.add_argument("--expect-cursor", action="store_true",
                    help="require the hardware cursor plane instead of the sprite")
    ap.add_argument("--expect-sprite", action="store_true",
                    help="require the software sprite baked into the scanout")
    ap.add_argument("--usb", action="store_true",
                    help="also hotplug a USB mouse with a 64-bit BAR")
    ap.add_argument("--moves", type=int, default=2)
    ap.add_argument("--hotplug-timeout", type=float, default=30.0)
    args = ap.parse_args()

    if args.wb_wait < 1 or args.wb_wait > 120:
        return fail("--wb-wait must be between 1 and 120")
    if args.moves < 1 or args.moves > 16:
        return fail("--moves must be between 1 and 16")

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
    qemu_extra = args.qemu_extra
    if args.usb and "qemu-xhci" not in qemu_extra:
        qemu_extra += " -device qemu-xhci,id=xhci"

    # gpu.enable=1 forces the driver on; gpu.test=1 makes the selftest report
    # even when it would skip. wb.wait= gives the driver a window to take the
    # screendump while the desktop is complete and the guest is still alive.
    cmdline = "wb.enable=1 wb.wait=%d gpu.enable=1 gpu.test=1" % args.wb_wait
    # The hardware cursor plane is opt-in: QEMU's GTK window hides a guest
    # cursor image while the pointer is grabbed, so the sprite is the default.
    if args.expect_cursor:
        cmdline += " gpu.cursor=hw"
    if args.expect_sprite:
        cmdline += " gpu.cursor=sw"
    if args.usb:
        # usb.mouse= opens the kernel poll window that catches a hotplug
        # (usb_selftest loops usb_poll until the device and its reports
        # arrive), and usb.moves= makes the guest require real motion.
        cmdline += " usb.mouse=%d usb.moves=%d" % (int(args.hotplug_timeout),
                                             args.moves)
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
        % (sock_path, qemu_extra),
        "CMDLINE_EXTRA=" + cmdline,
        "TIMEOUT_SEC=" + str(int(args.run_timeout)),
    ]
    if args.usb:
        # usb=off and i8042=off leave the hotplugged mouse as the only
        # pointer, exactly like the demo machine.
        cmd.append("MACHINE_EXTRA=usb=off,i8042=off")
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

        if args.expect_edid:
            # The device publishes a non-default timing: the mode and the
            # scanout must both come from the EDID instead of the fallback.
            text_now = read_log(serial_log).decode("utf-8", "replace")
            edid = EDID_RE.search(text_now)
            if edid is None:
                return fail("the guest never decoded an EDID timing")
            chosen = MODE_EDID_RE.search(text_now)
            if chosen is None:
                return fail("the EDID preferred timing did not become the mode")
            if (chosen.group(1), chosen.group(2)) != (edid.group(1), edid.group(2)):
                return fail("mode %sx%s differs from the EDID preferred %sx%s"
                            % (chosen.group(1), chosen.group(2),
                               edid.group(1), edid.group(2)))
            if int(edid.group(5)) < 1:
                return fail("the EDID reported no timing")
            if (edid.group(1), edid.group(2)) != (str(want_w), str(want_h)):
                return fail("scanout %dx%d differs from the EDID timing %sx%s"
                            % (want_w, want_h, edid.group(1), edid.group(2)))
            print("gpu: edid %sx%s (vendor %s, product 0x%s, %s mode(s))"
                  % (edid.group(1), edid.group(2), edid.group(3),
                     edid.group(4), edid.group(5)))

        if not wait_for(serial_log, DESKTOP, 1, proc, args.boot_timeout):
            return fail("guest never reported the Workbench desktop")
        if not wait_for(serial_log, GPU_OK, 1, proc, args.boot_timeout):
            return fail("guest never finished the GPU selftest")

        if args.expect_cursor:
            # The cursor plane is its own virtqueue and its pixels never reach
            # the scanout resource, so the counters are the whole proof.
            if not wait_for(serial_log, CURSOR_OK, 1, proc, args.boot_timeout):
                return fail("the hardware cursor selftest never completed")
            text_now = read_log(serial_log).decode("utf-8", "replace")
            if CURSOR_READY not in read_log(serial_log):
                return fail("the hardware cursor plane was never installed")
            if "CRUGOS: gpu cursor show " not in text_now:
                return fail("the cursor plane was never shown")
            if "CRUGOS: gpu cursor move " not in text_now:
                return fail("the cursor plane was never moved")
            cursor = CURSOR_OK_RE.search(text_now)
            if cursor is None:
                return fail("the hardware cursor counters are malformed")
            shows = int(cursor.group(1))
            cursor_moves = int(cursor.group(2))
            if shows < 1 or cursor_moves < 1:
                return fail("the cursor plane received too little: %d/%d"
                            % (shows, cursor_moves))
            print("gpu: cursor plane %d show(s), %d move(s)"
                  % (shows, cursor_moves))
        if args.expect_sprite:
            if not wait_for(serial_log, SPRITE_MARK, 1, proc,
                            args.boot_timeout):
                return fail("the guest never selected the software cursor sprite")
            if CURSOR_READY in read_log(serial_log):
                return fail("the hardware cursor plane was installed as well")
            print("gpu: software cursor sprite selected")

        qmp = Qmp(sock_path)
        if args.usb:
            # With the virtio-gpu present OVMF puts the xHCI 64-bit BAR at
            # 0xc000004000; a masked high dword used to map the low identity
            # page, so the controller never reset and no mouse appeared.
            if not wait_for(serial_log, USB_CTRL, 1, proc, args.boot_timeout):
                return fail("the xHCI controller never came up")
            qmp.device_add("usb-mouse", "mouse0", "xhci.0")
            qmp.resolve_device("mouse0")
            if not wait_for(serial_log, USB_MOUSE, 1, proc,
                            args.hotplug_timeout):
                return fail("the hotplugged USB mouse was never enumerated")
            moves = 0
            attempt = 0
            while moves < args.moves and attempt < 10:
                if proc.poll() is not None:
                    break
                attempt += 1
                qmp.move("x", 3 * attempt if attempt % 2 else -3 * attempt)
                want = read_log(serial_log).count(USB_MOVE) + 1
                if not wait_for(serial_log, USB_MOVE, want, proc, 10.0):
                    break
                moves = read_log(serial_log).count(USB_MOVE)
                time.sleep(0.15)
            if moves < 1:
                return fail("the USB mouse never reported pointer motion")
            print("gpu: usb controller ready, %d pointer move(s)" % moves)
        shot = tmpdir / "gpu.ppm"
        qmp.screendump(shot, args.device)
        width, height, _ = read_ppm(shot)
        print("gpu: device %s screendump %dx%d" % (args.device, width, height))
        if width != want_w or height != want_h:
            return fail("scanout is %dx%d but the resource is %dx%d"
                        % (width, height, want_w, want_h))

        if args.expect_sprite:
            point = last_pointer(read_log(serial_log).decode("utf-8", "replace"))
            if point is None:
                return fail("the guest never reported a pointer position")
            body_px = count_in_box(shot, point[0], point[1], SPRITE_W,
                                   SPRITE_H, SPRITE_BODY)
            print("gpu: sprite at (%d, %d) carries %d body px"
                  % (point[0], point[1], body_px))
            if body_px < SPRITE_MIN_PX:
                return fail("the software sprite is not in the scanout at "
                            "(%d, %d): %d body pixel(s)"
                            % (point[0], point[1], body_px))

        # Window geometry comes from workbench.c: the Workbench window opens at
        # (80, 90) and the Shell at (480, 300); the Shell is created second and
        # is therefore the active (amber) window. Samples avoid the text. The
        # background point follows the real size because an EDID gate can run
        # at a different resolution.
        samples = [
            ("desktop background", width - 80, height - 80, DESKTOP_BG),
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
    if args.usb:
        print("ok: virtio-gpu scanout %dx%d with the USB mouse (%d move(s), "
              "%d transfer(s), %d flush(es), exit %d)"
              % (want_w, want_h, moves, transfers, flushes, status))
        return 0
    extra = ""
    if args.expect_edid:
        extra += ", mode from the EDID"
    if args.expect_cursor:
        extra += ", hardware cursor"
    if args.expect_sprite:
        extra += ", software cursor sprite"
    print("ok: virtio-gpu scanout %dx%d%s (%d transfer(s), %d flush(es), exit %d)"
          % (want_w, want_h, extra, transfers, flushes, status))
    return 0


if __name__ == "__main__":
    sys.exit(main())
