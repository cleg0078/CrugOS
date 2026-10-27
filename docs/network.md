# Networking: net.device, virtio-net, IPv4 and TCP

The kernel owns a small IPv4 stack behind a device-neutral `net.device` layer.
Phase 4B delivered the first slice of the specification's Phase 4 networking
gate: a legacy virtio-net NIC, Ethernet framing, ARP, IPv4, ICMP echo, UDP and a
DHCP client. Phase 4C adds a minimal TCP transport (`kernel/net/tcp.c`) with a
client, a listener, an HTTP exchange in both directions and six socket
syscalls, all exercised by a boot self-test against QEMU user-mode networking
(slirp). A DNS resolver is the next increment.

```
   DHCP client    ICMP echo    TCP client/server (six socket syscalls)
        |              |              |
        +--------------+--------------+
                       v
             net_udp_send / net_ipv4_send_raw / net_eth_send
                      |
              ARP cache (8 entries) + IPv4 header + UDP checksum
                      |
                  net_receive(nd, frame, len)
                      |
        +-------------+-------------+
        |                           |
   virtio-net (eth0)            (future NICs)
        |
   PCI I/O BAR, two virtqueues
```

## Why virtio-net first

- The specification lists VirtIO-net and Intel e1000 as the initial NICs. A
  legacy virtio-net device needs no firmware handshake, no MSI-X and no
  interrupt plumbing, so it reuses the polled virtqueue pattern already proven
  by `virtio_blk`.
- QEMU's user-mode network gives a deterministic peer without host privileges:
  the guest gets `10.0.2.15/24`, the gateway `10.0.2.2` answers ARP and ICMP, and
  DNS is `10.0.2.3`. That makes an end-to-end gate with no external network.
- Nothing in the stack depends on interrupts: `net_poll()` drains the receive
  ring, so the self-test runs before the scheduler starts.

## net.device

`include/crug/net.h` defines the interface. Addresses are stored host-order as
`0xAABBCCDD` for `A.B.C.D`; the wire helpers (`net_get16`, `net_get32`,
`net_put16`, `net_put32`) convert to and from big-endian.

```c
struct net_device {
    char name[NET_NAME_MAX];        /* "eth0" */
    u8 mac[NET_MAC_LEN];
    u16 mtu;
    int (*send)(struct net_device *nd, const void *frame, usize len);
    void (*poll)(struct net_device *nd);
    void *priv;
    struct net_device *next;
};
```

| Call | Purpose |
| --- | --- |
| `net_init()` | Zero the tables, register the DHCP UDP port, probe NIC drivers |
| `net_register(nd)` | Append a device to the list (names must be unique) |
| `net_poll()` | Ask every device to drain its receive ring |
| `net_receive(nd, frame, len)` | Ingress: MAC filter, then ARP or IPv4 |
| `net_eth_send()` | Ethernet frame with the device's source MAC and 60-byte pad |
| `net_ipv4_send_raw()` | 20-byte IPv4 header (ttl 64, incrementing id) |
| `net_udp_send()` | UDP header and checksum over the pseudo-header |
| `net_udp_register(port, handler)` | Bind one of four UDP ports |
| `net_config_get()` / `net_set_config()` | Current `ip`, `mask`, `gateway`, `dns` |
| `net_selftest()` | DHCP lease, gateway ARP, one ICMP echo |

Buffers are static (`NET_FRAME_MAX` = 1600 bytes per stage), so no packet
allocation happens on the receive path. `net_checksum()`/`net_checksum_add()`
live in `kernel/net/net_util.c` and are host-testable;
`net_dhcp_parse()` is there too.

## virtio-net driver

`drivers/net/virtio_net.c` drives a legacy virtio-net PCI function
(`0x1AF4:0x1000`), the same transport as `virtio_blk`:

- The status handshake is `ACK(1) -> DRIVER(2) -> FEATURES_OK(8) -> DRIVER_OK(4)`,
  with `FAILED(128)` checked after the feature and device steps.
- Exactly one feature is negotiated: `VIRTIO_NET_F_MAC` (bit 5), so the host
  assigns `52:54:00:12:34:56` to `eth0`. No offload, status or MQ features.
- Queue 0 is receive, queue 1 is transmit. Each queue gets its own descriptor
  table, available ring and used ring from `pmm_alloc_pages()`; the queue size is
  read from `QueueNumMax` and clamped to 256.
- Receive: 32 descriptors of 2048 bytes each are pre-posted with
  `VIRTQ_DESC_F_WRITE`. Each buffer starts with the 10-byte
  `virtio_net_hdr`, so the frame is `buf + 10` with length
  `used_len - 10`; completed buffers are handed to `net_receive()` and
  re-posted immediately.
- Transmit: the frame is copied behind a zeroed `virtio_net_hdr` in a single
  2048-byte bounce buffer, one descriptor is published on queue 1 and the driver
  waits up to 500 ms for the used ring to advance. A missing completion fails the
  send instead of blocking the boot.
- Up to four devices (`eth0`..`eth3`) are probed with `pci_find_nth()`; the
  driver is polled, so no IRQ is routed or shared.

## Protocol core

`kernel/net/net.c` keeps one ingress buffer per stage (`g_l3`, `g_frame`,
`g_udp_buf`) plus an 8-entry ARP cache.

- **Ethernet.** Frames shorter than 60 bytes are zero-padded. Ingress drops
  frames whose destination is neither our MAC nor the broadcast address.
- **ARP.** Requests carry our MAC and configured IP; replies are cached
  (8 entries, oldest slot reused) and requests for our own address are answered.
  `arp_resolve()` retries three times within its timeout budget, polling the NIC
  and halting between attempts.
- **IPv4.** Version and header length are validated, the total length is bounded
  by the frame, and the header checksum must fold to zero. There is no
  fragmentation, no option parsing and no routing table: callers supply the
  next-hop MAC. `ttl` is 64 and the identification field increments per send.
- **ICMP.** Only echo replies (type 0, code 0) with a valid checksum and the
  expected identifier/sequence are accepted; source address and TTL are recorded
  for the marker line.
- **TCP ingress.** Protocol 6 segments are handed to
  `tcp_receive_segment()` with the source and destination addresses the
  pseudo-header checksum needs; the transport owns the remainder.
- **UDP.** Lengths are bounds-checked, the destination port selects one of four
  handlers, and the source checksum is not verified. Transmit computes the
  pseudo-header checksum and substitutes `0xFFFF` for a zero result.

## DHCP client

`kernel/net/dhcp.c` implements the four-message exchange for a lease:

1. `dhcp_init()` registers UDP port 68 once.
2. Up to three attempts send a DISCOVER to `255.255.255.255:67` with
   `xid = 0x43520000 | attempt << 8 | (ticks & 0xFF)`, the broadcast flag,
   the interface MAC in `chaddr` and options `53` (message type) and `55`
   (parameter request list: mask, router, DNS).
3. An OFFER records `yiaddr`, server identifier, mask, router and DNS, then a
   REQUEST repeats them as options `50` and `54`; an ACK fills
   `struct net_config` (mask defaults to `255.255.255.0` when omitted).
4. Every wait loop polls the NIC and halts the CPU between checks, so the
   client is safe before the scheduler exists.

Only option 1 (mask), 3 (router), 6 (DNS) and 54 (server) are parsed; lease
time, renewal and NAK are not acted on beyond failing the exchange.

## TCP transport

`kernel/net/tcp.c` is deliberately small: four sockets (`NET_TCP_MAX`), 4 KiB
of receive ring per socket (`NET_TCP_RX_BYTES`), a 1400-byte segment limit
(`NET_TCP_MSS`) and no options other than the 20-byte fixed header.

- **Sockets.** `tcp_socket_new()` takes the first free slot, inherits
  `net_config_get()->ip` as the local address and assigns the ephemeral port
  `49152 + slot`. States run `CLOSED -> SYN_SENT/LISTEN -> SYN_RECEIVED ->
  ESTABLISHED -> CLOSE_WAIT/FIN_WAIT -> TIME_WAIT`, matching
  `enum net_tcp_state` in `include/crug/net.h`. The receive window advertised
  in every segment is the free space left in the ring.
- **Handshake.** `tcp_connect()` picks an initial sequence number from
  `timer_ticks() ^ counter` (`counter` advances by 0x9E3779B9 per socket), sends
  SYN, waits for SYN+ACK and acknowledges. Retries reuse the same ISN; a RST or
  a timeout after five attempts (`NET_TCP_RETRIES`) fails the connect.
- **Ingress.** `tcp_receive_segment()` verifies the pseudo-header checksum
  (`net_tcp_checksum_ok`), matches the 4-tuple, handles RST, completes both
  handshakes, accepts in-order payload into the ring and acknowledges it, ACKs
  out-of-order payload without buffering it, and moves to `CLOSE_WAIT` or
  `TIME_WAIT` on FIN.
- **Send.** `tcp_send()` splits the buffer into `NET_TCP_MSS` chunks and
  advances `snd_nxt` with each send (the peer's ACK must land inside
  `[snd_una, snd_nxt]`). Each chunk is retransmitted up to five times with a
  500 ms acknowledgement wait (`TCP_WAIT_ACK_MS`). There is no congestion
  window, no Nagle, no SACK and no window probing: the sender ignores
  `snd_wnd` beyond recording it.
- **Receive.** `tcp_recv()` drains the ring and, when it is empty, polls the NIC
  and halts the CPU until data arrives, the peer closes or the timeout expires.
- **Close.** `tcp_close()` sends FIN+ACK once in `ESTABLISHED`/`CLOSE_WAIT`,
  then marks the slot `CLOSED`; the socket is freed with
  `tcp_socket_release()`.
- **Listener.** `tcp_listen()` parks a socket in `LISTEN`; an incoming SYN
  allocates a child socket directly in `SYN_RECEIVED` and replies SYN+ACK, and
  `tcp_accept()` polls until such a child reaches `ESTABLISHED`, returning it
  once.
- **Command line.** `tcp_cmdline()` runs from `net_init()` and reads two
  non-secret tokens from the loader command line: `net.tcp=<port>` names the
  remote peer for the client leg (1..65535), `net.serve=<seconds>` bounds how
  long the guest listener waits for the gate's connection (1..3600). Without
  `net.tcp=` the TCP self-test prints `CRUGOS: tcp selftest skipped` and the boot
  still passes.

`tcp_selftest()` runs inside `net_selftest()` after the ICMP check and walks
four steps, every one of which must succeed:

1. `tcp_listen()` on port 8080 (`NET_TCP_DEFAULT_HTTP_PORT`).
2. `tcp_connect()` to the gateway on `net.tcp=<port>`, then send
   `GET / HTTP/1.0` + `Host: crugos` and check that the reply contains
   `200 OK` and `crugos-tcp-ok`.
3. `syscall_socket_selftest()` repeats the exchange through
   `SYS_SOCKET_CONNECT/SEND/RECV/CLOSE` (numbers 9..12) against the same peer,
   proving the handle-table path works.
4. `tcp_accept()` the gate's own connection and answer it with
   `HTTP/1.0 200 OK` and a short body.

```
CRUGOS: tcp listen ready (port 8080)
CRUGOS: tcp connect ok (10.0.2.2:52847)
CRUGOS: tcp http ok (53 bytes)
CRUGOS: tcp socket syscall ok
CRUGOS: tcp http served (72 bytes)
```

## Socket syscalls

`kernel/core/syscall.c` exposes six calls on top of the transport, all using
typed handles (`HANDLE_SOCKET`) in the process handle table:

| Number | Call | Arguments | Returns |
| --- | --- | --- | --- |
| 9 | `SYS_SOCKET_CONNECT` | `ip`, `port` | socket handle, `SYS_EIO`/`SYS_ENOMEM` |
| 10 | `SYS_SOCKET_SEND` | `handle`, `buf`, `len` | bytes sent |
| 11 | `SYS_SOCKET_RECV` | `handle`, `buf`, `len` | bytes received (timeout 5000 ms) |
| 12 | `SYS_SOCKET_CLOSE` | `handle` | 0, or `SYS_EBADF` |
| 13 | `SYS_SOCKET_LISTEN` | `port` | listening handle |
| 14 | `SYS_SOCKET_ACCEPT` | `handle` | accepted handle, or `SYS_EAGAIN` |

`HANDLE_CLOSE` on a socket handle closes the connection before dropping the
handle. There is no `bind`, no address struct and no non-blocking mode: the
queue length, addresses and timeouts are fixed in the kernel.

## Boot integration

`kernel/core/main.c` probes the stack right after `input_init()`:

```
CRUGOS: input ready (serial + keyboard)
virtio : net eth0 mac 52:54:00:12:34:56
CRUGOS: net ready (1 device(s))
```

`selftest_run()` then calls `net_selftest()` after the storage checks:

- no device: `CRUGOS: net selftest skipped (no network device)` and the test
  passes, so an ordinary boot without a NIC stays green;
- `dhcp_acquire(1000 ms)`, `arp_resolve(gateway, 3000 ms)` and
  `icmp_ping(gateway, seq 1, 3000 ms)`; then `tcp_selftest()`, which is skipped
  unless the loader command line carries `net.tcp=<port>`; failures print
  `CRUGOS: net selftest failed (dhcp|arp|icmp|tcp listen|tcp connect|tcp http|
  tcp socket|tcp serve)` and the kernel panics with `selftest: network`.

A healthy gate prints:

```
CRUGOS: net ready (1 device(s))
CRUGOS: dhcp lease 10.0.2.15/255.255.255.0 gw 10.0.2.2 dns 10.0.2.3
CRUGOS: arp resolved 10.0.2.2 52:55:0a:00:02:02
CRUGOS: icmp echo reply from 10.0.2.2 seq=1 ttl=255
CRUGOS: tcp listen ready (port 8080)
CRUGOS: tcp connect ok (10.0.2.2:52847)
CRUGOS: tcp http ok (53 bytes)
CRUGOS: tcp socket syscall ok
CRUGOS: tcp http served (72 bytes)
CRUGOS: net selftest ok
```

The TCP lines appear only in a gate that passes `net.tcp=`; an ordinary
`make test` or `make test-net` boot has no `tcp listen` line and stays green.
The boot ends with `CRUGOS: phase 4c boot ok`.

## QEMU wiring

`scripts/run-qemu.sh` maps `NET_MODE=user` to:

```
-netdev user,id=net0
-device virtio-net-pci,netdev=net0,disable-modern=on,romfile=
```

`disable-modern=on` selects the legacy virtio transport the driver speaks, and
`romfile=` drops the iPXE option ROM so the NIC adds no firmware boot path or
dead time. Boots without `NET_MODE` keep the old command line, so every earlier
gate is unaffected (the serial log then has no `virtio : net` line).

The TCP gate needs a peer the guest can reach and a way to reach the guest:
`scripts/run-net-tcp.py` starts an HTTP responder on a free `127.0.0.1` port
(the guest sees slirp's `10.0.2.2` as the host loopback), passes it to the guest
as `net.tcp=<port> net.serve=<seconds>` through `CMDLINE_EXTRA`, and sets
`NET_HOSTFWD=tcp:127.0.0.1:<forward port>-:8080` so the same script can fetch
the guest's own listener. `run-qemu.sh` maps that variable to
`-netdev user,...,hostfwd=...`; the `tcp:` protocol prefix is required by QEMU.

## Gates and tests

| Target | What it proves |
| --- | --- |
| `make test` | No NIC: the stack is skipped, all earlier markers still pass |
| `make test-net` | slirp NIC: DHCP lease, gateway ARP, one ICMP echo, then `e2fsck` on the disk |
| `make test-tcp` | A responding host peer plus a host fetch of the guest listener: two client connections complete an HTTP exchange, the socket syscalls work, the guest serves one request, then `e2fsck` |
| `make test-host` | Wire helpers, RFC 1071 checksums, TCP/UDP checksums, DHCP option parsing (943 checks) |

`make test-net` runs the guest with `NET_MODE=user` and passes
`EXPECT_NET=yes` to `scripts/boot-check.py`, which then requires
`CRUGOS: net selftest ok` plus the lease, ARP and ICMP lines and their
address/sequence shapes. `make test-tcp` adds `EXPECT_TCP=yes`, which requires
the five `CRUGOS: tcp ...` lines with `(port N)`, `(a.b.c.d:port)`,
`(N bytes)` and `(N bytes)` shapes, and the driver exits 0 only when both
legs succeeded (`ok: tcp client/server http`).

## Known limits

- No DNS resolver, no IPv6, no fragmentation or reassembly, no ICMP errors and
  no routing table. TCP is an in-order bring-up transport: no options beyond the
  fixed header, no congestion control, no SACK, no TIME_WAIT timer (the slot is
  reclaimed immediately), no window probing, out-of-order segments are dropped
  rather than buffered, and the ISN is not randomized to cryptographic quality.
- Sockets are limited to the six syscalls above, four concurrent sockets and
  fixed 5000 ms accept/recv timeouts; there is no `bind`, `setsockopt` or
  non-blocking mode.
- ARP entries never expire, and the cache holds eight destinations.
- DHCP renewal/rebind, lease expiry, NAK handling and option 121 style routes
  are not implemented; the client is boot-time only.
- One virtqueue pair per device, polling only, four devices, four UDP handlers,
  and no VLAN, checksum offload, scatter-gather or multi-queue negotiation.
- UDP payload checksums are calculated on transmit but not verified on receive;
  packets are not authenticated, so the stack is for bring-up and trusted
  networks.
