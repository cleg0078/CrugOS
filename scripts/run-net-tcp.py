#!/usr/bin/env python3
"""Drive the phase 4C TCP gate: client leg, socket syscalls and guest server.

The guest boots with a legacy virtio-net NIC on QEMU's user-mode network and a
loader command line that asks for the TCP self-test:

    net.tcp=<echo port> net.serve=<seconds>

Two host roles run at once:

  * an HTTP responder on 127.0.0.1:<echo port>, which the guest reaches as
    10.0.2.2:<echo port> (slirp routes 10.0.2.2 to the host loopback). The guest
    uses it for the plain client leg and again for the socket-syscall leg;
  * an HTTP client of the guest's own server, reached through
    NET_HOSTFWD=tcp:127.0.0.1:<forward port>-:8080.

The serial console is carried over SERIAL_TCP so the driver can follow the boot
and fetch the guest listener as soon as the kernel reports it.
"""

import argparse
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path

HTTP_GET = b"GET / HTTP/1.0\r\nHost: crugos\r\n\r\n"
ECHO_RESPONSE = b"HTTP/1.0 200 OK\r\nContent-Length: 14\r\n\r\ncrugos-tcp-ok\n"
SERVED_BODY = b"crugos-served\n"
LISTEN_MARKER = b"CRUGOS: tcp listen ready (port 8080)"
MARKERS = (
    b"CRUGOS: tcp connect ok",
    b"CRUGOS: tcp http ok",
    b"CRUGOS: tcp socket syscall ok",
    b"CRUGOS: tcp http served",
    b"CRUGOS: net selftest ok",
)


def fail(message):
    print("FAIL: " + message)
    return 1


def free_port():
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.bind(("127.0.0.1", 0))
    port = sock.getsockname()[1]
    sock.close()
    return port


class EchoServer(threading.Thread):
    """Answer the guest's HTTP GET requests over the slirp host loopback."""

    def __init__(self, port):
        super().__init__(daemon=True)
        self.port = port
        self.count = 0
        self.stop = threading.Event()

    def run(self):
        server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        server.bind(("127.0.0.1", self.port))
        server.listen(8)
        server.settimeout(0.5)
        while not self.stop.is_set():
            try:
                conn, _ = server.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            threading.Thread(target=self.handle, args=(conn,), daemon=True).start()
        server.close()

    def handle(self, conn):
        try:
            conn.settimeout(5.0)
            request = b""
            while b"\r\n\r\n" not in request and len(request) < 4096:
                chunk = conn.recv(1024)
                if not chunk:
                    break
                request += chunk
            if request.startswith(b"GET "):
                conn.sendall(ECHO_RESPONSE)
                self.count += 1
        except OSError:
            pass
        finally:
            try:
                conn.close()
            except OSError:
                pass


def fetch_guest(port, result, timeout=25.0):
    """Fetch the guest HTTP server through the QEMU host-forwarded port."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            conn = socket.create_connection(("127.0.0.1", port), timeout=5.0)
        except OSError:
            time.sleep(0.2)
            continue
        try:
            conn.settimeout(10.0)
            conn.sendall(HTTP_GET)
            data = b""
            while True:
                chunk = conn.recv(1024)
                if not chunk:
                    break
                data += chunk
                if SERVED_BODY in data:
                    break
            result["data"] = data
            return
        except OSError as exc:
            result["error"] = str(exc)
            return
        finally:
            try:
                conn.close()
            except OSError:
                pass
    result["error"] = "guest server unreachable"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", default=".")
    parser.add_argument("--disk", required=True)
    parser.add_argument("--serial-log", required=True)
    parser.add_argument("--exit-code", required=True)
    parser.add_argument("--serve-seconds", type=int, default=15)
    parser.add_argument("--run-timeout", type=int, default=180)
    parser.add_argument("--connect-timeout", type=float, default=30.0)
    args = parser.parse_args()

    echo_port = free_port()
    forward_port = free_port()
    serial_port = free_port()

    echo = EchoServer(echo_port)
    echo.start()

    cmdline = "net.tcp=" + str(echo_port) + " net.serve=" + str(args.serve_seconds)
    command = [
        "make", "--no-print-directory", "run",
        "DISK_IMG=" + args.disk,
        "CRYPT_PASS=",
        "NET_MODE=user",
        "NET_HOSTFWD=tcp:127.0.0.1:" + str(forward_port) + "-:8080",
        "CMDLINE_EXTRA=" + cmdline,
        "SERIAL_TCP=127.0.0.1:" + str(serial_port),
        "QEMU_DISPLAY=none",
        "TIMEOUT_SEC=" + str(args.run_timeout),
    ]
    print("run-net-tcp: echo port", echo_port, "forward port", forward_port,
          "serial port", serial_port)
    process = subprocess.Popen(command, cwd=args.repo)

    log = bytearray()
    serial = None
    deadline = time.monotonic() + args.connect_timeout
    while time.monotonic() < deadline:
        try:
            serial = socket.create_connection(("127.0.0.1", serial_port), timeout=2.0)
            break
        except OSError:
            time.sleep(0.2)
    if serial is None:
        process.kill()
        process.wait()
        return fail("serial console did not open on 127.0.0.1:" + str(serial_port))
    serial.settimeout(1.0)

    fetch_result = {}
    fetch_thread = None
    try:
        while True:
            try:
                chunk = serial.recv(4096)
            except socket.timeout:
                if process.poll() is not None:
                    break
                continue
            except OSError:
                break
            if not chunk:
                break
            log += chunk
            if fetch_thread is None and LISTEN_MARKER in log:
                fetch_thread = threading.Thread(target=fetch_guest,
                                                args=(forward_port, fetch_result),
                                                daemon=True)
                fetch_thread.start()
    finally:
        serial.close()

    try:
        process.wait(timeout=args.run_timeout)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()
    echo.stop.set()
    if fetch_thread is not None:
        fetch_thread.join(timeout=15.0)

    Path(args.serial_log).write_bytes(bytes(log))
    text = bytes(log)

    exit_file = Path(args.exit_code)
    if not exit_file.exists():
        return fail("missing exit code file " + str(exit_file))
    raw = exit_file.read_text().strip()
    try:
        qemu_status = int(raw)
    except ValueError:
        return fail("invalid exit code " + repr(raw))
    if qemu_status != 33:
        return fail("QEMU exit status " + str(qemu_status) + " != 33")

    for marker in MARKERS:
        if marker not in text:
            return fail("missing serial marker " + repr(marker.decode()))
    if LISTEN_MARKER not in text:
        return fail("missing serial marker " + repr(LISTEN_MARKER.decode()))
    if echo.count < 2:
        return fail("expected two guest client connections, served " + str(echo.count))
    if "data" not in fetch_result:
        return fail("guest HTTP server check failed: " + fetch_result.get("error", "no response"))
    response = fetch_result["data"]
    if b"200 OK" not in response or SERVED_BODY not in response:
        return fail("unexpected guest HTTP response " + repr(response[:120]))
    print("ok: tcp client/server http (" + str(echo.count) + " echo connection(s), "
          + str(len(response)) + " byte(s) fetched from the guest, exit "
          + str(qemu_status) + ")")
    return 0


if __name__ == "__main__":
    sys.exit(main())
