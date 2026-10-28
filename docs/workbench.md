# Workbench: intuition.device and the phase 6A desktop

Phase 6A starts the Workbench: a small software window manager in the kernel,
`intuition.device`, that owns up to four windows on the GOP framebuffer.
Windows are raised and activated by a left click, dragged by their title bar,
and repainted straight into the linear framebuffer with the primitives in
`fb.h`. There are no application tasks behind them yet -- this phase is the
kernel-side foundation that the Workbench userspace will sit on -- so the only
resident of the desktop is the bring-up module itself, which opens two demo
windows when the loader command line asks for it.

```
  USB HID boot mouse (drivers/usb/xhci.c)
           |  usb_hid_mouse_decode()
           v
     input.device pointer state (kernel/input/input.c)
           |  input_pointer_read()
           v
       gfx.device (kernel/gfx/gfx.c)      cursor sprite -> framebuffer
           |  pointer hook (gfx_set_pointer_hook)
           v
     intuition.device (kernel/gfx/intuition.c)
           |  fb_fill_rect() / fb_frame_rect() / fb_draw_text()
           v
   framebuffer console (kernel/arch/x86_64/fb.c)
```

## Why the desktop lives in the kernel

The specification puts the Workbench on top of the executive, so the window
list, the stacking order, the activation rule and the drag state are kernel
state for now. Keeping them here has two practical benefits for this phase:

- The state machine is host-testable. `intuition.device` draws only through
  the five framebuffer primitives, so `tests/host/test_intuition.c` links the
  real `kernel/gfx/intuition.c` against counting stubs and checks the window
  list, the hit test, the drag arithmetic and the repaint counts without QEMU.
- The gate can prove the path end to end. A QMP-injected mouse report becomes
  a cursor motion, a title-bar drag and finally a coloured title bar in a QEMU
  screendump, so the phase is verified by pixels rather than by log lines.

`intuition.device` is disabled unless `wb.enable=1` is on the command line, so
every other boot keeps the plain text console and the existing gates are
untouched.

## intuition.device

`include/crug/intuition.h` fixes the geometry and the palette:

| constant | value | meaning |
| --- | --- | --- |
| `INTUITION_MAX_WINDOWS` | 4 | window slots in the kernel |
| `INTUITION_TITLE_HEIGHT` | 20 | title bar height in pixels |
| `INTUITION_BORDER` | 2 | border drawn between frame and content |
| `INTUITION_MIN_WIDTH` | 120 | smallest window a caller can open |
| `INTUITION_MIN_HEIGHT` | 80 | smallest window a caller can open |
| `INTUITION_DESKTOP_RGB` | `0x00102030` | desktop background |
| `INTUITION_WINDOW_RGB` | `0x00081018` | window body |
| `INTUITION_FRAME_RGB` | `0x00ffffff` | one-pixel window frame |
| `INTUITION_TITLE_ACTIVE_RGB` | `0x00ffb000` | title bar of the active window |
| `INTUITION_TITLE_IDLE_RGB` | `0x00808080` | title bar of the other windows |
| `INTUITION_TITLE_TEXT_RGB` | `0x00000000` | title text |
| `INTUITION_BODY_TEXT_RGB` | `0x00e0e0e0` | body text |

API:

| function | meaning |
| --- | --- |
| `void intuition_init(void)` | reset the window table, read the screen size from `fb_dimensions()` |
| `int intuition_open(i32 x, i32 y, u32 w, u32 h, const char *title, const char *body)` | open a window; returns its id (1..4) or -1 |
| `int intuition_close(int id)` | close a window; 0 or -1 |
| `void intuition_redraw(void)` | repaint the desktop and every window bottom to top |
| `void intuition_pointer_event(const struct input_pointer *event)` | consume one pointer report |
| `int intuition_window_rect(int id, i32 *x, i32 *y, u32 *w, u32 *h)` | read a window rectangle |
| `int intuition_active(void)` | active window id or -1 |
| `void intuition_set_reporting(int on)` | enable or silence the gate markers |
| `u64 intuition_drag_count(void)` | completed title-bar drags |
| `u32 intuition_window_count(void)` | open windows |

### Window anatomy

`intuition_open()` clamps the width to at least 120 and the height to at least
80, then clamps the rectangle into the screen (`win_clamp()`), so a window can
never be opened partly off-screen or larger than the framebuffer. The first
free slot wins, the new window goes to the top of the stacking order and
becomes the active one. Opening a window repaints the whole desktop, and
because the repaint walks the stacking order bottom to top, `g_frame_calls` in
the host tests grows by the number of open windows on every open.

A window is a 1-pixel white frame, a 20-pixel title bar at the border offset,
a 12x12 close-ish gadget on the right of the title bar (it anchors the bar
visually; it does not close anything yet) and a body of 8x16 text on a 20-pixel
line pitch. Body text lines are split at `\n` and truncated to 47 characters.

`intuition_close()` unlinks the window from the stacking order, clears the drag
state if it was being dragged, and hands activation to the new top window, or
to -1 when the last window is gone.

### Pointer semantics

`intuition_pointer_event()` reads the cursor at `fb_cursor_x()` /
`fb_cursor_y()`, i.e. after `gfx.device` has already applied the report, so
hit testing always uses the final cursor position:

- `changes & 0x01` with `buttons & 0x01` (left press): `win_hit()` walks the
  stacking order top down for the first window that contains the point. On a
  hit the window is raised, made active, and -- when the press is on the title
  bar -- a drag starts with the grab offset `(px - w->x, py - w->y)`. A press
  on the desktop does nothing to the desktop but also raises nothing.
- `changes & 0x01` without the left button (release): if a drag is running the
  window is already where the last motion put it, so the release prints
  `moved to (x, y)` and counts one drag.
- motion while a drag is running: the new origin is the cursor minus the grab
  offset, clamped into the screen; the desktop is repainted.
- anything else (motion without a drag, button changes that are not the left
  button) leaves the window state alone.

The full desktop is repainted on every state change. At 1024x768 that is a few
hundred thousand pixels, which is cheap enough for a polled bring-up phase and
keeps the renderer simple: no dirty rectangles, no clipping stacks, no
save-under (the framebuffer primitives restore and recomposite the pointer
sprite themselves).

## The desktop bring-up

`kernel/gfx/workbench.c` is the phase 6A resident. `wb_init()`:

1. requires `fb_ready()` and `fb_dimensions()`; otherwise it prints
   `CRUGOS: wb framebuffer absent` and returns -1.
2. turns the text console off the framebuffer with `fb_set_text_enabled(0)`.
   `fb_putc()` and the scroll path keep writing to the serial port, so boot
   diagnostics survive while windows own the screen.
3. calls `intuition_init()`, enables the gate markers when `wb.drag=` asked
   for one, and opens the two demo windows:

   | id | rectangle | title | body |
   | --- | --- | --- | --- |
   | 1 | (80, 90) 360x220 | `Workbench` | `CrugOS Workbench` / `phase 6a` / `software rendered` |
   | 2 | (480, 300) 320x200 | `Shell` | `4.OS` / `crugos>` |

4. installs `wb_report_cursor()` as the `gfx.device` pointer hook, shows the
   sprite and installs `wb_console_poll()` as the `input.device` poll hook, so
   a program waiting for a character still pumps `usb_poll()` and
   `gfx_pointer_poll()`.
5. prints `CRUGOS: wb desktop ready (WxH, N window(s))`.

`wb_selftest()` is silent unless `wb.enable=1` was set and returns success
when `wb.enable=1` was set but the framebuffer is missing (the bring-up
already reported that). It pulls `input_poll()` in the wait loop -- `input_poll()`
runs the console hook, which pumps the USB mouse and drains the pointer state
into the hook -- until `intuition_drag_count()` reaches `wb.drag=` (default 1)
or `wb.wait=` expires (default 5 s). A timeout prints
`CRUGOS: wb pointer timeout (drag N/M)` and panics the boot; success prints
`CRUGOS: wb selftest ok (N drag(s))` and then lingers for 1.5 s so the gate's
final screendump cannot race the rest of the boot to the QEMU exit.

## Command line

`wb_cmdline()` tokenises `bi->command_line` like the networking and audio
tokens do. Values are decimal, capped, and ignored when malformed:

| token | range | meaning |
| --- | --- | --- |
| `wb.enable=1` | 0/1 | take the framebuffer away from the text console and open the demo desktop |
| `wb.wait=S` | 0..120 | seconds the self-test waits for the drags |
| `wb.drag=N` | 0..4 | drags the self-test requires (also enables the markers) |

The gate boots with `wb.enable=1 wb.wait=30 wb.drag=1`.

## Boot integration and markers

`kernel/core/main.c` runs `wb_cmdline(bi)` and then `wb_init()` right after
`gfx_init()` / `gfx_cmdline()`, and `kernel/core/selftest.c` appends
`wb_selftest()` after the graphics self-test, panicking with
`selftest: workbench` when it fails. A normal boot prints nothing new.

In gate mode the serial log carries:

```
CRUGOS: wb window 1 opened (80, 90, 360x220)
CRUGOS: wb window 2 opened (480, 300, 320x200)
CRUGOS: wb desktop ready (1024x768, 2 window(s))
CRUGOS: wb cursor at (512, 312)
CRUGOS: wb window 2 active
CRUGOS: wb window 2 drag start (512, 312)
CRUGOS: wb window 2 drag to (557, 337)
CRUGOS: wb window 2 moved to (570, 350)
CRUGOS: wb selftest ok (1 drag(s))
CRUGOS: phase 6a boot ok
```

| marker | when |
| --- | --- |
| `CRUGOS: wb desktop ready (WxH, N window(s))` | the desktop took the screen |
| `CRUGOS: wb framebuffer absent` | `wb.enable=1` without a usable framebuffer |
| `CRUGOS: wb window N opened (x, y, WxH)` | a window was opened (reporting on) |
| `CRUGOS: wb window N active` | a left press hit window N |
| `CRUGOS: wb window N drag start (x, y)` | the press landed on the title bar |
| `CRUGOS: wb window N drag to (x, y)` | a motion moved the dragged window |
| `CRUGOS: wb window N moved to (x, y)` | the button was released after a drag |
| `CRUGOS: wb cursor at (x, y)` | pointer motion with reporting on |
| `CRUGOS: wb pointer timeout (drag N/M)` | the self-test wait expired |
| `CRUGOS: wb selftest ok (N drag(s))` | the requested drags happened |

## Host tests

`make test-host` links the real `kernel/gfx/intuition.c` and
`kernel/gfx/workbench.c` and reports 2487 checks. `tests/host/test_intuition.c`
replaces the framebuffer primitives with counting stubs and the cursor
accessors with a settable fake (`test_fb_fake_reset()` /
`test_fb_cursor_set()` from `tests/host/test_gfx.c`), then checks:

- clamping of undersized windows (120x80) and of windows that would fall off
  the screen, the four-window limit, closing an unknown window, activation
  hand-over when the active window closes, and the exact repaint counts.
- hit testing: a press on a window body activates it, a press on the desktop
  keeps the active window, a press on an overlapping stack picks the top one.
- title-bar drags: the grab offset, the new origin after a motion, the release
  marker and drag counter, clamping at the right/bottom and left/top edges,
  and the fact that a body press never drags.
- closing a window mid-drag, which must drop the drag without counting it.
- title text and body text reaching the framebuffer primitives.

## Gate

`make test-workbench` runs `scripts/run-workbench.py`, which boots:

```
make run DISK_IMG=build/x86_64/disk.img CRYPT_PASS= NET_MODE=none \
    QEMU_DISPLAY=none SERIAL_TCP= QEMU_EXTRA="-qmp unix:... -device qemu-xhci,id=xhci" \
    MACHINE_EXTRA="usb=off,i8042=off" CMDLINE_EXTRA="wb.enable=1 wb.wait=30 wb.drag=1"
```

`usb=off,i8042=off` removes QEMU's own input devices, so the hotplugged
usb-mouse is the only pointer and `input-send-event` cannot be served by
anything else. The script then:

1. waits for `CRUGOS: wb desktop ready`, opens the QMP socket and
   `device_add`s a `usb-mouse` as `mouse0` on `xhci.0` (the hotplug path of
   phase 5A/5B).
2. reads the starting cursor from the `CRUGOS: gfx ready (WxH, cursor at X,Y)`
   banner (the screen centre, 512,384 on the gate's 1024x768). The Workbench
   mode does not pass `gfx.wait=`, so the sprite marker is absent and the
   banner is the deterministic source of the starting point.
3. moves the pointer to the title bar of window 2 (512,312), presses the left
   button and sends two `(+45,+25)` relative motions, checking every reported
   `CRUGOS: wb cursor at` position, then releases.
4. requires `CRUGOS: wb window 2 moved to (570, 350)` -- the grab offset is
   (32,12), so a 90-pixel drag moves the window by exactly that -- and
   `CRUGOS: wb selftest ok (1 drag(s))`.
5. takes a QMP `screendump`, parses the P6 PPM and checks three pixels:
   `(570+120, 350+6)` is the active title colour `(255,176,0)`, the old title
   bar position is no longer active, and the frame of window 1 at `(80,90)` is
   white. The sample sits right of the title text, which is black and would
   otherwise read as a missing bar.
6. waits for exit status 33 and prints
   `ok: workbench title-bar drag (N drag(s), window 2 at (x, y), exit 33)`.

`make test-workbench` then runs `scripts/boot-check.py` with `EXPECT_WB=yes`
(the phase 6a boot marker, the desktop marker, a `wb selftest ok`, at least one
`wb window N opened`, one `wb window N moved to`, and one `wb cursor at` with
numeric coordinates) and `scripts/check-disk.py` for the ext4 root.

## Limits

- Gate-only bring-up: the desktop is enabled by `wb.enable=1` and the demo
  windows are hardcoded. There are no application tasks, no Workbench
  userspace and no window content owned by another program yet.
- Four windows, no resize, no gadgets that do anything (the close-ish box is
  decoration), no menus, no icons, no clipboard, no double-click semantics.
- A full desktop repaint per state change, and a polled pointer path: the
  latency is the 10 ms `usb.poll` tick until interrupts or a compositor make
  it cheaper.
- Text is 8x16 with a 20-pixel line pitch and a 47-character line cap; there
  is no word wrapping, no scrollback inside a window and no fonts.
- The keyboard still reaches the serial console, not a Workbench window.
- No clipping stack: `fb_fill_rect()` and `fb_draw_text()` clip to the screen,
  so a window cannot draw outside itself into another window's area.
