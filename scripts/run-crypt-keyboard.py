#!/usr/bin/env python3
"""Drive the CrugOS console passphrase prompt through the PS/2 keyboard.

usage: run-crypt-keyboard.py MODE [options]

MODE is correct or wrong. The script starts `make run` with no crypt.pass=
loader token and with a QMP socket, waits for the kernel prompt in the
serial log, then injects one sendkey event per character so the guest
keyboard driver (i8042) feeds input.device. Boot verification stays in
scripts/boot-check.py.
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

PROMPT = b"passphrase: "
EXPECTED_EXIT = {"correct": 33, "wrong": 35}
ATTEMPTS = {"correct": 1, "wrong": 3}

# QEMU key names for printable ASCII that the tests can type.
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


class Qmp:
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

    def send_key(self, name):
        # QMP names the command "send-key" and takes an array of KeyValue.
        self._cmd({"execute": "send-key",
                   "arguments": {"keys": [{"type": "qcode", "data": name}],
                                 "hold-time": 40}})

    def close(self):
        try:
            self.file.close()
            self.sock.close()
        except OSError:
            pass


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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("mode", choices=sorted(EXPECTED_EXIT))
    ap.add_argument("--password", default="")
    ap.add_argument("--wrong-password", default="wrong-password")
    ap.add_argument("--make", default="make")
    ap.add_argument("--repo", default=str(Path(__file__).resolve().parent.parent))
    ap.add_argument("--disk", required=True)
    ap.add_argument("--serial-log", required=True)
    ap.add_argument("--exit-code", required=True)
    ap.add_argument("--prompt-timeout", type=float, default=60.0)
    ap.add_argument("--run-timeout", type=float, default=180.0)
    args = ap.parse_args()

    if args.mode == "correct" and not args.password:
        return fail("correct mode needs --password")
    secret = (args.password if args.mode == "correct" else args.wrong_password)

    serial_log = Path(args.serial_log).resolve()
    serial_log.parent.mkdir(parents=True, exist_ok=True)
    serial_log.write_bytes(b"")
    exit_code_path = Path(args.exit_code).resolve()
    try:
        exit_code_path.unlink()
    except OSError:
        pass

    tmpdir = Path(tempfile.mkdtemp(prefix=".keyboard.",
                                   dir=str(Path(args.disk).resolve().parent)))
    sock_path = str(tmpdir / "qmp.sock")

    cmd = [
        args.make,
        "--no-print-directory",
        "run",
        "DISK_IMG=" + str(Path(args.disk).resolve()),
        "CRYPT_PASS=",
        "SERIAL_TCP=",
        "QEMU_EXTRA=-qmp unix:%s,server=on,wait=off" % sock_path,
    ]
    print("keyboard: starting " + " ".join(cmd))
    proc = subprocess.Popen(cmd, cwd=args.repo)

    try:
        if not wait_for(serial_log, PROMPT, 1, proc, args.prompt_timeout):
            return fail("guest never printed the passphrase prompt")
        qmp = Qmp(sock_path)
        typed = 0
        for attempt in range(1, ATTEMPTS[args.mode] + 1):
            if attempt > 1:
                if not wait_for(serial_log, PROMPT, attempt, proc,
                                args.prompt_timeout):
                    return fail("prompt %d never appeared" % attempt)
            for ch in secret:
                qmp.send_key(key_name(ch))
                typed += 1
                time.sleep(0.04)
            qmp.send_key("ret")
            typed += 1
            time.sleep(0.1)
        qmp.close()

        try:
            proc.wait(timeout=args.run_timeout)
        except subprocess.TimeoutExpired:
            proc.kill()
            return fail("run timed out")
    finally:
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

    if secret and secret.encode() in data:
        return fail("passphrase appeared in the serial log in plaintext")
    prompts = data.count(PROMPT)
    if prompts != ATTEMPTS[args.mode]:
        return fail("expected %d prompt(s), saw %d"
                    % (ATTEMPTS[args.mode], prompts))
    if b"CRUGOS: input ready (serial + keyboard)" not in data:
        return fail("the guest did not report a working keyboard")
    if args.mode == "wrong" and b"CRUGOS: crypt locked (too many attempts)" not in data:
        return fail("three wrong keyboard passphrases did not lock the volume")
    if args.mode == "correct" and b"CRUGOS: crypt ready" not in data:
        return fail("correct keyboard passphrase did not unlock the volume")
    expected = EXPECTED_EXIT[args.mode]
    if status != expected:
        return fail("guest exit status %d != expected %d" % (status, expected))

    print("ok: keyboard unlock %s (%d key event(s), %d prompt(s), exit %d)"
          % (args.mode, typed, prompts, status))
    return 0


if __name__ == "__main__":
    sys.exit(main())
