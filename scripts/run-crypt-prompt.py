#!/usr/bin/env python3
"""Drive the CrugOS serial passphrase prompt under QEMU.

usage: run-crypt-prompt.py MODE [options]

MODE is correct, wrong or timeout. The script picks a loopback TCP port,
starts `make run` with SERIAL_TCP and without a crypt.pass= loader token,
tees the console into --serial-log, and types an answer at every
`passphrase:` prompt. The boot is then checked by scripts/boot-check.py.
"""

import argparse
import os
import select
import socket
import subprocess
import sys
import time
from pathlib import Path

PROMPT = b"passphrase: "
EXPECTED_EXIT = {"correct": 33, "wrong": 35, "timeout": 35}
ANSWERS = {"correct": 1, "wrong": 3, "timeout": 0}


def free_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def fail(message):
    print("FAIL: " + message)
    return 1


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
    ap.add_argument("--cmdline-extra", default="")
    ap.add_argument("--connect-timeout", type=float, default=60.0)
    ap.add_argument("--run-timeout", type=float, default=120.0)
    args = ap.parse_args()

    if args.mode == "correct" and not args.password:
        return fail("correct mode needs --password")

    port = free_port()
    serial_log = Path(args.serial_log)
    serial_log.parent.mkdir(parents=True, exist_ok=True)
    exit_code = Path(args.exit_code)

    cmd = [
        args.make,
        "--no-print-directory",
        "run",
        "DISK_IMG=" + str(Path(args.disk).resolve()),
        "CRYPT_PASS=",
        "CMDLINE_EXTRA=" + args.cmdline_extra,
        "SERIAL_TCP=127.0.0.1:%d" % port,
    ]
    print("prompt: starting " + " ".join(cmd))
    proc = subprocess.Popen(cmd, cwd=args.repo)

    sock = None
    deadline = time.monotonic() + args.connect_timeout
    while sock is None:
        if proc.poll() is not None:
            return fail("make run exited before the serial socket was ready")
        try:
            sock = socket.create_connection(("127.0.0.1", port), timeout=0.2)
        except OSError:
            if time.monotonic() >= deadline:
                proc.kill()
                return fail("serial socket did not open")
            time.sleep(0.05)
    sock.setblocking(False)
    print("prompt: serial socket connected on port %d" % port)

    answers = []
    if args.mode == "correct":
        answers = [args.password.encode() + b"\r"]
    elif args.mode == "wrong":
        answers = [args.wrong_password.encode() + b"\r"] * 3

    log = open(serial_log, "wb")
    data = b""
    prompts = 0
    sent = 0
    exit_status = None
    end = time.monotonic() + args.run_timeout
    while True:
        if proc.poll() is not None and exit_status is None:
            exit_status = proc.returncode
            break
        if time.monotonic() >= end:
            proc.kill()
            log.close()
            return fail("run timed out")
        try:
            readable, _, _ = select.select([sock], [], [], 0.1)
        except (OSError, ValueError):
            readable = []
        if readable:
            try:
                chunk = sock.recv(4096)
            except BlockingIOError:
                chunk = b""
            if chunk:
                data += chunk
                log.write(chunk)
                log.flush()
                seen = data.count(PROMPT)
                while prompts < seen:
                    prompts += 1
                    if sent < len(answers):
                        sock.sendall(answers[sent])
                        sent += 1
            else:
                try:
                    proc.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    proc.kill()
                break
    log.close()
    try:
        sock.close()
    except OSError:
        pass
    if exit_status is None:
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
        exit_status = proc.returncode

    text = data.decode(errors="replace")
    if not exit_code.exists():
        return fail("missing QEMU exit code file " + str(exit_code))
    try:
        status = int(exit_code.read_text().strip())
    except ValueError:
        return fail("invalid QEMU exit code file " + str(exit_code))
    expected = EXPECTED_EXIT[args.mode]

    secrets = []
    if args.mode == "correct":
        secrets.append(args.password.encode())
    secrets = [s for s in secrets if s]
    for secret in secrets:
        if secret in data:
            return fail("passphrase appeared in the serial log in plaintext")

    if prompts == 0:
        return fail("guest never printed the passphrase prompt")
    if args.mode == "wrong" and prompts != ANSWERS["wrong"]:
        return fail("expected 3 prompts for 3 attempts, saw %d" % prompts)
    if args.mode == "timeout" and prompts != 1:
        return fail("expected 1 timed-out prompt, saw %d" % prompts)
    if "CRUGOS: crypt passphrase required" not in text:
        return fail("missing the interactive unlock banner")
    if args.mode == "wrong" and "CRUGOS: crypt locked (too many attempts)" not in text:
        return fail("three rejected passphrases did not lock the volume")
    if args.mode == "timeout" and "CRUGOS: crypt locked (no passphrase)" not in text:
        return fail("timed-out prompt did not report a locked volume")
    if status != expected:
        return fail("guest exit status %d != expected %d" % (status, expected))

    print("ok: passphrase prompt %s (%d prompt(s), %d answer(s), exit %d)"
          % (args.mode, prompts, sent, status))
    return 0


if __name__ == "__main__":
    sys.exit(main())
