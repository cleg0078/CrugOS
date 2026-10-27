# Versioning

CrugOS uses [Semantic Versioning 2.0.0](https://semver.org/) with annotated
`vMAJOR.MINOR.PATCH` Git tags. The current release is `v0.17.0` (phase 4C: a
minimal TCP transport behind `net.device` -- client, listener, an HTTP exchange
in both directions and six socket syscalls, gated end to end against a host peer
-- on top of the phase 4B polled virtio-net driver with Ethernet, ARP, IPv4,
ICMP echo, UDP and a DHCP client, the phase 4A PS/2 keyboard input and the
phase 3B-10
AES-256-XTS `crypt.device` with Argon2id keyslots, a checksummed crash-consistent filesystem over virtio-blk, NVMe, AHCI
and RAID1/RAID10 roots, and loader/token passphrase handling with redaction and
key zeroization).

## Source of truth

The repository file [`VERSION`](VERSION) is the only place a release number is
edited. The build reads it and passes `-DCRUG_VERSION_STR="<version>"` to the
kernel and the UEFI loader; `include/crug/version.h` supplies `0.0.0-dev` when
the compiler is invoked directly. The kernel prints the value on boot as
`CRUGOS: version <version>`, which the `make test` gate compares against
`VERSION`.

```
make version     # print the version the tree would build
```

## What the numbers mean

| Component | Bump when |
| --- | --- |
| MAJOR | a stable ABI breaks: the `boot_info` layout, the syscall dispatcher, the on-disk/handle interfaces, or the package format |
| MINOR | a phase or feature lands without breaking those ABIs |
| PATCH | bug fixes, documentation, build and test changes |

Pre-release suffixes (`0.2.0-rc1`) are allowed for release candidates. Once
`v1.0.0` exists, the kernel rejects a `boot_info` whose major version it does
not understand; before that, loader and kernel are always built and shipped
as a pair.

`BOOTINFO_VERSION` (currently 1) is versioned separately from the product: it
must be bumped whenever `struct boot_info` or a nested structure changes
layout or meaning, independently of whether the release is major, minor or
patch.

## Branches and tags

- `main` always builds and passes `make test`.
- Work happens on `feature/*` branches and reaches `main` by merge.
- A release is an annotated tag on `main`; tags are pushed explicitly.

## Release checklist

```
vi VERSION                       # pick the new SemVer
make distclean && make test      # clean build, boot gate, guest version check
make test-raid test-raid-degraded # RAID1 healthy and degraded gates
make test-raid10 test-raid10-degraded  # RAID10 healthy and degraded gates
make test-raid-repair test-raid10-repair  # cold-spare rebuild gates
make test-nvme                   # NVMe-only root boot and disk check
make test-ahci                   # SATA-only root boot and disk check
make test-journal-csum test-journal-replay  # JBD2 crash/replay gates
make test-crypt test-crypt-recovery test-crypt-wrongkey  # encrypted root
make test-crypt-tamper test-crypt-ni  # MAC tamper and AES-NI gates
make test-crypt-prompt test-crypt-prompt-wrong test-crypt-prompt-timeout  # console unlock
make test-crypt-keyboard test-crypt-keyboard-wrong  # PS/2 keyboard unlock
make test-net                    # virtio-net DHCP/ARP/ICMP boot gate
make test-tcp                    # TCP client/server HTTP gate
make size check-efi test-host repro
make check-disk                  # e2fsck + persisted files on the test disk
git add -A && git commit -m "release: vX.Y.Z"
git tag -a vX.Y.Z -m "CrugOS vX.Y.Z"
git push origin main vX.Y.Z
```

## Released versions

| Version | Date | Contents |
| --- | --- | --- |
| `v0.1.0` | 2026-10-27 | phase 0: UEFI loader, ELF handoff, BootInfo, serial + framebuffer console, boot report, reproducible build and boot gates |
| `v0.2.0` | 2026-10-27 | phase 1: bitmap physical allocator, identity page tables with W^X, GDT/IDT and exceptions, LAPIC timer and TSC calibration, AVX2 baseline; BootInfo stays v1 |
| `v0.3.0` | 2026-10-27 | phase 2: kernel heap, threads and scheduling classes, handles, MsgPort IPC, single `int 0x80` syscall dispatcher; BootInfo stays v1 |
| `v0.4.0` | 2026-10-27 | phase 3A: block layer, legacy virtio-blk, GPT, read-only ext4, generated test disk; BootInfo stays v1 and the kernel ABI is unchanged |
| `v0.5.0` | 2026-10-27 | phase 3B-1: writable ext4 (create/mkdir/write/truncate/unlink/rmdir/rename/fsync), virtio flush, boot write self-test and `e2fsck` disk gate; BootInfo stays v1 and the kernel ABI is unchanged |
| `v0.6.0` | 2026-10-27 | phase 3B-2: JBD2 journal replay, ordered metadata journaling, dirty-journal mount recovery and a replay self-test; BootInfo stays v1 and the kernel ABI is unchanged |
| `v0.7.0` | 2026-10-27 | phase 3B-3: multiple virtio-blk devices, checksummed RAID1 metadata, `md0` assembly, healthy/degraded root boot and host RAID gates; BootInfo stays v1 and the kernel ABI is unchanged |
| `v0.8.0` | 2026-10-27 | phase 3B-4: polled NVMe driver (64-bit BAR mapping, admin/I/O queues, namespace 1, PRP I/O, flush), NVMe-only root boot and `make test-nvme`; BootInfo stays v1 and the kernel ABI is unchanged |
| `v0.9.0` | 2026-10-27 | phase 3B-5: RAID10 metadata (level/chunk), mirror-pair mapping, four-member healthy and two-member degraded `md0` boots, host image builder/checker gates; BootInfo stays v1 and the kernel ABI is unchanged |
| `v0.10.0` | 2026-10-27 | phase 3B-6: cold-spare RAID rebuild at boot (blank member detection, lowest missing slot, 64-sector copy from the mirror, kernel-written metadata), `raid-blank.sh` and healthy/degraded repair gates; BootInfo stays v1 and the kernel ABI is unchanged |
| `v0.11.0` | 2026-10-27 | phase 3B-7: polled AHCI/SATA driver (class 01/06/01, port bring-up, 48-bit LBA IDENTIFY, DMA EXT commands, 8 KiB bounce), SATA-only root boot and `make test-ahci`; BootInfo stays v1 and the kernel ABI is unchanged |
| `v0.12.0` | 2026-10-27 | phase 3B-8: ext4 `metadata_csum` (superblock, group descriptors, bitmaps, inodes, directory/extent tails), JBD2 checksum v3 journals (tags, superblock, descriptor tail, commit block), multi-descriptor transactions, boot-time journal replay from a crash image, and `make test-journal-csum` / `make test-journal-replay`; BootInfo stays v1 and the kernel ABI is unchanged |
| `v0.13.0` | 2026-10-27 | phase 3B-9: `crypt.device` (GPT type `8f1c7d2e-...`, 16 KiB header, eight keyslots, AES-256-XTS data area with data-relative tweaks), Argon2id (RFC 9106) key derivation, 64-byte master key wrapped per slot with a keyed BLAKE2b-256 MAC and a recovery passphrase, AES-NI/software dispatch pinned to IEEE 1619 vector 10, loader `\crugos\cmdline.txt` with `crypt.pass=` plus redaction and key zeroization, `mkcrypt` host tool, `mkdisk.sh crypt` mode, and five encrypted-root gates; BootInfo stays v1 and the kernel ABI is unchanged |
| `v0.14.0` | 2026-10-27 | phase 3B-10: masked serial-console passphrase prompt when the ESP carries no `crypt.pass=`, shared line editor (`kernel/lib/lineedit.c`) with CR/LF, backspace, Ctrl-C and overflow handling, three attempts bounded by `crypt.prompt_timeout=`, fail-closed `crypt locked` markers, `SERIAL_TCP` serial capture and the `run-crypt-prompt.py` driver behind three new gates (correct, wrong, timeout); BootInfo stays v1 and the kernel ABI is unchanged |
| `v0.15.0` | 2026-10-27 | phase 4A: `input.device` (set-1 scancode decoder, Shift/Ctrl/Alt/Caps state, US layout, control-code mapping, 64-byte queue), polled i8042 keyboard driver, `console_try_getc()` merging serial and keyboard input, `CRUGOS: input ready (serial + keyboard)` marker and console prompt wording, QMP `send-key` driver (`run-crypt-keyboard.py`) behind two new keyboard gates and 903 host checks; BootInfo stays v1 and the kernel ABI is unchanged |
| `v0.17.0` | 2026-10-27 | phase 4C: TCP transport (`kernel/net/tcp.c`: four sockets, 1400-byte segments, 4 KiB receive rings, connect with bounded SYN retries, listen/accept, send with bounded retransmission, in-order receive, close), `tcp_selftest()` client/listener HTTP exchange selected by `net.tcp=`/`net.serve=`, six socket syscalls (`SYS_SOCKET_CONNECT/SEND/RECV/CLOSE/LISTEN/ACCEPT`, typed handles), `scripts/run-net-tcp.py` with a slirp responder and a QEMU host forward behind `make test-tcp`, `EXPECT_TCP` boot-check markers and 943 host checks; BootInfo stays v1 and the kernel ABI is unchanged |
| `v0.16.0` | 2026-10-27 | phase 4B: net.device registry and net_config, legacy virtio-net PCI driver (VIRTIO_NET_F_MAC only, 32 pre-posted 2 KiB RX buffers on queue 0, one 2 KiB TX bounce buffer on queue 1 with a 500 ms completion wait, up to four polled eth0..eth3), Ethernet framing with 60-byte padding and a MAC/broadcast filter, ARP cache with request/reply and resolution retries, IPv4 header checksum validation, ICMP echo, UDP with a transmit pseudo-header checksum, DHCP client (DISCOVER/OFFER/REQUEST/ACK, three attempts, options 53/55/50/54, mask/router/DNS), net_selftest() boot gate, QEMU slirp wiring (NET_MODE=user) behind make test-net and 938 host checks; BootInfo stays v1 and the kernel ABI is unchanged |
