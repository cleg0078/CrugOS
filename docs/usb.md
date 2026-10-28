# USB: xHCI host controller and the HID boot keyboard

CrugOS can now take keyboard input from a USB keyboard. Phase 5A adds a polled
xHCI host controller driver (`drivers/usb/xhci.c`), a USB HID boot-protocol
decoder (`drivers/usb/usb_hid.c`) and the glue that feeds decoded characters
into `input.device`, so a hotplugged keyboard types into the same console byte
queue the PS/2 keyboard uses.

```
PCI function (class 0C/03/30)                     QEMU: -device qemu-xhci
        |
        v
xhci.c: 64-bit MMIO BAR -> capability / operational / runtime registers
        |
        +-- command ring (4 KiB, 256 TRBs) -> ENABLE_SLOT, ADDRESS_DEVICE,
        |                                     EVALUATE_CONTEXT, CONFIGURE_ENDPOINT
        +-- event ring   (4 KiB, 256 TRBs) -> command completion, port status
        |                                     change, transfer events
        +-- EP0 ring (control transfers: GET_DESCRIPTOR, SET_CONFIGURATION)
        +-- HID ring (8-byte interrupt IN reports)
                                |
                                v
        usb_hid_boot_decode() -> input_char() -> input.device queue (64 B)
                                |
                                v
                        console_try_getc() -> lineedit -> crypt_unlock()
```

Everything is polled from the console read path: there is no MSI/MSI-X, no
interrupt handler and no DMA engine beyond the controller walking the rings it
is given. The driver runs on the boot thread between timer ticks, so the timer
and scheduler interrupt path is untouched.

## Controller bring-up (`usb_init`)

1. `pci_find_class_nth(0x0C, 0x03, 0x30, 0, ...)` looks for the xHCI
   programming interface. When no controller exists the driver prints
   `CRUGOS: usb controller absent` and disables itself; every other boot path is
   unaffected.
2. Command register: memory space and bus mastering (`0x2 | 0x4`).
3. BAR0 sizing. xHCI always uses a 64-bit memory BAR. Firmware is expected to
   have assigned it, but OVMF has no xHCI driver, so under QEMU BAR0 reads back
   as `0x00000004` - "64-bit memory BAR, base zero" - which looks like a 32-bit
   BAR until the low dword is decoded. The driver sizes the BAR itself, then
   calls `pci_pick_mmio()` to find a free window walking down from
   `0xffb00000` in 1 MiB steps, skipping the OVMF flash window and any range
   another function already decodes, and programs both BAR halves:

   ```
   CRUGOS: usb controller BAR assigned at 0xffb00000
   CRUGOS: usb controller ready (xhci, 8 port(s))
   ```

4. The BAR is mapped page by page (`VM_PRESENT | VM_WRITE | VM_NX | VM_PCD |
   VM_PWT | VM_GLOBAL`) into the identity map that phase 1 built. Pages already
   reachable through the 2 MiB huge pages are re-protected with
   `vm_protect_page()`, the rest get `vm_map_page()`.
5. Reset: stop the controller, write `HCRST`, wait for `HCRST` and `CNR` to
   clear. This matters under UEFI: OVMF may have used the controller before
   handing off, so the driver never assumes a pristine state.
6. Read `HCSPARAMS1` (ports, slots), `HCCPARAMS1` (32 vs 64 byte contexts, port
   power control), `DBOFF` and `RTSOFF`.
7. Allocate the command/EP0/HID rings, the event ring, the event ring segment
   table, the device context array and one input context page from the PMM. All
   of them are 4 KiB aligned and identity mapped.
8. Program `CONFIG`, `DCBAAP`, `CRCR` (with the cycle bit), `ERSTSZ`, `ERSTBA`,
   `ERDP` (with `EHB`), then set `RUN` and wait for `HALT` to clear. The
   `CRCR` value is read back, so a controller that silently ignores the ring
   setup is reported instead of timing out later.

## Rings and TRBs

A transfer request block is 16 bytes:

```c
typedef struct xhci_trb {
    u64 param;    /* data pointer, inline data with IDT, or a ring address */
    u32 status;   /* transfer length / completion code / event flags */
    u32 control;  /* cycle bit, flags, TRB type */
} xhci_trb;
```

Each ring is one 4 KiB page holding 256 TRBs. Index 255 is a Link TRB pointing
back at the ring base with the toggle-cycle (`TC`) bit set. When a new transfer
does not fit in the remaining slots, the driver pads the rest with No-Op TRBs,
rewrites the Link TRB with the current producer cycle, flips the producer cycle
and restarts at index 0. Software always writes the cycle bit the controller
expects, so a stale entry left from the previous lap can never run.

The event ring works the other way round: `event_pop()` returns a TRB whose
cycle bit matches the consumer cycle, advances the dequeue index and writes
`ERDP = base + dequeue * 16 | EHB` to acknowledge it. `EHB` must be written for
every event, otherwise the controller stops posting new ones.

## Enumeration

For the first connected port the driver runs:

1. `ENABLE_SLOT` (command TRB type 9). The completion event returns the slot ID;
   `dcbaa[slot]` is set to the device context page.
2. `ADDRESS_DEVICE` (type 11) with the input context: slot context
   (`speed << 20 | last_ctx << 27`, `port << 16`) and an EP0 context using the
   8-byte default packet size.
3. `GET_DESCRIPTOR(device, 8)` over EP0, then `EVALUATE_CONTEXT` (type 13) to
   raise the EP0 packet size to `bMaxPacketSize0` (clamped to 8..64).
4. `GET_DESCRIPTOR(device, 18)`, then `GET_DESCRIPTOR(config, 9)` for
   `wTotalLength` (clamped to 512 bytes) and `GET_DESCRIPTOR(config, total)`:

   ```
   CRUGOS: usb device 0627:0001 on port 5 (speed 3)
   ```

5. `usb_hid_parse_config()` walks the descriptor chain and accepts only an
   interface with class 3 (HID), subclass 1 (boot) and protocol 1 (keyboard),
   followed by an interrupt IN endpoint. Any other interface clears the match,
   so a HID mouse or a composite device is rejected rather than misread.
6. `SET_CONFIGURATION(1)`, then `CONFIGURE_ENDPOINT` (type 12) for the report
   endpoint: context entries = the new DCI, endpoint type 7 (interrupt IN),
   `max_packet` and `interval` from the descriptor, dequeue pointer with the DCS
   bit set.
7. Arm one Normal TRB (type 1) pointing at the 8-byte report buffer with `IOC`
   and ring the doorbell for `DCI = ep_num * 2 + 1` (EP1 IN is DCI 3).

Control transfers build a three-TRB TD: Setup Stage with `IDT`, `CHAIN` and the
eight setup bytes packed into the parameter field; an optional Data Stage with
`CHAIN` and `DIR_IN` for IN transfers; and a Status Stage with `IOC` that
carries `DIR_IN` for OUT transfers only. Missing the `IDT` detail is a classic
xHCI mistake: with `IDT` set the controller reads the setup packet from the TRB
itself, so putting a pointer there makes the device STALL with completion code 6.

## Boot report decoding (`usb_hid_boot_decode`)

A boot keyboard sends eight bytes: one modifier bitmap and six usage slots. The
decoder mirrors the PS/2 keymap in `kernel/input/input.c` so both sources
produce the same bytes:

- modifier bits: `0x01` left Ctrl, `0x02` left Shift, `0x10` right Ctrl,
  `0x20` right Shift; Alt and GUI bits are ignored;
- usages `0x04`..`0x1D` are the letters `a`..`z`, folded with Shift XOR Caps
  Lock;
- usages `0x1E`..`0x27` are the digits `1`..`0`, Shift gives the symbols on
  the US row;
- usages `0x2D`..`0x38` are the punctuation row (`-`, `=`, `[`, `]`, the
  backslash, `;`, the apostrophe, the backtick, `,`, `.`, `/`); `0x32` is
  unused and Shift selects the shifted glyph;
- `0x28` Enter, `0x29` Escape (`0x1B`), `0x2A` Backspace (`0x7F`), `0x2B` Tab
  (`0x09`), `0x2C` Space;
- `0x39` is Caps Lock: pressing it toggles a latch and produces no character;
- Ctrl folds letters to `0x01`..`0x1A`, `[`..`_` to `0x1B`..`0x1F` and drops
  `Ctrl+Space` / `Ctrl+``, exactly like the PS/2 decoder;
- the previous six usages are remembered, so a held key produces one character
  and only newly pressed keys report; releasing and pressing again works.

The first new key in a report becomes the returned character, so a report with
two new keys delivers the lower slot only.

## Ports and hotplug

`PORTSC` is read for every port after each `PORT_STATUS_CHANGE` event:

- `CCS` (bit 0) - a device is connected;
- `PED` (bit 1) - the port is enabled. It is write-1-to-disable, so the driver
  never writes a 1 there;
- `PR` (bit 4) - reset in progress;
- `PLS` (bits 5..8) - link state;
- `PP` (bit 9) - port power, preserved as read;
- `speed` (bits 10..13);
- change bits `CSC`, `PEC`, `WRC`, `OCC`, `RC`, `PLC`, `CEC` (bits 17..23),
  which are write-1-to-clear.

Clearing changes writes back the read value with `PED`, `PR` and every change
bit cleared, plus the change bits set: `(v & ~(PED | PR | RW1C)) | RW1C`. A reset
powers the port, writes `PR`, polls up to one second for `PR` to clear and
`RC` to appear, and requires `CCS && PED` before reading the speed.

When the port that carries the keyboard loses `CCS`, the driver drops the
binding and prints `CRUGOS: usb keyboard gone`; a later connect is enumerated
from scratch. This is what the hotplug gate exercises: the guest boots with an
empty controller, the host plugs a keyboard into a port, and the port status
change event drives the whole enumeration path.

## Command line

| Token | Meaning |
| --- | --- |
| `usb.wait=SECONDS` | wait up to that long for a keyboard before boot continues (default 0, cap 120) |
| `usb.keys=N` | wait for N decoded keys in the self-test, then exit (default 0, cap 16) |

Both default to "do not wait", so a normal boot never blocks on a missing USB
keyboard. The gate passes `usb.wait=20 usb.keys=3`.

## Boot integration

`usb_init()` runs right after `input_init()` / `keyboard_init()`, before the
storage stack, and registers `usb_poll()` as the input poll hook
(`input_set_poll()`). `input_try_getc()` runs that hook first, so every console
read drains the xHCI event ring before it looks at the byte queue. In
`kernel/core/selftest.c` `usb_selftest()` sits after the networking self-test:

```
CRUGOS: usb controller ready (xhci, 8 port(s))
CRUGOS: usb device 0627:0001 on port 5 (speed 3)
CRUGOS: usb keyboard ready (endpoint 0x81, 8 byte reports)
CRUGOS: usb selftest ok (3 key(s))
```

`usb_selftest()` returns success immediately when no controller was found or
`usb.wait=0`, so the marker set of every earlier gate is unchanged. With
`usb.wait=N` it polls for N seconds and fails the boot with
`CRUGOS: usb keyboard timeout` when no keyboard shows up; with `usb.keys=N` it
also waits for N key reports.

## Host tests

`tests/host/test_usb.c` links `drivers/usb/usb_hid.c` into the host binary and
covers the descriptor walk (boot keyboard, full-speed packet size, masked
`wMaxPacketSize`, non-boot protocol, missing endpoint, OUT endpoint, bulk
endpoint, a later interface clearing the match, zero `bLength`, overrunning
`bLength`, NULL arguments) and the decode matrix (letters, Shift, Caps latch,
Ctrl chords including `Ctrl+[` and `Ctrl+Space`, digits and shifted symbols,
punctuation and its shifted row, Enter/Escape/Backspace/Tab/Space, the unused
`0x32` usage, held-key repeat suppression, release and re-press, two new keys in
one report, modifier usages in the key array, and `usb_hid_reset()`).
`make test-host` reports 1117 checks including these.

## Gate

`make test-usb-hotplug` proves the hotplug path end to end.
`scripts/run-usb-hotplug.py` boots the usual disk with:

```
QEMU_EXTRA="-qmp unix:<tmp>/qmp.sock,server=on,wait=off -device qemu-xhci,id=xhci"
MACHINE_EXTRA="usb=off,i8042=off"
CMDLINE_EXTRA="usb.wait=20 usb.keys=3"
```

`usb=off` keeps the machine's own USB controllers out of the picture and
`i8042=off` removes the PS/2 controller, so the only keyboard the guest can see
is the one hotplugged over QMP. The driver then:

1. waits for `CRUGOS: usb controller ready`;
2. issues `device_add {"driver": "usb-kbd", "id": "kbd0", "bus": "xhci.0"}`;
3. waits for `CRUGOS: usb keyboard ready`;
4. sends one `send-key` per character and waits for `CRUGOS: usb key` with that
   character; if console routing delivers nothing (a `-display none` boot has no
   console keyboard handler) it falls back to `input-send-event` addressed at
   `/machine/peripheral/kbd0`;
5. requires QEMU exit status 33 and `CRUGOS: usb selftest ok`.

`scripts/boot-check.py` then verifies the `usb` marker set under
`EXPECT_USB=yes`: the controller and keyboard came up, the endpoint was
reported, at least one key was decoded, and `CRUGOS: input ready (serial only)`
proves the PS/2 path was absent.

## Limits

- Polled only: no interrupts and no MSI/MSI-X, so reports are drained when the
  console is read; the self-test loops on `cpu_wait_for_irq()`, which keeps the
  vCPU idle between polls. Key latency is therefore bounded by the 10 ms timer
  tick while a prompt waits for input.
- Boot protocol only: the driver never sends `SET_PROTOCOL` and never parses the
  HID report descriptor, so a keyboard without the boot report layout is
  rejected. Media keys and other HID collections are ignored.
- One keyboard on the first xHCI controller; no hubs, no multiple devices. The
  trb/context code is shared with the rest of the controller, but only the boot
  keyboard path is exercised.
- When firmware left the BAR unassigned the driver picks a window below
  `0xffc00000`; it does not relocate other devices.
- No port power switching beyond preserving `PP`, no wake-on-connect, no
  bandwidth accounting.
