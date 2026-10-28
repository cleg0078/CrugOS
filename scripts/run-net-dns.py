#!/usr/bin/env python3
"""Drive the phase 4D DNS gate: a hermetic name lookup over the slirp network.

The guest boots with a legacy virtio-net NIC on QEMU's user-mode network and a
loader command line that points the resolver at the host:

    net.dns=10.0.2.2:<port> net.dns-tcp=1 net.name=<name>

slirp redirects TCP connections to 10.0.2.2 to the host loopback (UDP is not
redirected, which is why the gate forces the TCP transport).  This driver plays
a DNS server on 127.0.0.1:<port>, answers the A query for <name> with
<address>, and checks the guest's serial markers: the resolve, the cache hit,
the resolve syscall and the boot self-test all have to report success.

The serial console is carried over SERIAL_TCP so the driver can follow the boot.
"""

import argparse
import re
import socket
import struct
import subprocess
import sys
import threading
import time
from pathlib import Path

MARKERS = (
    b"CRUGOS: dns server",
    b"CRUGOS: dns resolved",
    b"CRUGOS: dns cache hit",
    b"CRUGOS: dns socket syscall ok",
    b"CRUGOS: dns selftest ok",
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


def skip_name(msg, pos):
    """Return the offset just past the dotted name that starts at pos."""
    p = pos
    while p < len(msg):
        length = msg[p]
        if length == 0:
            return p + 1
        if length & 0xC0 == 0xC0:
            return p + 2
        p += 1 + length
    return len(msg)


def parse_query(query):
    """Return (id, name, qtype) for one DNS query, or None if malformed."""
    if len(query) < 12:
        return None
    ident = struct.unpack(">H", query[0:2])[0]
    qend = skip_name(query, 12)
    if qend + 4 > len(query):
        return None
    labels = []
    p = 12
    while p < qend and query[p]:
        length = query[p]
        labels.append(query[p + 1:p + 1 + length].decode("ascii", "replace"))
        p += 1 + length
    qtype = struct.unpack(">H", query[qend:qend + 2])[0]
    return ident, ".".join(labels), qtype


def build_response(query, address, ttl):
    """Answer the question in query with a single A record."""
    parsed = parse_query(query)
    if parsed is None:
        return b""
    ident, _name, qtype = parsed
    if qtype != 1:
        return b""
    qend = skip_name(query, 12) + 4
    question = query[12:qend]
    header = struct.pack(">HHHHHH", ident, 0x8180, 1, 1, 0, 0)
    answer = (b"\xc0\x0c" + struct.pack(">HHIH", 1, 1, ttl, 4)
              + socket.inet_aton(address))
    return header + question + answer


class DnsServer(threading.Thread):
    """A minimal DNS server on TCP/53 semantics, answering A queries."""

    def __init__(self, port, address, ttl):
        super().__init__(daemon=True)
        self.port = port
        self.address = address
        self.ttl = ttl
        self.queries = []
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

    def recv_exact(self, conn, count):
        data = b""
        while len(data) < count:
            chunk = conn.recv(count - len(data))
            if not chunk:
                return None
            data += chunk
        return data

    def handle(self, conn):
        try:
            conn.settimeout(10.0)
            prefix = self.recv_exact(conn, 2)
            if prefix is None:
                return
            (length,) = struct.unpack(">H", prefix)
            if length == 0 or length > 512:
                return
            query = self.recv_exact(conn, length)
            if query is None:
                return
            parsed = parse_query(query)
            if parsed is not None:
                self.queries.append(parsed[1])
            response = build_response(query, self.address, self.ttl)
            if not response:
                return
            conn.sendall(struct.pack(">H", len(response)) + response)
        except OSError:
            pass
        finally:
            try:
                conn.close()
            except OSError:
                pass


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", default=".")
    parser.add_argument("--disk", required=True)
    parser.add_argument("--serial-log", required=True)
    parser.add_argument("--exit-code", required=True)
    parser.add_argument("--name", default="crugos.test")
    parser.add_argument("--address", default="10.0.2.99")
    parser.add_argument("--ttl", type=int, default=60)
    parser.add_argument("--run-timeout", type=int, default=180)
    parser.add_argument("--connect-timeout", type=float, default=30.0)
    args = parser.parse_args()

    dns_port = free_port()
    serial_port = free_port()

    server = DnsServer(dns_port, args.address, args.ttl)
    server.start()

    cmdline = ("net.dns=10.0.2.2:" + str(dns_port) + " net.dns-tcp=1"
               + " net.name=" + args.name)
    command = [
        "make", "--no-print-directory", "run",
        "DISK_IMG=" + args.disk,
        "CRYPT_PASS=",
        "NET_MODE=user",
        "CMDLINE_EXTRA=" + cmdline,
        "SERIAL_TCP=127.0.0.1:" + str(serial_port),
        "QEMU_DISPLAY=none",
        "TIMEOUT_SEC=" + str(args.run_timeout),
    ]
    print("run-net-dns: dns port", dns_port, "serial port", serial_port)
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
    finally:
        serial.close()

    try:
        process.wait(timeout=args.run_timeout)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()
    server.stop.set()

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
    pattern = (r"dns resolved " + re.escape(args.name) + r" -> "
               + re.escape(args.address) + r" via (udp|tcp|cache)")
    if not re.search(pattern, text.decode("utf-8", "replace")):
        return fail("dns resolved marker does not name " + args.name + " -> "
                    + args.address)
    if args.name not in server.queries:
        return fail("the guest never asked the server for " + args.name)
    print("ok: dns resolve over tcp (" + str(len(server.queries)) + " query(s), "
          + args.name + " -> " + args.address + ", exit " + str(qemu_status) + ")")
    return 0


if __name__ == "__main__":
    sys.exit(main())
