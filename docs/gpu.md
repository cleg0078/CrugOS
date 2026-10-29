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
acceptance item is deferred to a future 8B. Phase 8A is about the display
plumbing a 3D renderer would later draw through: a device, a resource, a
scanout, and a present path that tells the host what changed.

The software path is not replaced, it is fronted. With no virtio-gpu device,
with `gpu.enable=0`, or after any driver failure, the kernel keeps booting on
the GOP framebuffer exactly as it did in phase 7C. All 59 earlier gates still
boot that way and are unchanged. `make test-gpu` is the 60th gate and the
only one that boots a GPU.

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

The resource is never resized and never replaced. There is no second resource,
no cursor plane and no scanout for an overlay; a hardware cursor would need a
second resource plus cursor commands, and the pointer sprite is still drawn by
the software renderer into the same pixels.

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
| `gpu.size=WxH` | none | preferred scanout size; the host mode and then the built-in list are the fallbacks |
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

## The gate

`make test-gpu` is gate 60. It is the only gate that boots without any
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
ok: boot markers found, version 0.46.0 avx2 yes raid no nvme no rebuild no
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
| `CRUGOS: gpu selftest failed (present)` | the self-test could not present |

The same "ignore and continue" rule applies to a device that answers slowly:
the deadline is two seconds, and a timeout is reported through the same answer
line with a timeout name rather than by spinning forever.

## Limits

- 2D scanout only. There is no 3D, no virglrenderer, no Vulkan, no command
  stream, no shader and no "accelerated triangle"; the specification's phase 8
  acceptance item is deferred to a future 8B. Nothing in this phase should be
  read as partial 3D support.
- One resource, one scanout, one display. No second resource, no overlay, no
  hardware cursor plane and no multi-head. The pointer is still painted into
  the same pixels by the software renderer.
- No mode-set after boot and no hotplug. The mode is chosen once from the
  token, the host display info or the built-in list; a display change needs a
  reboot.
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
- `make test-host` covers the pure helpers only (encoding, response classes,
  rectangle clipping, backing-page arithmetic, display pick and damage merge),
  so it reports 8488 checks. The command sequence, the queue and the present
  path are proved only by the gate.
