# Networking: net.device, virtio-net and IPv4

The kernel owns a small IPv4 stack behind a device-neutral `net.device` layer.
Phase 4B delivers the first slice of the specification's Phase 4 networking
gate: a legacy virtio-net NIC, Ethernet framing, ARP, IPv4, ICMP echo, UDP and a
DHCP client, all exercised by a boot self-test against QEMU user-mode networking
(slirp). TCP, DNS and the socket layer are later increments.

```
        DHCP client          ICMP echo            (TCP, DNS, sockets: later)
             |                   |
             +--------+  +-------+
                      v  v
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
  `icmp_ping(gateway, seq 1, 3000 ms)`; failures print
  `CRUGOS: net selftest failed (dhcp|arp|icmp)` and the kernel panics with
  `selftest: network`.

A healthy gate prints:

```
CRUGOS: net ready (1 device(s))
CRUGOS: dhcp lease 10.0.2.15/255.255.255.0 gw 10.0.2.2 dns 10.0.2.3
CRUGOS: arp resolved 10.0.2.2 52:55:0a:00:02:02
CRUGOS: icmp echo reply from 10.0.2.2 seq=1 ttl=255
CRUGOS: net selftest ok
```

The boot ends with `CRUGOS: phase 4b boot ok`.

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

## Gates and tests

| Target | What it proves |
| --- | --- |
| `make test` | No NIC: the stack is skipped, all earlier markers still pass |
| `make test-net` | slirp NIC: DHCP lease, gateway ARP, one ICMP echo, then `e2fsck` on the disk |
| `make test-host` | Wire helpers, RFC 1071 checksums, DHCP option parsing (938 checks) |

`make test-net` runs the guest with `NET_MODE=user` and passes
`EXPECT_NET=yes` to `scripts/boot-check.py`, which then requires
`CRUGOS: net selftest ok` plus the lease, ARP and ICMP lines and their
address/sequence shapes.

## Known limits

- No TCP, DNS resolver, socket API or IPv6. No fragmentation or reassembly,
  ICMP errors, TCP-style retransmission or routing table.
- ARP entries never expire, and the cache holds eight destinations.
- DHCP renewal/rebind, lease expiry, NAK handling and option 121 style routes
  are not implemented; the client is boot-time only.
- One virtqueue pair per device, polling only, four devices, four UDP handlers,
  and no VLAN, checksum offload, scatter-gather or multi-queue negotiation.
- UDP payload checksums are calculated on transmit but not verified on receive;
  packets are not authenticated, so the stack is for bring-up and trusted
  networks.
