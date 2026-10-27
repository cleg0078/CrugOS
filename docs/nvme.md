# NVMe

Phase 3B-4 adds a polled NVMe driver so CrugOS can boot from an NVMe
namespace instead of a legacy virtio-blk disk. The driver is deliberately
small: one I/O queue pair per controller, one namespace, no interrupts, no
MSI-X and no hotplug. It is enough to mount the GPT/ext4 root and to run the
same read, JBD2 and writable-ext4 self-tests as the virtio path.

```
PCI class 01/08/02 (mass storage, NVM, NVMe I/O controller)
  -> drivers/pci/pci.c             pci_find_class_nth(), BAR helpers
  -> drivers/block/nvme.c          admin + I/O queues, identify, read/write/flush
  -> kernel/block/block.c          registry, partitions, bounds checks
  -> kernel/fs/gpt.c               GPT header + entries
  -> kernel/fs/storage.c           root policy (crugroot / RAID / nvme%un1)
```

## Controller discovery

`pci_find_class_nth()` scans the legacy 0xCF8/0xCFC configuration space for
the nth function whose class dword matches base class `0x01` (mass storage),
subclass `0x08` (NVM) and programming interface `0x02` (NVMe). `nvme_init()`
probes up to `NVME_MAX_CONTROLLERS` (4) controllers. Each controller exposes
one namespace and is registered as `nvme0n1`, `nvme1n1`, and so on.

The BAR is sized by writing all ones to BAR0 (and the high half when it is a
64-bit memory BAR), restoring the original value, and computing the mask. Every
4 KiB page of the BAR is mapped with `vm_map_page(pa, pa, VM_PRESENT |
VM_WRITE | VM_NX | VM_PCD | VM_PWT | VM_GLOBAL)`. The identity map only covered
0..4 GiB originally; phase 3B-4 generalized `vm.c` so any canonical virtual
address can be mapped. QEMU places this BAR at `0xc000000000`, well above the
old identity limit, so the generic mapping is required.

## Registers

| Offset | Register | Use |
| --- | --- | --- |
| `0x0000` | CAP | MQES, DSTRD, MPSMIN |
| `0x0008` | VS | version (read only) |
| `0x000C` | INTMS | interrupt mask set (all ones) |
| `0x0010` | INTMC | interrupt mask clear |
| `0x0014` | CC | enable, CSS, MPS, IOSQES, IOCQES |
| `0x001C` | CSTS | RDY, CFS |
| `0x0024` | AQA | admin queue sizes |
| `0x0028` | ASQ | admin submission queue physical address |
| `0x0030` | ACQ | admin completion queue physical address |
| `0x1000` | doorbells | `2 * qid + (cq ? 1 : 0)` times DSTRD |

Two offsets are easy to get wrong and are called out here because both were
real bugs in this tree: **CSTS is `0x1C`** (not `0x18`) and **AQA is `0x24`**
(not `0x20`). Reading `0x18` never observes RDY, so enable appears to time out;
writing AQA at `0x20` silently lands in reserved space, so the controller never
accepts the queue geometry.

## Queue setup

1. Mask interrupts with `INTMS = 0xFFFFFFFF`.
2. If `CC.EN` is set (firmware may have left the controller running), clear it
   and wait for `CSTS.RDY` to become 0. OVMF drives the NVMe controller while
   scanning boot options, so this disable step is mandatory.
3. Allocate one page for the admin submission queue and one for the admin
   completion queue from the PMM.
4. Write `AQA`, `ASQ` and `ACQ`.
5. Write `CC` with `EN | CSS=NVM | MPS=4K | IOSQES=6 | IOCQES=4` (`0x00460001`)
   and wait for `CSTS.RDY`.

Queue depth is `min(CAP.MQES + 1, 64)` and must be at least 2. Both admin and
I/O queues use the same depth. Completion polling checks the phase bit, copies
the 16-byte completion, advances the head, toggles the phase on wrap, and rings
the completion doorbell. Every wait has a 1000 ms `timer_monotonic_ns()`
deadline.

### Create I/O queue encoding

The NVMe command layout is a 64-byte submission entry: DW0 opcode/CID, DW1
NSID, DW6-7 PRP1, DW8-9 PRP2, DW10..DW15 command dwords. For the two create
commands the dwords are:

| Command | CDW10 | CDW11 |
| --- | --- | --- |
| Create I/O CQ (`0x05`) | `(qsize-1) << 16 | qid` | `PC` |
| Create I/O SQ (`0x01`) | `(qsize-1) << 16 | qid` | `(cqid << 16) | PC` |

The low 16 bits of CDW10 are the **queue identifier** and the high 16 bits are
the **zero-based queue size**. Reversing them creates queue 63 when the driver
intends queue 1, and Create SQ then fails with status `0x8201` (SCT 1, SC 0:
completion queue invalid). Both queues are created after the namespace is
identified.

## Namespace and I/O

`Identify Controller` (`0x06`, CNS 1) and `Identify Namespace` (`0x06`, NSID 1,
CNS 0) are issued on the admin queue into a bounce page. The namespace size
comes from bytes 0..7 of the namespace identify data; the active LBA format is
selected by `FLBAS`, and the driver requires a 512-byte LBA (`LBADS == 9`).

Reads and writes use a single I/O command per call:

- `NVME_IO_READ` (`0x02`) and `NVME_IO_WRITE` (`0x01`) with NSID 1,
  `CDW10/11` = starting LBA, `CDW12` = zero-based block count, and PRP1/PRP2
  pointing at a bounce buffer;
- up to two pages use PRP1/PRP2 directly, more pages use a PRP list page;
- `NVME_IO_FLUSH` (`0x00`) maps to `block_device.flush`;
- the bounce buffer and PRP list are freed after the command completes.

All I/O is synchronous and polled; no interrupts, MSI-X or queues beyond queue
pair 1 are used. An I/O error marks the controller not ready so later requests
fail instead of silently reusing a broken queue.

## Root selection

`storage_init()` initializes virtio-blk first, then NVMe, and prints
`CRUGOS: virtio ready (<n> device(s))` and `CRUGOS: nvme ready (<n> device(s))`.
It then parses every 512-byte block device, prefers a partition named
`crugroot`, otherwise tries RAID1, and finally mounts ext4 on the selected
device. A boot with only an NVMe disk therefore shows `virtio ready (0
device(s))`, `nvme ready (1 device(s))` and `block ready (1 device(s))`.

QEMU attaches the test image as:

```
-drive file=build/x86_64/nvme.img,if=none,id=nvme0,format=raw
-device nvme,drive=nvme0,serial=CRUGNVME0,logical_block_size=512
```

`scripts/run-qemu.sh` takes `NVME_DISK` and `NVME_SERIAL` for this. The ESP
stays on the vvfat disk; OVMF may print `failed to load Boot0002 "UEFI QEMU
NVMe Ctrl CRUGNVME0" ... Not Found` before falling back to the ESP. That is
expected: the boot loader is not on the NVMe disk.

## Test gate

`make test-nvme` regenerates `build/x86_64/nvme.img` with `scripts/mkdisk.sh`
(the same GPT/ext4 image as `make disk`), boots with only that image attached
as NVMe, requires `CRUGOS: nvme ready` and a namespace (`nvme0n1`), then runs
`scripts/check-disk.py` on the image. The guest must also pass the usual JBD2
and writable-ext4 self-tests, so the NVMe path is exercised for reads, writes,
metadata updates and flush.

## Failure modes

- `nvme   : controller not ready`: CSTS never reached RDY. Check the CSTS
  offset (`0x1C`) and that AQA/ASQ/ACQ were written before `CC.EN`.
- Create SQ fails with status `0x8201`: the Create CQ/Create SQ CDW10 or CDW11
  encoding is reversed. Queue id is low 16 bits; size is high 16 bits.
- `not ready` after `CC.EN` because firmware left the controller enabled: the
  driver must disable and wait for RDY=0 before reprogramming.
- A namespace is not found: `Identify Namespace` uses NSID 1. Multi-namespace
  controllers are not supported in this phase.
- `unsupported lba size`: only 512-byte logical blocks are accepted.
- A BAR above 4 GiB faults: the generic high-canonical mapping in `vm.c` must
  be present; the old identity map did not cover `0xc000000000`.

## Known limits

- Polled I/O, one admin queue and one I/O queue pair, queue depth at most 64.
- One namespace per controller, 512-byte LBAs, up to four controllers.
- No MSI/MSI-X, no interrupts, no hotplug, no power management, no SMART,
  no firmware update, no scatter-gather lists (PRP lists only).
- No AHCI, USB storage, `crypt.device`, RAID10, online rebuild or redundant
  ESP yet; those remain phase 3B / phase 4 work.
