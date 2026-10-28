# Workbench: intuition.device, menus and gadgets

Phase 6A starts the Workbench: a small software window manager in the kernel,
`intuition.device`, that owns up to four windows on the GOP framebuffer.
Windows are raised and activated by a left click, dragged by their title bar,
and repainted straight into the linear framebuffer with the primitives in
`fb.h`. Phase 6B adds the interaction model on top: a screen-top menu bar with
popup menus, labelled gadgets inside a window body, a close gadget at the right
end of every title bar, and a typed event queue that reports what the user
did. There are no application tasks behind the desktop yet -- the kernel-side
device is what the Workbench userspace will sit on -- so the only resident is
the bring-up module itself, which opens two demo windows when the loader
command line asks for it.

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
| `INTUITION_MAX_MENUS` | 2 | menus per window |
| `INTUITION_MAX_ITEMS` | 6 | items per menu |
| `INTUITION_MAX_GADGETS` | 4 | gadgets per window |
| `INTUITION_EVENT_QUEUE` | 16 | events held before drops |
| `INTUITION_MENU_BAR_HEIGHT` | 18 | menu bar height in pixels |
| `INTUITION_MENU_ITEM_HEIGHT` | 18 | one popup row |
| `INTUITION_MENU_ITEM_WIDTH` | 150 | popup width in pixels |
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
| `INTUITION_MENUBAR_RGB` | `0x00c0c0c0` | menu bar background |
| `INTUITION_MENU_BG_RGB` | `0x00c0c0c0` | popup background |
| `INTUITION_MENU_TEXT_RGB` | `0x00000000` | menu text |
| `INTUITION_MENU_HIGHLIGHT_RGB` | `0x00ffb000` | highlighted strip or row |
| `INTUITION_GADGET_RGB` | `0x00a0a0a0` | gadget fill |
| `INTUITION_GADGET_PRESSED_RGB` | `0x00606060` | gadget fill while held |
| `INTUITION_GADGET_TEXT_RGB` | `0x00000000` | gadget label |

API:

| function | meaning |
| --- | --- |
| `void intuition_init(void)` | reset the window table, read the screen size from `fb_dimensions()` |
| `int intuition_open(i32 x, i32 y, u32 w, u32 h, const char *title, const char *body)` | open a window; returns its id (1..4) or -1 |
| `int intuition_close(int id)` | close a window; 0 or -1 |
| `int intuition_set_menus(int id, const struct intuition_menu *menus, u32 count)` | attach up to two strip menus to a window; -1 when the window or the table is missing |
| `int intuition_add_gadget(int id, i32 x, i32 y, u32 w, u32 h, const char *label)` | add a body-relative gadget; returns its index or -1 |
| `int intuition_event_poll(struct intuition_event *out)` | pop the oldest event; 1 when one was copied |
| `int intuition_menu_open(void)` | open menu index in the active window, or -1 |
| `int intuition_menu_hover(void)` | highlighted popup row, or -1 |
| `u32 intuition_menu_opens(void)` | menus opened so far |
| `u32 intuition_gadget_clicks(void)` | gadget clicks so far |
| `u64 intuition_close_count(void)` | close-gadget clicks so far |
| `u64 intuition_action_count(void)` | menu, gadget and close actions |
| `u32 intuition_event_count(void)` | events waiting in the queue |
| `u32 intuition_event_dropped(void)` | events dropped on a full queue |
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
a 12x12 close gadget at `x + w - 2 - 18`, `y + 2 + 4` on the right of the
title bar (in phase 6B it closes the window) and a body of 8x16 text on a
20-pixel line pitch. Body text lines are split at `\n` and truncated to 47 characters.

`intuition_close()` unlinks the window from the stacking order, clears the drag
state if it was being dragged, and hands activation to the new top window, or
to -1 when the last window is gone.

### Pointer semantics

`intuition_pointer_event()` reads the cursor at `fb_cursor_x()` /
`fb_cursor_y()`, i.e. after `gfx.device` has already applied the report, so
hit testing always uses the final cursor position:

- `changes & 0x01` with `buttons & 0x01` (left press) is resolved in a fixed
  order, so the most specific target wins: a row of the open popup selects its
  item, a press on the menu bar opens (or switches) a strip, a press on a close
  box closes that window, a press on a body gadget arms a click, and only then
  does `win_hit()` walk the stacking order top down for the first window that
  contains the point. On a window hit the window is raised, made active, and --
  when the press is on the title bar -- a drag starts with the grab offset
  `(px - w->x, py - w->y)`. A press on the desktop raises and closes nothing.
  The release (`changes & 0x01` without the left button) completes a pending
  gadget click, so a press that leaves its gadget and releases elsewhere does
  not count.
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

## Menus

The active window owns the menu bar. `intuition_set_menus()` copies up to
`INTUITION_MAX_MENUS` strips into the window (the table is copied, the strings
stay the caller's), and the bar shows that window's strips even while another
window is stacked above it. A table entry without a title or without items
ends the table, so a half-built table is never drawn.

The bar is 18 pixels tall. Strip 0 starts at x = 4 and every strip is
`strlen(title) * 8 + 16` pixels wide, so strips abut. A press on the bar opens
the strip under the pointer, or switches to it while a popup is open, and
prints `CRUGOS: wb menu '<title>' open (window N)`.

The popup is `INTUITION_MENU_ITEM_WIDTH` (150) pixels wide, starts at the
strip's left edge and at y = 18, and is `4 + items * 18` pixels tall: a
2-pixel inset, one 18-pixel row per item and 2 pixels of padding at the
bottom. Row N spans y = 20 + 18N .. 38 + 18N. Hovering a row paints it amber
and prints `CRUGOS: wb menu '<title>' item N highlighted ('<item>')` once per
change; leaving the popup clears the highlight. A press on a row closes the
popup, counts one action, prints
`CRUGOS: wb menu '<title>' selected ('<item>')` and queues
`INTUITION_EVENT_MENU` with the window id, the menu index and the item index.
A press elsewhere while a popup is open dismisses it: in the bar the strip
under the pointer opens instead, anywhere else nothing else happens.

## Gadgets and the close gadget

`intuition_add_gadget()` stores up to `INTUITION_MAX_GADGETS` labelled
rectangles in body-relative coordinates (`body x = window x + 2`,
`body y = window y + 2 + 20`). A gadget that would stick out of the body is
cut down to the body edge, and one smaller than 16x16 or without a label is
rejected, so a gadget can never draw outside its window.

A press inside a gadget arms it and paints it `INTUITION_GADGET_PRESSED_RGB`;
the release inside the same gadget is the click. Moving off the gadget and
releasing elsewhere cancels it, exactly like a button the pointer left. A
click counts one action, prints `CRUGOS: wb gadget '<label>' clicked in
window N` and queues `INTUITION_EVENT_GADGET` with the window id and the
gadget index; the first match in insertion order wins, so overlapping gadgets
resolve top-down.

The 12x12 close gadget sits at the right end of every title bar. Pressing it
closes the window at once (the release is not needed), prints `CRUGOS: wb
window N closed (gadget)`, counts one close and one action, and queues
`INTUITION_EVENT_CLOSE`. Activation falls to the new top window, or to -1 when
the last window is gone. A press just left of the box is still a title-bar
press and starts a drag.

## The event queue

`struct intuition_event` is the typed report the desktop hands to its caller:

| field | meaning |
| --- | --- |
| `type` | `INTUITION_EVENT_ACTIVATE`, `_DRAG`, `_MENU`, `_GADGET` or `_CLOSE` (0 = none) |
| `window` | window id the event belongs to, 0 when it has none |
| `index` | menu index or gadget index, -1 when unused |
| `item` | selected menu item index, 0 when unused |
| `x`, `y` | window origin when the event was generated |

Activation and completed drags are queued as well, so a caller that drains the
queue sees the whole story of a click and not only the actions the kernel
already handled. The queue holds `INTUITION_EVENT_QUEUE` (16) events; when it
is full the newest event is dropped and counted, so `intuition_event_count()`
plus `intuition_event_dropped()` accounts for every event the device produced.
`intuition_event_poll()` returns 1 and copies the oldest event, or 0 when the
queue is empty. `intuition_init()` resets the queue and both counters.

## The desktop bring-up

`kernel/gfx/workbench.c` is the phase 6A resident. `wb_init()`:

1. requires `fb_ready()` and `fb_dimensions()`; otherwise it prints
   `CRUGOS: wb framebuffer absent` and returns -1.
2. turns the text console off the framebuffer with `fb_set_text_enabled(0)`.
   `fb_putc()` and the scroll path keep writing to the serial port, so boot
   diagnostics survive while windows own the screen.
3. calls `intuition_init()`, enables the gate markers when `wb.drag=` or
   `wb.menu=1` asked for them, and opens the two demo windows:

   | id | rectangle | title | body |
   | --- | --- | --- | --- |
   | 1 | (80, 90) 360x220 | `Workbench` | `CrugOS Workbench` / `phase 6b` / `software rendered` |
   | 2 | (480, 300) 320x200 | `Shell` | `4.OS` / `crugos>` |

   Window 1 carries the `Workbench` (About/Info/Quit) and `Window`
   (Close/New) menus plus a 96x22 `Run` gadget at body (12, 60); window 2
   carries `Shell` (New/Close) and `Workbench` and no gadget. Both tables go
   in through phase 6B's `intuition_set_menus()`.

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
and the menu, gadget and close events counted by `wb_drain_events()` reach
`wb.acts=` (only with `wb.menu=1`, defaulting to 1). `wb_drain_events()` pops
the device queue and prints one line per action, so the gate counts exactly
what the desktop delivered. A timeout prints
`CRUGOS: wb pointer timeout (drag N/M)` when the drags are missing and
`CRUGOS: wb action timeout (K/N)` when the actions are, and panics the boot;
success prints `CRUGOS: wb selftest ok (N drag(s), M action(s))` and then
lingers for 1.5 s so the gate's final screendump cannot race the rest of the
boot to the QEMU exit. `wb.wait=0` turns both waits off, which is what
`make demo` relies on.

## Command line

`wb_cmdline()` tokenises `bi->command_line` like the networking and audio
tokens do. Values are decimal, capped, and ignored when malformed:

| token | range | meaning |
| --- | --- | --- |
| `wb.enable=1` | 0/1 | take the framebuffer away from the text console and open the demo desktop |
| `wb.wait=S` | 0..120 | seconds the self-test waits for the drags |
| `wb.drag=N` | 0..4 | drags the self-test requires (also enables the markers) |
| `wb.menu=1` | 0/1 | also wait for `wb.acts=` menu, gadget and close actions |
| `wb.acts=N` | 0..8 | actions the self-test requires (default 1 with `wb.menu=1`) |
| `wb.hold=S` | 0..3600 | seconds `wb_hold()` keeps the desktop on screen after the boot marker |

`make test-workbench` boots with `wb.enable=1 wb.wait=30 wb.drag=1`, and
`make test-workbench-menu` boots with
`wb.enable=1 wb.wait=30 wb.menu=1 wb.acts=4`. `make demo` boots
`wb.enable=1 wb.wait=0 wb.hold=<seconds>` instead: `wb.wait=0` turns both
self-tests into no-ops, so the interactive desktop needs neither a drag nor an
action before the boot marker, and `wb.hold=` keeps the guest alive
afterwards.

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
CRUGOS: wb selftest ok (1 drag(s), 0 action(s))
CRUGOS: phase 6b boot ok
```

The menu gate is a second boot and adds the action lines:

```
CRUGOS: wb menu 'Shell' open (window 2)
CRUGOS: wb menu 'Shell' item 0 highlighted ('New')
CRUGOS: wb menu 'Shell' selected ('New')
CRUGOS: wb action 1 menu window 2 item 0
CRUGOS: wb gadget 'Run' clicked in window 1
CRUGOS: wb action 2 gadget window 1 index 0
CRUGOS: wb window 2 closed (gadget)
CRUGOS: wb action 3 close window 2
CRUGOS: wb menu 'Workbench' open (window 1)
CRUGOS: wb menu 'Workbench' selected ('Quit')
CRUGOS: wb action 4 menu window 1 item 2
CRUGOS: wb selftest ok (0 drag(s), 4 action(s))
CRUGOS: phase 6b boot ok
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
| `CRUGOS: wb menu '<title>' open (window N)` | a strip was opened from the bar |
| `CRUGOS: wb menu '<title>' item N highlighted ('<item>')` | the hovered popup row changed |
| `CRUGOS: wb menu '<title>' selected ('<item>')` | a popup row was chosen |
| `CRUGOS: wb gadget '<label>' clicked in window N` | a gadget click completed |
| `CRUGOS: wb window N closed (gadget)` | the close gadget closed the window |
| `CRUGOS: wb action K menu\|gadget\|close ...` | one event was drained from the queue |
| `CRUGOS: wb cursor at (x, y)` | pointer motion with reporting on |
| `CRUGOS: wb pointer timeout (drag N/M)` | the self-test wait expired with drags missing |
| `CRUGOS: wb action timeout (K/N)` | the self-test wait expired with actions missing |
| `CRUGOS: wb selftest ok (N drag(s), M action(s))` | the requested drags and actions happened |
| `CRUGOS: wb holding desktop for S s` | `wb.hold=` was given and `wb_hold()` started |
| `CRUGOS: wb hold finished` | the hold deadline expired, so the guest exits |

## Host tests

`make test-host` links the real `kernel/gfx/intuition.c` and
`kernel/gfx/workbench.c` and reports 2639 checks. `tests/host/test_intuition.c`
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
- menus: opening a strip, the press-between-strips and press-on-inactive-window
  cases, row hover and clearing it, selection queueing `MENU` with the window,
  menu and item indices, the two-menu and six-item clamps, and an untitled
  table entry ending the table instead of being drawn.
- gadgets: the body-relative rectangle, a gadget cut down to the body, the
  four-gadget limit, a click whose release lands outside (cancelled), the
  gadget index in the queued event, and a body click that queues nothing.
- the close gadget: closing a window, the queued `CLOSE` event, activation
  falling to the next window, the last window leaving `intuition_active()` at
  -1, and a press just outside the box starting a drag.
- the event queue: a 16-event FIFO, the drop counter on overflow, the action
  count still growing past the queue depth, and the FIFO drain order.

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
(the phase 6b boot marker, the desktop marker, a `wb selftest ok`, at least one
`wb window N opened`, one `wb window N moved to`, and one `wb cursor at` with
numeric coordinates) and `scripts/check-disk.py` for the ext4 root.

### Menu and gadget gate

`make test-workbench-menu` runs `scripts/run-workbench-menu.py`, the phase 6B
counterpart of the drag gate: the same machine (`usb=off,i8042=off`, an `xhci`
controller, a hotplugged `usb-mouse`) and the same QMP socket, but the command
line carries `wb.enable=1 wb.wait=30 wb.menu=1 wb.acts=4` and the script clicks
through the desktop:

1. moves the pointer to window 2's `Shell` strip at (30, 9) and clicks, so the
   log must show `wb menu 'Shell' open (window 2)`.
2. moves to (79, 29) -- the centre of popup row 0 -- and takes a `screendump`,
   requiring `(130, 29)` to be the amber highlight `(255,176,0)` and
   `(130, 47)` (row 1) to still be the grey `(192,192,192)`.
3. clicks that row, so the log must show
   `wb menu 'Shell' selected ('New')` and `wb action 1 menu window 2 item 0`.
4. clicks the `Run` gadget at (142, 183) (`wb gadget 'Run' clicked`,
   `wb action 2 gadget window 1 index 0`).
5. clicks window 2's close gadget at (786, 312) (`wb window 2 closed (gadget)`,
   `wb action 3 close window 2`) and checks two pixels of a second `screendump`:
   the desktop background `(16,32,48)` at (640, 400) where the window was, and
   white frame at (80, 90) for window 1.
6. opens window 1's `Workbench` strip at (30, 9), clicks row 2 (`Quit`) at
   (79, 65) and requires `wb action 4 menu window 1 item 2` followed by
   `wb selftest ok` and exit status 33.

Every pointer move is issued in steps of at most 100 pixels and the script
follows the position the guest reports in `CRUGOS: wb cursor at`: QEMU splits a
larger relative motion into several 8-bit HID reports and the guest drops some
of them, so a single jump of 644 pixels landed 136 pixels short when the gate
was first written.

`boot-check.py` then runs with `EXPECT_MENU=yes`, which requires the phase 6b
marker, the desktop marker, the menu open and selected markers, the gadget
click marker, the close marker, at least one `wb action N ...` line and a
`wb selftest ok (N drag(s), M action(s))` with at least one action, and
`scripts/check-disk.py` proves the ext4 root is still clean.

## Interactive demo

`make demo` boots the same desktop in a real QEMU window instead of a headless
gate: `DEMO_QEMU_EXTRA` plugs a keyboard and a mouse into the xHCI controller
before boot, `DEMO_DISPLAY` (GTK) opens the window, and the command line carries
`wb.enable=1 wb.wait=0 wb.hold=$(DEMO_HOLD_SEC)`. The desktop appears once
`CRUGOS: wb desktop ready` is printed, `wb_hold()` then polls input once per
timer tick until the deadline, so the two demo windows stay on screen and the
pointer and the keyboard are live (pointer motion prints
`CRUGOS: usb mouse move`, a title-bar press starts a drag). After the hold the
guest prints `CRUGOS: wb hold finished`, exits with status 33 and leaves
`build/x86_64/serial.log` behind. See [build.md](build.md).

## Limits

- Disabled by default: the desktop is enabled by `wb.enable=1` (the gates and
  `make demo` pass it, a normal boot does not) and the demo windows are
  hardcoded. There are no application tasks, no Workbench userspace and no
  window content owned by another program yet.
- Four windows, two menus of six items and four gadgets per window, one open
  popup at a time. No window resize, no sub-menus, no keyboard navigation of
  the menu bar, no icons, no clipboard and no double-click semantics; a menu
  selection or gadget click queues an event, but no program is attached to it
  yet, so the shell will have to consume that queue.
- A popup always opens left-aligned on its strip, so a strip near the right
  edge can run its popup off screen, where the clipping primitives cut it. Menu
  and gadget tables are copied, but their strings stay caller-owned and must
  stay valid while the window is open.
- A full desktop repaint per state change, and a polled pointer path: the
  latency is the 10 ms `usb.poll` tick until interrupts or a compositor make
  it cheaper.
- Text is 8x16 with a 20-pixel line pitch and a 47-character line cap; there
  is no word wrapping, no scrollback inside a window and no fonts.
- The keyboard still reaches the serial console, not a Workbench window, so
  there is no keyboard accelerator for the menu bar.
- No clipping stack: `fb_fill_rect()` and `fb_draw_text()` clip to the screen,
  so a window cannot draw outside itself into another window's area.
