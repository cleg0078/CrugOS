# virtio-gpu 2D scanout

Phase 8A is the first half of the specification's phase 8, "GPU acceleration".
It gives the kernel a real display device instead of the firmware's linear
framebuffer: a virtio-gpu-pci function is discovered over the modern virtio-pci
transport, one 2D resource is created with contiguous backing pages, attached as
the scanout, and left as the surface the Intuition compositor and the Workbench
paint into. Every present transfers the damaged rectangle to the host and
flushes it, so what a screendump of the device shows is what the guest actually
sent, not what a VGA adapter was still holding in memory.

Only the 2D path exists. There is no virglrenderer context, no Vulkan, no
command stream and no 3D resource; the specification's "accelerated triangle"
acceptance item is still deferred. Phase 8A is about the display
plumbing a 3D renderer would later draw through: a device, a resource, a
scanout, and a present path that tells the host what changed.

Phase 8B adds the two pieces the 2D path was missing: the display's EDID is
fetched and parsed so the scanout follows the monitor's preferred timing, and
the pointer gets a cursor plane instead of being painted into the same
pixels. The first cut of the cursor support was wrong on the wire and drew
nothing; the layout is fixed and gate 64 proves the plane's pixels over VNC.
Since `v0.47.1` the default is `gpu.cursor=sw`, which paints the guest's own
12x19 sprite into the scanout, because a window manager such as QEMU's GTK UI
does not composite the device plane; `gpu.cursor=hw` keeps the 8B plane and
gate 65 proves the sprite's pixels. There is still no virgl context and no 3D
resource; the specification's accelerated triangle stays deferred.

The software path is not replaced, it is fronted. With no virtio-gpu device,
with `gpu.enable=0`, or after any driver failure, the kernel keeps booting on
the GOP framebuffer exactly as it did in phase 7C. All 59 earlier gates still
boot that way and are unchanged. `make test-gpu` is gate 60,
`make test-gpu-usb` gate 61, `make test-gpu-edid` gate 62,
`make test-gpu-cursor` gate 63, `make test-gpu-cursor-shape` gate 64 and
`make test-gpu-cursor-sw` gate 65; those six are the only gates that boot a
GPU. The last four are the phase 8B gates.

## Why a GPU path

The framebuffer CrugOS has used since phase 1 is a memory range the firmware
maps and then forgets. It works, and it is why the desktop gates can assert
pixel colours at all, but it is a dead end for three reasons.

- The firmware owns the mode. The kernel never learns what other modes the
  display supports, cannot change resolution after boot, and cannot tell the
  host that anything changed. A screendump reads guest memory, which works
  under QEMU and stops being true on real hardware.
- Every pixel travels through the CPU's cached view of guest RAM, so a
  compositor repaint is memory bandwidth with no boundary at which the display
  is guaranteed coherent. That is fine for a 1024x768 demo and is not a
  foundation for acceleration.
- A virtio device is where a future 3D path will live. The specification puts
  virglrenderer behind exactly this device, so the transport, the control
  queue, the resource and the present model have to exist before a 3D context
  can be added to them.

What 8A does not try to fix is the compositor. The Workbench still renders in
software into guest memory; only the last hop to the host changed. That is
deliberate: the phase is about the boundary, and a GPU renderer that drew
directly into the resource would hide whether the boundary works.

## The virtio-gpu device and transport

The driver binds PCI device id 0x1050, which is virtio-gpu over the modern
transport. Device id 0x1010, the legacy transitional id, is detected and then
ignored with a line that says so: the modern capability layout (common, notify,
ISR and device structures behind PCI capabilities) is what the driver
implements, and maintaining a second legacy register interface for a device
that QEMU can expose either way buys nothing.

The device is brought up in the same order as the other virtio drivers:

1. The modern capabilities are located: `GPU_PCI_CAP_COMMON` 1,
   `GPU_PCI_CAP_NOTIFY` 2, `GPU_PCI_CAP_ISR` 3 and
   `GPU_PCI_CAP_DEVICE` 4. A missing capability, an unmappable BAR or a
   rejected feature set ends the bring-up with a reason line.
2. One control queue (index 0) is configured. Its descriptor table, driver area
   and device area come from `pmm_alloc_pages(3)`, which is also the largest
   queue the driver accepts (`GPU_QUEUE_MAX` 64 entries).
3. Commands are synchronous. Each one is built in a 256-byte buffer
   (`GPU_CMD_MAX`), the response arrives in a 512-byte buffer
   (`GPU_RESP_MAX`), and the driver waits for completion or a two-second
   deadline (`GPU_TIMEOUT_NS`). Only one command is in flight at any time.
4. Every command starts with a 24-byte control header built by
   `gpu_cmd_init()`: type at offset 0, flags at 4, fence at 8 and context id
   at 16. The driver never issues a command the device could answer
   asynchronously, so the fence field stays zero.

The command set is the 2D subset:

| Command | Value | Used for |
| --- | --- | --- |
| `GET_DISPLAY_INFO` | 0x0100 | ask the host for its preferred mode |
| `RESOURCE_CREATE_2D` | 0x0101 | create resource 1 as `B8G8R8X8_UNORM` (format 2) |
| `SET_SCANOUT` | 0x0103 | make the resource the visible scanout |
| `RESOURCE_FLUSH` | 0x0104 | tell the host a rectangle changed |
| `TRANSFER_TO_HOST_2D` | 0x0105 | copy a guest rectangle into the resource |
| `RESOURCE_ATTACH_BACKING` | 0x0106 | give the resource its contiguous backing pages |

Responses are classified rather than compared by string. `gpu_resp_class()`
maps the response type to 1 for `OK_NODATA` (0x1100), 2 for
`OK_DISPLAY_INFO` (0x1101), -1 for the error range
`ERR_UNSPEC` 0x1200 through `ERR_INVALID_PARAMETER` 0x1205, and 0 for
anything else, and `gpu_resp_name()` turns each of those into the short name
the failure line quotes.

The mode comes from `gpu_display_pick()`: the `gpu.size=WxH` token wins if
it was supplied and fits the limits, then the host's display info, then the
first of 1024x768, 800x600 and 640x480 that the device accepts. A size larger
than `GPU_RECT_MAX_W` x `GPU_RECT_MAX_H` (4096x2160) or one whose backing
would exceed `GPU_BACKING_MAX_PAGES` 8192 pages is refused rather than
clamped.

## The 2D resource and the scanout

There is exactly one resource, id 1, created as a
`B8G8R8X8_UNORM` 2D resource at the chosen size. Its backing is contiguous
physical memory sized by `gpu_backing_pages()`:

| Field | Value | Meaning |
| --- | --- | --- |
| format | 2 | `B8G8R8X8_UNORM`: one 32-bit pixel, alpha ignored |
| width, height | picked mode | 1280x800 in the gate, 1024x768 by default |
| backing pages | ceil(w * h * 4 / 4096) | 1000 pages at 1280x800 |
| max pages | 8192 | 4096x2160 is the largest supported mode |
| resource id | 1 | the only resource the driver ever creates |

The order matters and the failure lines name the step: create the resource,
attach the backing list, set it as scanout, then hand its base pointer to the
framebuffer code. If any of those fails the boot continues on the firmware
framebuffer, because a display device that cannot be driven is a worse
outcome only if it stops the machine.

The 2D resource is never resized and never replaced. Phase 8A had no second
resource, no cursor plane and no scanout for an overlay; phase 8B adds exactly
one more resource, the 64x64 cursor plane, on its own queue. A device without
that plane still gets the software sprite drawn into the same pixels, so the
compositor path is unchanged either way.

## The framebuffer and the damage rectangle

The compositor does not know a GPU exists. `kernel/arch/x86_64/fb.c` grew a
second attach path instead:

- `fb_attach_linear(base, width, height, stride, format, present)` installs
  the GPU resource as the framebuffer and stores a present callback.
  `fb_attach()`, the firmware path, leaves that callback null and behaves
  exactly as it did before.
- `fb_store()` accumulates a damage rectangle as pixels are written:
  `gpu_damage_add()` merges every store into the bounding box
  `(x0, y0, x1, y1)` with exclusive right and bottom edges.
- `fb_present()` clips the accumulated rectangle, calls the callback once,
  and resets the accumulator. When no callback is installed it does nothing,
  which is why the software path costs the same as before.
- `fb_present_begin()` and `fb_present_end()` bracket a repaint with a
  depth counter; only the outermost end flushes. `intuition_redraw()` and
  `intuition_redraw_window()` are wrapped that way, so a full desktop repaint
  of several windows and gadgets costs one transfer and one flush rather than
  one per primitive.

The console is the exception. `fb_putc()` flushes when it writes a newline or
wraps a line rather than after every glyph: one device round trip per character
would make boot output the slowest part of the boot, while one per line keeps
the screen current and still batches 80 characters into a single transfer. The
rect, text and scroll primitives each flush at the end of their own operation,
so a clearing or scrolling step is visible even when no newline follows.

A present is therefore two commands: `TRANSFER_TO_HOST_2D` for the damage
rectangle, then `RESOURCE_FLUSH` for the same rectangle. `gpu_transfers()`
and `gpu_flushes()` count them, and the gate compares those counts with the
numbers in the two success lines rather than trusting a screenshot alone.

## Boot tokens and markers

| Token | Default | Meaning |
| --- | --- | --- |
| `gpu.enable=0|1` | 1 | `0` disables the driver and keeps the firmware framebuffer |
| `gpu.size=WxH` | none | preferred scanout size, ahead of the EDID; the EDID preferred timing, the host mode and the built-in list are the fallbacks |
| `gpu.test=1` | 0 | makes the self-test announce a skip on a software framebuffer |

The boot prints one line when the device is up and one when the scanout is
reachable:

```
CRUGOS: virtio-gpu ready (1280x800, 1000 backing page(s), resource 1, modern pci)
CRUGOS: gpu scanout ok (1280x800, 1 transfer(s), 1 flush(es))
```

The self-test runs after the Workbench is initialised, because that is the
first point at which there is a desktop to present:

```
CRUGOS: gpu selftest start
CRUGOS: gpu selftest ok (1280x800, 13 transfer(s), 13 flush(es))
```

On a software framebuffer the self-test is silent unless `gpu.test=1` was
supplied, in which case it prints the skip line and still returns success:

```
CRUGOS: gpu selftest skipped (software framebuffer)
```

The ready line carries the mode, the backing page count, the resource id and
the transport, so a log can be read without another command. The scanout line's
counters are one transfer and one flush: the first full present of the desktop.
The self-test line's counters are the ones `make test-gpu` asserts.

With EDID support the ready line is preceded by the EDID and mode-source
lines of phase 8B (below), and the pointer is shown on the hardware cursor
plane rather than painted into the resource; `gpu.size=` still wins over the
EDID, so every earlier gate and the demo keep their size.

## The gate

`make test-gpu` is gate 60 of 65. It is the only gate that boots without any
firmware framebuffer: the QEMU command line is
`-vga none -device virtio-gpu-pci,id=gpu0`, so the GOP has no adapter to bind
and the virtio-gpu resource is the machine's only display. A screendump of the
`gpu0` device therefore contains only pixels the guest transferred and
flushed over virtio.

The gate boots with `wb.enable=1 wb.wait=30 gpu.enable=1 gpu.test=1`, waits
for `CRUGOS: gpu scanout ok`, `CRUGOS: wb desktop ready`,
`CRUGOS: gpu selftest ok` and `CRUGOS: wb selftest ok`, then takes its
screendumps through QMP and checks specific pixels:

```
gpu: device gpu0 screendump 1280x800
gpu: desktop background pixel (1200, 700) -> ((16, 32, 48),)
gpu: shell title bar pixel (600, 306) -> ((255, 176, 0),)
gpu: workbench title bar pixel (200, 96) -> ((128, 128, 128),)
gpu: workbench frame pixel (80, 150) -> ((255, 255, 255),)
gpu: amber 5800 px, frame 3627 px
ok: virtio-gpu scanout 1280x800 (13 transfer(s), 13 flush(es), exit 33)
```

The colour counts matter as much as the four pixels: 5800 amber pixels is the
title bar of one active window and 3627 white pixels is the frame drawn around
it, which would not survive a half-transferred rectangle. The driver then
requires the guest to exit 33 and runs `scripts/check-disk.py`. The boot-check
line carries `gpu yes` and every earlier flag `no`:

```
ok: boot markers found, version 0.47.1 avx2 yes raid no nvme no rebuild no
ahci no crypt no aes any net no tcp no dns no usb no mouse no audio no gfx no
wb no menu no shell no keys no desk no icons no drawer no select no screen no
cx no app no pkg no caps no wbpkg no gpu yes ... QEMU exit status 33
ok: ext4 root is clean, /hello.txt and /selftest/b.txt persisted
```

The three knobs are `GPU_WB_WAIT` (30 seconds the gate lets the desktop
settle), `GPU_RUN_TIMEOUT_SEC` (300, the wall-clock budget) and
`GPU_QEMU_EXTRA` (the device flags above). `scripts/boot-check.py` gained
`--expect-gpu=yes` as a flag rather than a positional argument, so the
argument order of every earlier gate is unchanged; without the flag a boot that
prints a scanout line fails on purpose.

`make demo` boots the Workbench on the same scanout instead of on firmware
memory: `DEMO_GPU=1` (the default) adds `-vga none -device
virtio-gpu-pci,id=gpu0` and `DEMO_GPU_SIZE=1024x768` becomes `gpu.size=`, so
the demo log starts with `CRUGOS: virtio-gpu ready (1024x768, 768 backing
page(s), resource 1, modern pci)` and `CRUGOS: gpu scanout ok (1024x768, 1
transfer(s), 1 flush(es))`. The demo pointer is the guest sprite because
`DEMO_CURSOR=sw` is the default, so `DEMO_CMDLINE` carries `gpu.cursor=sw`;
`make demo DEMO_CURSOR=hw` uses the device cursor plane instead. `DEMO_GPU=0`
drops the device and both tokens and boots the software framebuffer.

This gate does not assert a boot-time budget. In this configuration the serial
boot report reads:

```
  boot   : pmm=25800 us cpu=354 us vm=621 us traps=311 us timer=65234 us
           heap=518 us storage=326483 us sched=6755 us syscall=110 us
           total=426189 us
```

That total is roughly twice a GOP gate's, because the storage phase under this
host's vvfat ESP dominates it and the GPU leg adds no firmware graphics
protocol to wait for. The 250 ms boot contract is a separate measurement on the
GOP path and is not checked here.

### The 64-bit BAR bug

Gate 60 as first written could not see this bug. With no virtio-gpu device
OVMF leaves the xHCI BAR unassigned and the kernel assigns `0xffb00000` below
4 GiB. Add `virtio-gpu-pci` and OVMF moves the controller to
`0xc000004000`, above 4 GiB. The driver decoded the 64-bit BAR like this:

```
bar_hi = orig_hi & 0xfffff000u;
```

That mask keeps only the high dword's page frame and zeroes the rest, so for
`0xc000004000` the address collapses to `0x4000`: the kernel mapped
physical `0x4000`, the low identity page that still holds the real-mode
IVT and BDA, instead of the controller. Reads of USBCMD and USBSTS returned
whatever those bytes happened to be, HCRST never cleared, and the boot printed
`CRUGOS: usb controller absent (reset timeout)`. The visible symptom was
`make demo` with the new `DEMO_GPU=1` default: the desktop appeared on
the virtio-gpu scanout but the keyboard and mouse were dead, because the
input controller had never been mapped.

The fix uses the high dword unmasked for the address and rebuilds the
all-ones size probe from both dwords:

```
mask = (u64)(pci_read32(0x10) & 0xfffffff0u);
if (is64) mask |= (u64)pci_read32(0x14) << 32;
else      mask |= 0xffffffff00000000ull;
size = ~mask + 1;   /* clamped to [4 KiB, 64 KiB] */
```

### Gate 61: scanout plus a hotplugged mouse

`make test-gpu-usb` repeats the scanout boot with an xHCI controller
present, which is the configuration the BAR bug hid in. The device flags come
from `GPU_USB_QEMU_EXTRA`, which defaults to `GPU_QEMU_EXTRA` plus
`-device qemu-xhci,id=xhci`, and the boot uses
`MACHINE_EXTRA='usb=off,i8042=off'` so the firmware's own USB stack is off
and there is no PS/2 controller to fall back on:

```
-vga none -device virtio-gpu-pci,id=gpu0 -device qemu-xhci,id=xhci
```

The guest command line is
`wb.enable=1 wb.wait=30 gpu.enable=1 gpu.test=1 usb.mouse=30 usb.moves=2`:
the desktop plus the existing input window, which polls for up to 30 seconds
and wants two pointer moves. `scripts/run-gpu.py` grew a `--usb` leg
that hot-plugs a `usb-mouse` on the `xhci.0` bus over QMP: it sends
`device_add`, resolves the new device's QOM path with `qom-list`, then
moves the pointer with `input-send-event` relative events. The gate
requires two pointer reports before it accepts the same four pixel samples as
gate 60, so a controller that mapped but never delivered input fails:

```
gpu: usb controller ready, 2 pointer move(s)
gpu: device gpu0 screendump 1280x800
gpu: desktop background pixel (1200, 700) -> ((16, 32, 48),)
gpu: shell title bar pixel (600, 306) -> ((255, 176, 0),)
gpu: workbench title bar pixel (200, 96) -> ((128, 128, 128),)
gpu: workbench frame pixel (80, 150) -> ((255, 255, 255),)
ok: virtio-gpu scanout 1280x800 with the USB mouse (2 move(s), 13 transfer(s),
13 flush(es), exit 33)
```

The guest side shows the same path over serial:

```
CRUGOS: usb controller ready (xhci, 8 port(s))
CRUGOS: usb device 0627:0001 on port 5 (speed 3)
CRUGOS: usb mouse ready (endpoint 0x81, 4 byte reports)
CRUGOS: usb mouse move dx=3 dy=0
CRUGOS: usb mouse move dx=-6 dy=0
CRUGOS: usb selftest ok (0 key(s), 2 pointer event(s))
```

`scripts/boot-check.py` needed no change: gate 61 calls it with
`--expect-gpu=yes` exactly like gate 60, so the flag surface and every
earlier gate's argument order are unchanged. The descriptor `0627:0001` is
QEMU's `usb-mouse`, and it arrives on port 5 of the xHCI's eight ports.

## EDID mode selection

The 2D scanout of phase 8A took its mode from the host's `GET_DISPLAY_INFO`
answer or from the built-in list. That is enough for QEMU, but on a real
monitor the display's own preferred timing is the one it advertises in its
EDID, and a driver that ignores it can pick a size the panel then scales.
Phase 8B fetches the blob and parses it in the kernel.

The control queue issues `GET_EDID` (command 0x20) directly after
`GET_DISPLAY_INFO`, and the driver negotiates the EDID device feature as bit 2
of the feature word (`VIRTIO_GPU_F_EDID`; bit 1 is VIRGL and bit 0 is the 3D
half this phase does not use). A success response is 0x1104
(`GPU_RESP_OK_EDID`). The blob is copied into a 1024-byte buffer
(`GPU_EDID_MAX_BYTES`) and handed to `gpu_edid_parse()` in
`kernel/gpu/edid.c`, a pure file with no console, allocator or port I/O that
the host tests link directly.

### The parser rules

- the blob must be at least 128 bytes and must start with the eight-byte EDID
  header `00 ff ff ff ff ff ff 00`;
- the 128-byte block must sum to 0 modulo 256, the checksum byte included;
- the four 18-byte detailed timing descriptors begin at offset 54; a
  descriptor whose first word is zero is a display descriptor (monitor name,
  range limits, serial number) and is skipped rather than decoded as a
  timing;
- at most `GPU_EDID_MAX_MODES` (4) timings are kept, the first one is the
  preferred timing, and a blob with no usable timing is rejected because an
  EDID that names no mode cannot drive a scanout;
- the vendor id is one big-endian word at offset 8, decoded as three five-bit
  letters (`a = (id >> 10) & 31`, `b = (id >> 5) & 31`, `c = id & 31`); a
  letter outside 1..26 prints as `???`, so a malformed id cannot crash the
  parser or emit control characters.

Each timing decodes as `clock_khz` = the little-endian word 0 times 10,
`width = b2 | ((b4 & 0xf0) << 4)` with `hblank = b3 | ((b4 & 0x0f) << 8)`,
`height = b5 | ((b7 & 0xf0) << 4)` with `vblank = b6 | ((b7 & 0x0f) << 8)`, and
`refresh_hz = clock_khz * 1000 / (htotal * vtotal)`. Every read stays inside
the 18-byte descriptor it came from.

### Mode priority

`gpu_init()` now chooses in four steps and prints which one won:

1. `gpu.size=WxH` from the command line, when present and acceptable;
2. the preferred EDID timing;
3. the host `GET_DISPLAY_INFO` mode;
4. the built-in 1024x768 fallback, then 800x600 and 640x480, capped at
   4096x2160 and 8192 backing pages.

`gpu.size=` deliberately stays ahead of the EDID: it is what lets a gate, or
`make demo` with `DEMO_GPU_SIZE=`, pin a size on a host whose monitor
advertises something else. The two new lines say what happened:

```
CRUGOS: gpu edid 1152x864 (vendor RHT, product 0x1234, 1 mode(s))
CRUGOS: gpu mode 1152x864 (edid preferred)
```

The EDID line carries the preferred timing, the vendor, the product code and
the timing count; the mode line names the source (`edid preferred`,
`gpu.size`, `display info` or `fallback`), so a log explains why the scanout
is the size it is instead of leaving the reader to guess.

### Gate 62: make test-gpu-edid

`make test-gpu-edid` is gate 62. It pins the device to a timing that neither
the fallback nor QEMU's default can produce:

```
GPU_EDID_DEVICE ?= virtio-gpu-pci,id=gpu0,xres=1152,yres=864
GPU_EDID_QEMU_EXTRA ?= -vga none -device $(GPU_EDID_DEVICE)
```

1152x864 is neither the driver's 1024x768 fallback nor QEMU's usual 1280x800,
so a 1152x864 scanout can only come from a decoded EDID. The gate runs
`scripts/run-gpu.py --expect-edid`, then `boot-check.py` with
`--expect-gpu=yes --expect-gpu-edid=yes`, then `scripts/check-disk.py` like
every boot gate. The `--expect-edid` leg requires the EDID line, the
`(edid preferred)` mode line whose size equals the decoded EDID, at least one
EDID timing in that line's `N mode(s)` count and a screendump whose size
equals the timing. The verified output is:

```
gpu: edid 1152x864 (vendor RHT, product 0x1234, 1 mode(s))
gpu: device gpu0 screendump 1152x864
ok: virtio-gpu scanout 1152x864, mode from the EDID (14 transfer(s), 14 flush(es),
exit 33)
```

The runner also re-checks the four desktop pixel samples against the
1152x864 screendump, and `boot-check.py` reports `gpu yes gpuedid yes
gpucursor no` before the QEMU exit status 33. The EDID is QEMU's manufactured
one for the pinned device, not the host monitor's, so the 1152x864 timing is
the same on every machine: that is what makes the gate hermetic, and it is
also why the EDID gate does not need a real panel.

## The hardware cursor plane

The 12x19 sprite of phase 5D is a framebuffer citizen: the console and the
compositor save the pixels under it and recomposite them on every move.
Virtio-gpu has a hardware cursor plane that never touches guest memory, so
phase 8B moves the pointer onto it when the device offers one.

### The second queue

The cursor plane is queue 1, configured alongside control queue 0 and enabled
before `DRIVER_OK`. It has its own 64x64 `B8G8R8X8_UNORM` resource (resource
id 2, `GPU_CURSOR_PX` pixels a side) attached the same way as the scanout.
`UPDATE_CURSOR` and `MOVE_CURSOR` are 56-byte commands, and their wire layout
is the lesson of this release. `struct virtio_gpu_update_cursor` is a 24-byte
`virtio_gpu_ctrl_hdr`, then `struct virtio_gpu_cursor_pos` (`scanout_id`, `x`,
`y`, `padding`), then `resource_id`, `hot_x`, `hot_y` and a final padding
word: the scanout id comes FIRST, at offset 24. The driver writes
`GPU_CURSOR_CMD_BYTES` (56) bytes with scanout 0 at 24, x at 28, y at 32,
`pos.padding` 0 at 36, the resource id at 40, `hot_x` 0 at 44, `hot_y` 0 at 48
and a zero word at 52, so the hotspot stays 0,0 and matches the software
sprite's top-left anchor. Each command is submitted with one non-write
descriptor and a single 16-bit queue notify, and the driver only waits for the
used index to advance. A cursor move must never block the compositor on a
response structure, so the driver does not require a used-ring response for
these commands at all.

QEMU's `update_cursor()` begins with `if (cursor->pos.scanout_id >=
max_outputs) return;`, and that is what the 48/40-byte layout this release
first shipped walked into: with x at offset 24, QEMU read `scanout_id = 640`,
returned immediately and dropped every cursor command. No cursor was defined,
no plane appeared and no pixel was drawn, while the guest happily printed its
own show and move counters and looked healthy. That is why the pointer was
invisible in `make demo`, and why gate 63 could not see the bug: a counter in
the guest is not a pixel on the host.

The commands deliberately do not travel on the control queue: its 2D command
switch has no cursor case and answers 0x1200 (`err-unspec`), which an earlier
implementation read as a device failure and silently fell back to the sprite.
The cursor queue is the only path now.

### The sprite upload

A 2D virtio-gpu keeps a resource's pixels on the host side, and
`RESOURCE_ATTACH_BACKING` copies nothing, so the first `UPDATE_CURSOR` needs
the sprite already in the host resource. `gpu_cursor_upload()` therefore sends
a `TRANSFER_TO_HOST_2D` first (the 56-byte command form) and only then shows
the pointer; later shows reuse the uploaded image. Without that transfer the
plane can be defined and still show nothing, with no error anywhere.

### The sprite and the fallback

`include/crug/fb.h` gained `struct fb_cursor_ops` with `show`, `move` and
`hide` callbacks plus `fb_set_cursor_ops()`, and `kernel/arch/x86_64/fb.c`
defers every cursor show, move and hide to the installed ops. If no ops are
installed, or an op returns an error, the old software sprite is drawn into
the scanout exactly as before, so a device without a cursor plane keeps a
working pointer. `fb_attach_linear()` clears the hardware-shown state, which
is what stops a stale plane from lingering after a re-attach.

The sequence reads:

```
CRUGOS: gpu cursor ready (64x64, resource 2, hardware plane)
CRUGOS: gpu cursor show 640 400
CRUGOS: gpu cursor move 648 404
CRUGOS: gpu cursor selftest ok (2 show(s), 1 move(s))
```

A device that offers no cursor plane prints
`CRUGOS: gpu cursor queue unavailable (no cursor plane)` and stays on the
sprite. `gpu_selftest()` now hides the pointer, shows it at the centre, shows
it at centre+8/+4, hides it again and fails the boot self-test when either the
show or the move counter is zero, so a queue that accepts commands but never
updates is caught without a screenshot.

### Gate 63: make test-gpu-cursor

`make test-gpu-cursor` is gate 63. It boots the default device from
`GPU_QEMU_EXTRA` (QEMU manufactures a 1280x800 EDID under `-display none`),
runs `scripts/run-gpu.py --expect-cursor`, then `boot-check.py` with
`--expect-gpu=yes --expect-gpu-cursor=yes`; the command line appends
`gpu.cursor=hw` explicitly because the guest sprite is the default now. A
screendump of `gpu0` never contains the cursor plane, because the host scans it
out separately, so the serial counters are the evidence rather than pixels:

```
gpu: cursor plane 2 show(s), 1 move(s)
ok: virtio-gpu scanout 1280x800, hardware cursor (14 transfer(s), 14 flush(es),
exit 33)
```

The boot-check line reports `gpu yes gpuedid no gpucursor yes`, the QEMU exit
status is 33 and `e2fsck` is clean, as in every boot gate. For most of this
release that counter check felt like enough, and it was not: while the command
layout was wrong the guest still counted its own shows and moves, so gate 63
passed while no cursor ever reached the host. A screendump cannot settle it
either, because the host scans the cursor out separately, so gate 63 stays as a
cheap smoke test and gate 64 is the real proof.

### Gate 64: make test-gpu-cursor-shape

`make test-gpu-cursor-shape` is gate 64 and the pixel-level proof. It boots
`make run` under `QEMU_DISPLAY=vnc=127.0.0.1:<port>` with
`QEMU_EXTRA='-vga none -device virtio-gpu-pci,id=gpu0'` and
`CMDLINE_EXTRA='wb.enable=1 wb.wait=30 gpu.enable=1 gpu.test=1 gpu.cursor=hw'`
(a new
`--cmdline` option overrides that line), then
`scripts/run-gpu-cursor-shape.py` speaks RFB 3.8 to QEMU: security type 1, a
shared `ClientInit`, encodings -223 (`VNC_DESKTOP_SIZE`) and -314
(`VNC_ALPHA_CURSOR`), and a 2x2 incremental `FramebufferUpdateRequest` every
0.2 s, because a VNC server sends nothing without a request.

The alpha-cursor rectangle is the evidence. `check_shape` demands 64x64, at
least 100 opaque pixels, and both the white border (`0xFFFFFF`) and the amber
body (`0xFFD040`):

```
gpu: cursor shape 64x64 at 0,0 (120 opaque pixel(s), 2 colour(s))
ok: hardware cursor shape 64x64 carries 120 opaque pixel(s) in 2 colour(s), exit 33
```

Then `boot-check.py --expect-gpu=yes --expect-gpu-cursor=yes` and
`check-disk.py` finish it. The gate is hermetic in the useful way: without a
working `UPDATE_CURSOR` no cursor rect ever arrives, so the script times out
with a cursor-shape timeout instead of asserting something vague, and the
layout bug has nowhere to hide. The same probe with the demo command line
(`wb.enable=1 wb.wait=0 wb.desk=1 wb.dr=1 wb.dsel=1 wb.screens=1 wb.cx=1
wb.shell=1 wb.fill=12 wb.hold=20 gpu.enable=1 gpu.test=1 gpu.cursor=hw`) also
returned the
64x64 shape, which is the proof that the `make demo` pointer is back.

The knobs are `GPU_SHAPE_WB_WAIT` (30), `GPU_SHAPE_BOOT_TIMEOUT_SEC` (180),
`GPU_SHAPE_TIMEOUT_SEC` (240), `GPU_SHAPE_RUN_TIMEOUT_SEC` (300) and
`GPU_SHAPE_QEMU_EXTRA` (`-vga none -device virtio-gpu-pci,id=gpu0`), all
overridable.

## Which pointer the guest draws

Phase 8B installs a cursor op set as soon as the device offers a cursor plane,
and `fb_cursor_composite()` in `kernel/arch/x86_64/fb.c` starts with
`if (!g_ready || !g_cursor_wanted || g_cursor_drawn || g_cursor_ops) return;`,
so the software sprite went quiet the moment the plane came up. That is
harmless on a VNC client, which composites the virtio-gpu cursor plane, and
wrong in a GTK window: `gd_update_cursor()` in `ui/gtk.c` calls
`gdk_window_set_cursor(window, s->null_cursor)` whenever the pointer is
grabbed or absolute (`s->ptr_owner == vc || qemu_input_is_absolute()`) and
expects the guest to paint the pointer into the scanout, but a GTK window
never composites that plane. Gate 64 had already proved the plane carries the
right pixels, so the invisible pointer was a surface problem, not a wire
problem.

`gpu.cursor=sw|hw` picks the side of the fix. `sw` is the default: the guest
composites its 12x19 sprite (`FB_CURSOR_BODY_RGB` `0x00FFD040` body,
`0x00FFFFFF` border) into the framebuffer, so the pointer is real scanout
content that every window manager, VNC viewer and `screendump` can see. `hw`
restores the phase 8B plane on virtqueue 1. The parser is
`gpu_cursor_mode_parse(const char *text, u32 len)` in `kernel/gpu/gpu_util.c`:
1 for `hw`, 0 for `sw` and -1 for anything else, with an explicit length and
an exact case-sensitive compare; a rejected value prints `CRUGOS: gpu cursor
mode ignored (use gpu.cursor=sw|hw)` and keeps the sprite. Sprite mode prints
`CRUGOS: gpu cursor software sprite (gpu.cursor=sw)` in place of
`CRUGOS: gpu cursor ready (64x64, resource 2, hardware plane)`, and a plane
that fails to come up reports `CRUGOS: gpu cursor unavailable (hardware
plane)`.

`make demo` passes `gpu.cursor=$(DEMO_CURSOR)` in `DEMO_CMDLINE` and defaults
`DEMO_CURSOR` to `sw`, so the demo pointer is part of the framebuffer it
already shows; `make demo DEMO_CURSOR=hw` is the phase 8B plane in a window.
Gates 63 and 64 append `gpu.cursor=hw` explicitly, because their subject is
the device plane and the sprite is the default.

### Gate 65: make test-gpu-cursor-sw

`make test-gpu-cursor-sw` is gate 65 and the pixel proof for the default. It
boots the default `virtio-gpu-pci` device from `GPU_QEMU_EXTRA` with
`wb.enable=1 wb.wait=N gpu.enable=1 gpu.test=1 gpu.cursor=sw` and runs
`scripts/run-gpu.py --expect-sprite`, which waits for the sprite marker, fails
if `CRUGOS: gpu cursor ready` appears, reads the last pointer position from
the serial log (`CRUGOS: gfx ready (WxH, cursor at X,Y)`, `CRUGOS: gfx cursor
at (X, Y)` or `CRUGOS: wb cursor at (X, Y)`) and counts the `(255, 208, 64)`
pixels inside the 12x19 sprite box anchored there, demanding at least 40:

```
gpu: sprite at (640, 400) carries 71 body px
ok: virtio-gpu scanout 1280x800, software cursor sprite (15 transfer(s), 15
flush(es), exit 33)
```

Then `boot-check.py --expect-gpu=yes` and `check-disk.py` finish it as usual.
The evidence is pixels in the scanout a window manager would show, not a
guest counter: the counter lesson of gate 63 is exactly why this gate reads
the surface instead. A body count below 40, or a `gpu cursor ready` line in
sprite mode, fails the gate.

## Failure modes

Every driver failure keeps the boot alive on the software framebuffer and
prints one line with the step that failed. That is the design: a display device
is optional, and a kernel that refuses to boot because a scanout did not come
up cannot be debugged over serial.

| Line | Cause |
| --- | --- |
| `CRUGOS: virtio-gpu disabled (gpu.enable=0), software framebuffer` | `gpu.enable=0` |
| `CRUGOS: virtio-gpu ignored (legacy-only device, modern transport required)` | only device id 0x1010 is present |
| `CRUGOS: virtio-gpu ignored (no virtio capabilities)` | the capability chain is incomplete |
| `CRUGOS: virtio-gpu ignored (common BAR unmapable)` | the BAR cannot be mapped |
| `CRUGOS: virtio-gpu ignored (features rejected)` | the device offers nothing usable |
| `CRUGOS: virtio-gpu ignored (no control queue)` | queue 0 could not be configured |
| `CRUGOS: virtio-gpu ignored (no display mode)` | the host offered no acceptable mode |
| `CRUGOS: virtio-gpu ignored (no backing memory for WxH)` | the page allocator refused the backing |
| `CRUGOS: virtio-gpu ignored (resource create failed)` | `RESOURCE_CREATE_2D` failed |
| `CRUGOS: virtio-gpu ignored (attach backing failed)` | `RESOURCE_ATTACH_BACKING` failed |
| `CRUGOS: virtio-gpu ignored (set scanout failed)` | `SET_SCANOUT` failed |
| `CRUGOS: virtio-gpu ignored (framebuffer attach failed)` | `fb_attach_linear()` refused |
| `CRUGOS: virtio-gpu command 0x<type> answered 0x<code> (<name>)` | a late command returned an error |
| `CRUGOS: gpu cursor queue unavailable (no cursor plane)` | queue 1 could not be configured; the pointer stays on the sprite |
| `CRUGOS: gpu cursor unavailable (hardware plane)` | `gpu.cursor=hw` but queue 1 or resource 2 failed; the guest sprite takes over |
| `CRUGOS: gpu cursor mode ignored (use gpu.cursor=sw\|hw)` | the token value is neither `sw` nor `hw`; the sprite stays |
| sprite body below 40 px at the reported pointer, gate 65 fails | the guest sprite is not reaching the scanout |
| no cursor rect over VNC, gate 64 times out | a dropped or rejected cursor command: the device read a bad `scanout_id` and returned early |
| `CRUGOS: gpu selftest failed (present)` | the self-test could not present |

The same "ignore and continue" rule applies to a device that answers slowly:
the deadline is two seconds, and a timeout is reported through the same answer
line with a timeout name rather than by spinning forever.

## Limits

- 2D scanout only. There is no virglrenderer context, no Vulkan, no command
  stream, no shader and no "accelerated triangle"; that acceptance item of the
  specification's phase 8 remains deferred. Phase 8B adds EDID mode selection
  and the hardware cursor plane, which are display plumbing, not 3D: nothing
  in this release should be read as partial 3D support.
- Two resources, one scanout, one display. Resource 1 is the desktop, resource
  2 is the 64x64 cursor. There is no overlay, no multi-head (`max_outputs=2`
  is not gated) and no clone. The pointer is normally painted into resource 1
  by the software sprite; `gpu.cursor=hw` moves it onto resource 2 instead.
- No mode-set after boot and no hotplug. The mode is chosen once from the
  token, the EDID preferred timing, the host display info or the built-in
  list, and a display change needs a reboot.
- One command in flight with a two-second deadline, and a queue of at most 64
  entries. The driver is not interrupt-driven; it polls.
- The backing is contiguous physical memory capped at
  `GPU_BACKING_MAX_PAGES` 8192 pages, which is why 4096x2160 is the largest
  mode. A larger request is refused rather than spread over a scatter list.
- The damage rectangle is a bounding box, not a list. Repainting two windows at
  opposite corners of the screen transfers the whole area between them.
- The console batches per line, so a glyph written without a newline is not
  visible until the line ends or the cursor wraps. The desktop flushes at the
  end of each repaint instead.
- The only automated proof is a QEMU screendump of the `gpu0` device plus
  the transfer/flush counters. No physical GPU, no real virtio-gpu host and no
  non-QEMU device was tested; the driver has been exercised against one
  implementation.
- The guest can still fall back silently: a boot that reaches the desktop on
  the firmware framebuffer looks normal apart from the missing
  `virtio-gpu ready` line, so a gate must ask for it explicitly with
  `--expect-gpu=yes`.
- `make test-host` covers the pure helpers only (command encoding, response
  classes, rectangle clipping, backing-page arithmetic, display pick, damage
  merge, the EDID parser and its rejection cases, the `gpu.cursor` parser), so
  it reports 8528 checks.
  The command sequence, the queues, the present path and the cursor plane are
  proved only by the gates.
- The EDID is parsed but not acted on beyond the mode: no colour depth, gamma,
  aspect-ratio or range-limit handling, and no mode list is offered to
  userspace. `gpu.size=` still overrides the preferred timing by design.
- The hardware cursor is a 64x64 plane with one sprite, and the default is the
  guest sprite, not the plane: `gpu.cursor=sw` composites a 12x19 sprite into
  the framebuffer, so a `screendump` does show the pointer, while `hw` is only
  visible on a surface that composites the plane (a VNC client does, a GTK
  window does not). There is no per-window cursor and no alpha blending beyond
  the device's own scanout. Gate 63 asserts the show and move counters, gate 64
  reads the alpha-cursor rectangle from a VNC client and gate 65 counts the
  sprite body pixels in the scanout.
- No 3D on this host in any case: there is no `/dev/dri` render node, so QEMU
  refuses virgl with `The display backend does not have OpenGL support
  enabled` and `egl: no drm render node available`, and blob resources stay
  off unless the device is created with `blob=on`. The 3D half is not merely
  unimplemented, it is untestable here.
