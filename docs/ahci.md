# AHCI / SATA storage

Phase 3B-7 adds a third block transport: a polled AHCI driver that boots the
root filesystem from a SATA disk. It sits beside legacy virtio-blk and NVMe, so
`storage_init()` can mount `crugroot` (or a RAID array) regardless of which
controller the firmware handed the disk to.

```
kernel/fs/storage.c
  -> drivers/block/virtio_blk.c   (legacy virtio-blk over a PCI I/O BAR)
  -> drivers/block/nvme.c         (NVMe over a 64-bit memory BAR)
  -> drivers/block/ahci.c         (AHCI over a 32-bit memory BAR)   <- new
  -> kernel/block/block.c         (registry, partitions, flush)
  -> kernel/fs/gpt.c, kernel/fs/ext4.c
```

## Discovery

- `pci_find_class_nth()` scans PCI class `01/06/01` (mass storage, SATA,
  AHCI 1.0) for up to `AHCI_MAX_CONTROLLERS` controllers.
- QEMU's q35 machine exposes the ICH9 AHCI controller at `00:1f.2`; the
  firmware's own ESP disk also lives on it, so a test boot normally enumerates
  two devices (`sda` = ESP, `sdb` = the test image).
- Up to `AHCI_MAX_DEVICES` (8) drives are registered as `sda`..`sdh`.

## BAR sizing and mapping

The driver sizes BAR5 with the standard probe (write all ones, read back,
restore), then maps every 4 KiB page of the window uncached:

```
for (off = 0; off < size; off += CRUG_PAGE_SIZE) {
    u64 page = pa + off;
    u64 ucf = VM_PRESENT | VM_WRITE | VM_NX | VM_PCD | VM_PWT | VM_GLOBAL;

    /* The identity map may already cover the BAR; just make it
     * uncached in that case. */
    if (vm_translate(page) == page) {
        if (vm_protect_page(page, ucf) != 0)
            return -1;
    } else if (vm_map_page(page, page, ucf) != 0) {
        return -1;
    }
}
```

Two details matter:

- **32-bit BAR sizing.** For a 32-bit memory BAR the upper half of the mask
  must be forced to ones before `~mask + 1`, otherwise the size overflows to
  `0xffffffff00001000` and the power-of-two check rejects the BAR. The same
  fix is applied in `nvme.c`, where 64-bit BARs supplied the upper bits from
  `hi_mask` and hid the bug.
- **Identity-mapped BARs.** `vm_map_page()` refuses to overwrite a present
  PTE, and `pte_for()` splits the 2 MiB identity huge page into 4 KiB
  entries. The ICH9 AHCI BAR (`0x810c4000`) therefore already has a present
  PTE. The driver detects that with `vm_translate()` and switches to
  `vm_protect_page()`, which keeps the identity translation but adds
  `PCD|PWT` so the registers are uncached.

## Port probing

`ahci_controller_probe()` caps the port count by both `CAP.NP` and the mapped
BAR window (ICH9 advertises 6 ports in a 4 KiB window):

```
ports = (cap & 0x1f) + 1u;
if (c->bar_size > AHCI_PORT_BASE) {
    u32 max_ports = (c->bar_size - AHCI_PORT_BASE) / AHCI_PORT_SIZE;
    if (ports > max_ports)
        ports = max_ports;
}
```

For each `PI` bit the driver polls `SSTS.DET` for `3` (device present) and then
checks `SIG == 0x101` (ATA). Empty ports never report `DET == 3`, so the poll
uses a short `AHCI_LINK_TIMEOUT_MS` (20 ms) window instead of the command
timeout: with the full 1000 ms budget, four empty q35 ports added 4 s to the
boot.

## Port setup and commands

`ahci_port_setup()` stops the port (`CMD.ST`/`CMD.FRE` cleared, waits for
`CMD.CR`/`CMD.FR`), allocates one page each for the command list, the FIS
receive area and one command table, programs `CLB`/`FB`, sets
`SUD|POD|FRE|ST` and waits for `TFD.BSY`/`TFD.DRQ` to clear.

Every transfer uses a single command slot and a single PRDT entry:

| Field | Value |
| --- | --- |
| FIS | `0x27` H2D, `0x80` (command), `0x40` (LBA mode) |
| Commands | `0xEC` IDENTIFY, `0x25` READ DMA EXT, `0x35` WRITE DMA EXT, `0xEA` FLUSH CACHE EXT |
| LBA | 48-bit, spread over FIS bytes 4..10 |
| Count | 16-bit, FIS bytes 12..13 |
| PRDT | `dba` = bounce buffer, `dbc` = bytes - 1 |
| Timeout | `AHCI_TIMEOUT_MS` = 1000 ms |

`ahci_exec()` clears `PORT_IS`, writes `PORT_CI` bit 0, then polls `PORT_CI`
until the slot retires, and fails on `TFD.ERR` or a task-file error
interrupt. All I/O is polled; interrupts stay masked and no command uses
multiple slots or native queuing.

Transfers larger than `AHCI_BOUNCE_BYTES` (8 KiB = 16 sectors) are split by
`ahci_bd_read()`/`ahci_bd_write()`, which copy through the bounce buffer, so
the PRDT never needs more than one entry.

## Identification

The IDENTIFY DEVICE payload provides:

- LBA48 capacity when word 83 bit 10 (`0x0400`) is set: words 100..103 give
  the 64-bit sector count; otherwise words 60..61 give the LBA28 count.
- logical sector size when word 106 bit 12 (`0x1000`) is set: words 117..118
  give the size in words; only 512-byte logical sectors are accepted.

The resulting `struct block_device` gets `read`, `write` and `flush` hooks
(`FLUSH CACHE EXT`) so the ext4 and JBD2 flush semantics are the same as on
virtio and NVMe.

## Root selection

`storage_init()` probes virtio-blk, NVMe and AHCI in that order, prints one
ready line per transport, then parses the GPT of every collected 512-byte
device. The root policy is unchanged: a partition named `crugroot` is mounted
directly, otherwise Linux-RAID (`fd00`) partitions are assembled into `md0`.
The chosen partition is reported as `CRUGOS: root device <name>`.

Boot markers added by this phase:

```
ahci   : sda <sectors> sectors, port <n>
CRUGOS: ahci ready (<n> device(s))
CRUGOS: root device <partition>
CRUGOS: phase 3b8 boot ok
```

## Test gate

```
make test-ahci      # boot from a SATA image and e2fsck the root
make ahci-disk      # regenerate build/<arch>/ahci.img
```

`make test-ahci` boots with no virtio and no NVMe disk: the ESP is still served
by the same controller as `sda`, and the boot contract additionally requires
`CRUGOS: ahci ready (>=1 device(s))`, an `sd?` root device line and the
`phase 3b8` marker. `scripts/check-disk.py` then extracts the AHCI image,
runs `e2fsck -fn` and verifies `/hello.txt` plus the persisted
`/selftest/b.txt`.

QEMU attaches the disk with:

```
-drive file=$AHCI_IMG,if=none,id=ahci0,format=raw \
-device ide-hd,drive=ahci0,bus=ide.1
```

Port 0 stays the firmware's ESP disk, so the test image lands on port 1.

## Failure modes

| Serial line | Meaning |
| --- | --- |
| `ahci   : BAR setup failed at bb:dd.f` | BAR sizing or page mapping failed |
| `ahci   : port <n> setup failed` | port stop/ready timed out |
| `ahci   : identify failed on port <n>` | IDENTIFY command failed |
| `ahci   : unsupported logical sector size <n> on port <n>` | 4K-native drive |
| `storage: no block device driver` | no virtio, NVMe or AHCI drive found |
| unexpected `CRUGOS: ahci ready (0 device(s))` | disk missing from the QEMU command line |

## Known limits

- One command slot, one PRDT entry and no native command queuing; everything
  is polled with a 1000 ms deadline.
- 512-byte logical sectors only; 4K-native drives are rejected.
- No hotplug, no port multiplier, no PMP/FIS-based switching, no NCQ, no TRIM
  and no error recovery beyond failing the request.
- At most four controllers and eight drives; names are fixed at boot.
- Interrupts are not used: the controller's error interrupt is polled through
  `PORT_IS` after every command.
