# Security: the fix programme

CrugOS reached a point where the adversarial surface is real: a ring 3 with
spawn, exec, fork and copy-on-write, a network stack that answers unsolicited
packets from anywhere, and a package installer that writes to the filesystem on
behalf of a signed container. Phase S1 is the first security pass over that
surface. It is a fix programme, not a feature: every item starts as a written
finding, ends as a one-line marker the boot checks can assert, and leaves a gate
behind so the bug cannot come back silently.

The findings come from `specs/fix-p2/`: `REPORT.md` plus one file per area
under `findings/`, a review of the tree at `a1a99df`. That review raised one
Critical, ten High, sixteen Medium, fourteen Low and nine Info items. S1 does
not fix all of them. It fixes the Critical, the four Highs that a remote or
ring-3 attacker can reach directly, and the Mediums that are cheap and local.

## Why a security programme

The threat model changed twice. Phase U1 made ring 3 live: a user program can
now hand the kernel any pointer, any syscall number and, since S1's own H8 fix,
any trap vector. Phase 4D made the network remote-reachable and unauthenticated:
the DHCP client, the DNS resolver and the TCP stack all process packets that
nobody asked for, and the kernel has no firewall, no user and no login.

That combination means a bug is not a crash, it is a foothold. An unbounded
recursion in the DNS decoder runs on the 16 KiB kernel stack; a forged reply is
accepted as an answer; four unsolicited SYNs fill the TCP table; one `int $4`
from CPL3 panics the machine. None of those need credentials.

So the programme has three rules:

- **Prove it, do not hope it.** A fix is not done when the code looks right. It
  is done when an opt-in self-test provokes the old bug, the boot prints a
  marker that could only appear after the fix, and a gate fails if the marker
  goes away.
- **One switch per behaviour.** Self-tests never run in a normal boot. A boot
  token asks for them (`dns.test=hostile`, `user.test=8`, `net.synflood=1`),
  a `boot-check.py` flag requires their markers, and every other gate fails if
  those markers appear uninvited. A security test therefore cannot quietly
  weaken an ordinary boot.
- **Leave the deferral written down.** A finding that S1 does not fix is named
  in this file and in the roadmap, with the reason. Silence would look like a
  fix.

## How a phase works

Each fix follows the same path:

1. Reproduce the finding from `specs/fix-p2/findings/` against the current
   tree, or establish that the report is wrong (see the rejected finding
   below). A finding is a hypothesis until the source agrees.
2. Change the smallest thing that removes the class of bug, not just the
   instance: an iterative decoder instead of a depth cap, a validated port
   instead of a fixed one, a reaper instead of a bigger table.
3. Add a self-test behind a boot token, with markers that state the attack and
   the outcome in one line each.
4. Teach `scripts/boot-check.py` the marker set behind a flag, and make the
   flag's absence reject the markers.
5. Add a gate target so the whole thing is one `make` command, and document it
   here and in [boot.md](boot.md), [network.md](network.md) and
   [traps.md](traps.md).

The next review produces the next set of findings in `specs/fix-p3/`. Findings
are never deleted when they are fixed; they are marked done with the phase that
closed them.

## Phase S1 (v0.48.0)

S1 is the first pass. It closes the Critical, four Highs and the Mediums listed
below, adds three gates (66, 67 and 68) and leaves the Highs that need a
network-locking or handle-lifetime redesign to S2.

### C1: DNS name decompression is iterative

The DNS decoder followed compression pointers recursively. A 12-link chain
worked, a 1200-link chain consumed the 16 KiB kernel stack and the machine
died. The fix in `kernel/net/dns.c` is structural, not a bigger limit:
`dns_decode_name()` is a loop with one hop budget for the whole name
(`DNS_MAX_HOPS`, 16) and every pointer must move strictly backwards, so a
cycle cannot be built at all. A chain of 12 still decodes; 1200 links are
rejected without touching the stack.

```
CRUGOS: dns hostile chain ok (12 link(s) decoded, 1200 link(s) rejected)
CRUGOS: dns hostile source check ok (3 forged reply(ies) ignored)
CRUGOS: dns hostile selftest ok
```

### H1: DNS replies are checked against the query

The resolver accepted any DNS reply that arrived, from any address, with any
transaction id. A spoofed answer is a trivial cache poison. A reply is now
accepted only when its source IP is the server the query went to, its source
port is that server's port, its destination port is the local port the query
used, and its id matches. Ids come from an xorshift64 stream
(`dns_rng_next()`) seeded from the bootloader entropy seed mixed with the TSC,
and the resolver keeps `NET_DNS_LOCAL_PORTS` (2) local ports instead of the
old fixed 53000, so a blind guess has to hit a moving target.

The same marker block above reports the source check: three forged replies are
injected and all three are ignored.

### H4: TCP table exhaustion

`NET_TCP_MAX` is 4. Four unsolicited SYNs, each answered with a half-open
socket, filled the table and the stack could no longer accept a real
connection. Two changes in `kernel/net/tcp.c` undo that:

- `tcp_listen_input()` answers an allocation failure with RST|ACK
  (`tcp_send_reset()`) and counts an overflow, so the attacker learns nothing
  and the table is not silently wedged.
- `tcp_reap_stale()` releases `SYN_SENT` and `SYN_RECEIVED` sockets older
  than `NET_TCP_SYN_TIMEOUT_NS` (10 s), and runs from `tcp_socket_new()`, so a
  later real connection reclaims the slot.

RST handling is validated too: `tcp_rst_valid()` accepts an RST in
`SYN_SENT` only when its ack equals `snd_nxt`, and otherwise only when the
sequence number lies inside the receive window, so an off-path RST cannot kill
a healthy connection.

```
CRUGOS: tcp syn flood selftest ok (1 overflow(s), 3 reaped)
```

### H8: every user trap vector has a gate

Ring 3 could panic the machine with one instruction. Vector 4 (`#OF`) was
classified `TRAP_HANDLED` but `trap_dispatch()` had no handled branch and fell
through to the fatal path; worse, only vectors 0-32 and 128 had IDT gates, so
any other `int $n` from CPL3 was a triple fault. `kernel/arch/x86_64/idt.c`
now installs a shared `isr_ignore` stub on gates 33 through 255 (the syscall
vector excluded) and the kernel prints `CRUGOS: idt gates 256 present`. The
policy maps a user-mode vector 4 to `TRAP_USER` and `trap_dispatch()` returns
for `TRAP_HANDLED`, so the fault is charged to the thread instead of the
machine.

The probe is `/bin/trap4`: it executes `int $4` from CPL3 (`INTO` is illegal
in 64-bit mode, so the interrupt instruction is the honest test). The thread is
killed with code -4 and the system keeps running.

```
CRUGOS: user trap4 probe (int $4 from ring 3)
CRUGOS: user fault overflow at rip=... err=...
CRUGOS: user fault killed tid N code -4
CRUGOS: user program /bin/trap4 finished (exit -4)
```

### H9: package ids are validated

`pkg_install()` never validated the package id it read from the container. An
id of `..` made the staging directory resolve out of `/Applications`, so an
install could write outside its own tree. `pkg_id_valid()` (declared in
`include/crug/pkg.h`) now rejects an empty id, `.`, `..` and any path
separator, and runs before the filesystem is touched. The host tests cover the
positive ids `captest`, `a.b`, `a_b-c` and `Caps1`, and the negative ids
`NULL`, `""`, `.` and `..`.

### The Medium findings

Three Mediums were fixed in the same pass because they are local and cheap:

- **UDP verdicts.** A datagram is accepted only when its destination address is
  one of ours or broadcast, and a non-zero UDP checksum is verified over the
  IPv4 pseudo-header (`net_udp_checksum()` / `net_udp_checksum_ok()` in
  `kernel/net/net_util.c`, shared with the transmit path). A zero checksum
  stays legal, as RFC 768 requires.
- **virtio-net used length.** `drivers/net/virtio_net.c` clamps the device's
  `used_len` to `VIRTIO_NET_BUF_SIZE` before parsing, so a malicious or buggy
  device cannot make the stack read past the buffer it was given.
- **Message-port truncation.** `msgport_recv()` in `kernel/core/msgport.c`
  returns the number of bytes actually copied instead of the full message
  length, so `SYS_PORT_RECV` no longer reports bytes it did not write. This
  repairs the P1-F12 truncation report rather than a fresh finding.

## What S1 does not fix

These are known and deliberately deferred to S2. They are not hypothetical:
each one has a finding in `specs/fix-p2/findings/`.

- **H2, no network locking.** `net_poll()` is not re-entrant: a second entry
  clobbers the shared `g_l3`, `g_frame` and `g_udp_buf` buffers and the
  socket array. Fixing it means a locking or single-owner redesign of the
  whole receive path, not a patch.
- **H3, ARP slot eviction and stack exhaustion.** The ARP cache can evict slot
  0, and the TCP receive path emits ACKs by calling back into `net_poll()`,
  which can recurse until the kernel stack is gone. H2's redesign is the
  prerequisite.
- **H5 and H6, port lifetime.** `SYS_HANDLE_CLOSE` never destroys a message
  port, and thread exit tears down no handles at all, so ports and their
  memory leak and a closed port can be used after free.
- **H7, the handle table.** One global 256-entry table with no per-process
  rights checks: any process can name any handle number.
- **H10, lost wakeup.** A wakeup delivered while the receiver is between its
  check and its block is lost, and a closed port can be woken after free.
- **The remaining Mediums:** unwaited zombies pin memory, there is no
  per-process memory quota, the `kfree()` large path trusts its argument, and
  the ephemeral port choice is deterministic.

The Low and Info items are recorded in the review and are not scheduled.

## Rejected findings

One report in `specs/fix-p2/findings/devices.md` is wrong and is recorded here
so it is not "fixed" into a regression. It claims the GPU `GET_EDID` command
reads the size field at offset 24 but should read it at offset 28.

`struct virtio_gpu_ctrl_hdr` is 24 bytes: it is `type` (4), `flags` (4) and
`fence_id` (8) and `ctx_id` (8). `struct virtio_gpu_get_edid` therefore has
`size` at offset 24 and `edid[]` at offset 32, exactly as
`drivers/gpu/virtio_gpu.c` builds it. The reference is
`/usr/include/linux/virtio_gpu.h`, lines 354-359. Moving the field to 28 would
truncate the length and read four bytes of padding as data.

## Gates

Phase S1 adds three gates. They share `S1_RUN_TIMEOUT_SEC` (240) as the
wall-clock budget and each one is a single boot with its own token:

```
make test-dns-hostile   # decode a hostile compression chain and refuse forged replies
make test-trap-vector   # execute int $4 from ring 3 and kill only the thread
make test-net-synflood  # fill the TCP table and prove the reaper frees it
```

Gate 66 boots with `dns.test=hostile` and runs `boot-check.py
--expect-dnshostile=yes`; the boot must print the chain and source-check
markers above and end `dnshostile yes ... QEMU exit status 33`. Gate 67 boots
with `user.test=8` and `boot-check.py --expect-trapvector=yes`; it requires
`CRUGOS: idt gates 256 present`, the `user trap4 probe` line, a
`user fault overflow`, a `user fault killed` line and the exact
`/bin/trap4 finished (exit -4)` marker, plus the whole ring-3 chain from the
earlier user gates (`user yes isolated yes ushell yes spawn yes fork yes
forkbig yes`). Gate 68 boots with `net.synflood=1` and
`--expect-synflood=yes`; the self-test line is matched as
`CRUGOS: tcp syn flood selftest ok (N overflow(s), M reaped)`.

In a normal boot none of the three run, and every other gate fails if their
markers appear: the flags are opt-in, and the absence of a flag is a
prohibition, not a default.

## Next

S2 takes the deferred list. The order is dictated by dependencies: the network
lock (H2) first, because ARP and TCP re-entry (H3) cannot be fixed on top of a
re-entrant receive path; then the handle and port lifetime work (H5, H6, H7,
H10) as one redesign, because a per-process table, teardown at exit and port
destruction are the same change seen from three sides; then the Mediums.

Each item gets the S1 treatment: reproduce, fix the class, mark it, gate it,
document it. The review for the next pass lives in `specs/fix-p3/`, and this
file grows a section per phase rather than being rewritten.
