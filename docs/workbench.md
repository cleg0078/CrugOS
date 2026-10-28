# Workbench: intuition.device, menus, gadgets and the shell

Phase 6A starts the Workbench: a small software window manager in the kernel,
`intuition.device`, that owns up to four windows on the GOP framebuffer.
Windows are raised and activated by a left click, dragged by their title bar,
and repainted straight into the linear framebuffer with the primitives in
`fb.h`. Phase 6B adds the interaction model on top: a screen-top menu bar with
popup menus, labelled gadgets inside a window body, a close gadget at the right
end of every title bar, and a typed event queue that reports what the user
did. Phase 6C fills the second window with a live shell -- a scrollback text
grid, a line editor with history and a command set over the mounted ext4 root.
Phase 6D makes every window resizable from a corner grip and gives the
keyboard a navigation role: Tab cycles the focus, the arrow keys move the
active window and Enter opens its menu for the arrows to drive. Phase 6E adds
desktop objects, which sit under the windows and select or open a drawer, and
the depth gadget, which sends a window to the back of the stacking order. Phase
6F makes the objects themselves interactive: a press selects and grabs a tile,
a motion drags it across the desktop, a send-to-front gadget joined the depth
gadget in every title bar, and the icon table can be driven from the keyboard.
Phase 6G turns the `System:` drawer into a scrolling list: the arrow keys move
the viewport, `r` re-reads the volume, a 4-pixel scrollbar tracks the position
and a resize reflows the rows. There are no application tasks behind the
desktop yet -- the kernel-side device is what the Workbench userspace will sit
on -- so the only residents are the
bring-up module and its shell, which appear when the loader command line asks
for them.

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
| `INTUITION_RESIZE_SIZE` | 12 | resize grip side in pixels |
| `INTUITION_KEY_STEP` | 8 | pixels an arrow key moves the active window |
| `INTUITION_MIN_WIDTH` | 120 | smallest window a caller can open |
| `INTUITION_MIN_HEIGHT` | 80 | smallest window a caller can open |
| `INTUITION_DEPTH_SIZE` | 12 | depth gadget side in pixels |
| `INTUITION_FRONT_SIZE` | 12 | send-to-front gadget side in pixels |
| `INTUITION_ICON_DRAG_MIN` | 4 | dead zone before a tile drag starts moving it |
| `INTUITION_MAX_ICONS` | 8 | desktop object slots |
| `INTUITION_ICON_WIDTH` | 64 | icon tile width in pixels |
| `INTUITION_ICON_HEIGHT` | 48 | icon tile height in pixels |
| `INTUITION_ICON_LABEL_GAP` | 4 | pixels between a tile and its label |
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
| `INTUITION_ICON_RGB` | `0x00c0c0c0` | icon tile fill |
| `INTUITION_ICON_INNER_RGB` | `0x00081018` | icon inner panel |
| `INTUITION_ICON_TEXT_RGB` | `0x00e0e0e0` | icon label |
| `INTUITION_ICON_SELECTED_BORDER_RGB` | `0x00ffb000` | frame of the selected icon |

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
| `u64 intuition_resize_count(void)` | completed resize-grip gestures |
| `void intuition_key_event(u8 key)` | consume one decoded key byte |
| `u32 intuition_window_count(void)` | open windows |
| `int intuition_add_icon(i32 x, i32 y, const char *label, u32 kind)` | add a desktop object; returns its index or -1 |
| `int intuition_icon_rect(int index, i32 *x, i32 *y, u32 *w, u32 *h)` | read an icon tile rectangle; 0 or -1 |
| `int intuition_icon_selected(void)` | selected desktop object, or -1 |
| `u32 intuition_icon_count(void)` | desktop objects added |
| `u64 intuition_icon_clicks(void)` | presses on a desktop object |
| `u64 intuition_depth_count(void)` | windows sent to the back |
| `u64 intuition_icon_moves(void)` | completed icon drags |
| `u64 intuition_front_count(void)` | windows raised by the send-to-front gadget |
| `int intuition_icon_key(u8 key)` | drive the icon table from the keyboard; 1 when Enter asks for activation |
| `int intuition_set_paint(int id, void (*fn)(int id, i32 x, i32 y, u32 w, u32 h, void *ctx), void *ctx)` | install one body painter; a NULL callback restores the static body text |
| `void intuition_redraw_window(int id)` | repaint one window after its content changed |

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
title bar (in phase 6B it closes the window), a 12x12 depth gadget four pixels
left of it (phase 6E), a 12x12 send-to-front gadget twelve pixels further left
(phase 6F), a 12x12 resize grip in the inner
bottom-right corner (phase 6D), and a body of 8x16 text on a 20-pixel line
pitch. Body text lines are split at `\n` and truncated to 47 characters. Since
phase 6F one window draw is five framebuffer frames -- the window, the close
box, the depth box, the front box and the resize grip -- which is why the host
tests count five `fb_frame_rect()` calls per repaint.

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
  box closes that window, a press on the depth box sends it to the back, a press
  on the front box raises and activates it, a press on the resize grip starts a
  size drag, a press on a body gadget arms a click,
  and only then
  does `win_hit()` walk the stacking order top down for the first window that
  contains the point. On a window hit the window is raised, made active, and --
  when the press is on the title bar -- a drag starts with the grab offset
  `(px - w->x, py - w->y)`. A press that hits no window selects a desktop object
  or, on the empty desktop, clears the selection.
  The release (`changes & 0x01` without the left button) completes a pending
  gadget click, so a press that leaves its gadget and releases elsewhere does
  not count.
- `changes & 0x01` without the left button (release): if a drag is running the
  window is already where the last motion put it, so the release prints
  `moved to (x, y)` and counts one drag; if a resize is running the release
  prints `resized to WxH`, queues `RESIZE` and counts one action.
- motion while a drag is running: the new origin is the cursor minus the grab
  offset, clamped into the screen; the desktop is repainted.
- motion while a resize is running: the new size is the size at press plus the
  pointer delta, floored at 120x80 and refused when it would exceed the screen;
  the desktop is repainted when the size actually changed.
- motion while an icon drag is running: the tile origin is the cursor minus the
  grab offset after a 4-pixel dead zone, clamped so all 64x48 pixels stay on
  the desktop; the desktop is
  repainted when the origin changed. The release prints `dropped at (x, y)`,
  queues `ICON_MOVE` with the object index and the new origin, counts one icon
  move and one action, and leaves the tile selected.
- anything else (motion without a drag, button changes that are not the left
  button) leaves the window state alone.

The full desktop is repainted on every state change. At 1024x768 that is a few
hundred thousand pixels, which is cheap enough for a polled bring-up phase and
keeps the renderer simple: no dirty rectangles, no clipping stacks, no
save-under (the framebuffer primitives restore and recomposite the pointer
sprite themselves).

## Desktop objects

Phase 6E adds desktop objects: a tile the size of `INTUITION_ICON_WIDTH` x
`INTUITION_ICON_HEIGHT` (64x48) with one label line under it, drawn on the
desktop background. Because the repaint walks the stacking order bottom to top,
the icons are painted first and every window covers them, so a press inside a
window never reaches a tile.

| property | value |
| --- | --- |
| table | `INTUITION_MAX_ICONS` (8) objects |
| tile | `INTUITION_ICON_WIDTH` x `INTUITION_ICON_HEIGHT` (64x48) |
| label gap | `INTUITION_ICON_LABEL_GAP` (4) pixels under the tile |
| tile fill | `INTUITION_ICON_RGB` (`0x00c0c0c0`) |
| inner panel | `INTUITION_ICON_INNER_RGB` (`0x00081018`) |
| label | `INTUITION_ICON_TEXT_RGB` (`0x00e0e0e0`) |
| selected frame | `INTUITION_ICON_SELECTED_BORDER_RGB` (`0x00ffb000`) |
| kinds | `INTUITION_ICON_VOLUME` (0), `INTUITION_ICON_DRAWER` (1), `INTUITION_ICON_FILE` (2) |

`intuition_add_icon(x, y, label, kind)` stores one object and returns its
index, or -1 when the table is full, the label is missing or no framebuffer is
ready. The tile is clamped like a window, so all 64x48 pixels and the label stay
on screen; `intuition_icon_rect()` reads a stored rectangle back.

A left press inside a tile selects that object, so there is one selected icon
at a time: its frame and its label turn
`INTUITION_ICON_SELECTED_BORDER_RGB`, `intuition_icon_clicks()` and the
action count grow by one, `INTUITION_EVENT_ICON` is queued with the object
index and the tile origin, and a reporting boot prints
`CRUGOS: wb icon '<label>' selected`. A press on the empty desktop clears the
selection instead. `intuition_icon_selected()` returns the selected index or
-1, and `intuition_icon_count()` the objects added.

The device reports presses only, so a double click is a caller concept: the
Workbench treats two presses on the same object inside `WB_DOUBLE_CLICK_NS`
(600000000 ns) as a drawer open. With `wb.desk=1` it adds one volume object
labelled `System:` at (16,240), prints
`CRUGOS: wb icon ready ('System:', volume)` after the desktop, shell and keys
markers, and opens a normal Intuition window titled `System:` at (560,60),
360x260 whose painter lists the root directory entries of the mounted ext4
root, one name per line, clipped to the body width, printing
`CRUGOS: wb drawer 'System:' opened (N entries)`. An empty or unmounted root
prints `no volume` instead. Since phase 6G the body is a scrolling viewport
over that listing; see [The drawer window](#the-drawer-window).

### Dragging an icon

Phase 6F makes a tile grabbable. A left press on a tile still selects it, and it
also starts an icon drag: the grab offset is `(px - tile x, py - tile y)` and the
tile follows the cursor from there. `INTUITION_ICON_DRAG_MIN` (4 pixels) is a
real dead zone: pointer motion that leaves the cursor less than 4 pixels away on
both axes is ignored, and the drag becomes live once either axis reaches 4. The
new origin is the cursor minus the
grab offset, clamped to `[0, screen width - 64]` x `[0, screen height - 48]`, so
no pixel and no label leaves the desktop. A reporting boot prints
`CRUGOS: wb icon '<label>' drag to (x, y)` for every origin change.

The release queues `INTUITION_EVENT_ICON_MOVE` (type 9) with the object index in
`index` and the new origin in `x`/`y`, bumps `intuition_icon_moves()` and the
action count, prints `CRUGOS: wb icon '<label>' dropped at (x, y)`, and keeps
the tile selected, so its amber frame travels with it. A press that never
moves queues no move event and counts no move.

### Window body painters

`intuition_set_paint(id, fn, ctx)` installs one painter per window. Once
installed the callback receives the body rectangle in screen coordinates
instead of the static body text, and the gadgets are still drawn after it, so a
painter never has to know about them. `intuition_redraw_window(id)` repaints
that one window after its content changed, and a NULL callback restores the
static body text. There is a single painter slot, so a second call replaces the
first. The shell window uses this from phase 6C and the drawer from phase 6E.

## The drawer window

The `System:` drawer opened by double-clicking the volume icon is a scrolling
viewport over the root directory listing since phase 6G. The body is the
window height less the 20-pixel title bar and the two 2-pixel borders
(`h - 24`), and the painter walks it as 20-pixel rows of 8x16 glyphs, so the
number of visible rows is

    rows = body > 22 ? (body - 22) / 20 + 1 : 0

A 360x260 drawer has a 236-pixel body and shows 11 rows; resized to 460x360
the 336-pixel body shows 16. `g_drawer_top` is the index of the topmost visible
entry; entry `top + i` is painted at `y + 6 + i*20`, clipped to
`(w - 20) / 8` characters (at most 47) starting at `x + 6`.

| item | value |
|---|---|
| entries held | `WB_DRAWER_MAX` (40) |
| name bytes held | `WB_DRAWER_NAME_MAX` (32) |
| row pitch | `WB_DRAWER_ROW_H` (20) |
| scrollbar track | 4 px wide at `x + w - 8`, from `y + 6`, height `h - 12`, colour `0x00404040` |
| scrollbar thumb | height `max(12, track_h * rows / entries)`, offset `(track_h - thumb_h) * top / max_top`, colour `0x00c0c0c0` |
| scrollbar painted | only while `entries > rows` |

`wb_drawer_scroll(delta)` clamps the top row to `[0, max_top]` with
`max_top = entries > rows ? entries - rows : 0`, repaints the window and prints
`CRUGOS: wb drawer scrolled (top N of M)` while counting `g_drawer_scrolls` and
`g_drawer_ops_seen`. A press at either end returns 0 and counts nothing, so the
list never scrolls past its ends. `wb_drawer_refresh()` re-reads the listing,
clamps the top row against the new entry count, repaints and prints
`CRUGOS: wb drawer refreshed (N entries)`. Only one drawer window exists at a
time: a second open is ignored.

With `wb.dr=1` (and `wb.desk=1`) the shared key pump feeds the drawer: Up and
Down move the viewport by one entry and `r`/`R` refresh the listing. The pump
is not installed when `wb.shell=1`, and while the drawer is closed the same
keys drive the icon table. `wb.dr=1` also prints
`CRUGOS: wb drawer mode (arrow keys scroll, r refreshes)` and makes the
Workbench self-test wait for `wb.acts=` drawer operations, printing
`CRUGOS: wb drawer selftest ok (N scroll(s), M refresh(es))` or
`CRUGOS: wb drawer timeout (N/M)`.

`wb.fill=N` (0..24) creates `/f00` .. `/f(N-1)` in the root once, before the
drawer reads the listing, and prints `CRUGOS: wb fill N file(s)`. The kernel
mounts the ext4 root read-only at boot, so the fill brackets the writes with
`storage_remount_rw()` / `storage_remount_ro()` from `kernel/fs/storage.c`
(declared in `include/crug/storage.h`), which umount and remount the volume and
print `CRUGOS: storage remounted read-write` / `CRUGOS: storage remounted
read-only`. If the remount is refused the fill is skipped with
`CRUGOS: wb fill skipped (read-only volume)`. The shell's `write` command uses
the same helpers: before phase 6G it wrote into the read-only boot mount and
failed silently; it now remounts read-write around the create and write and
remounts read-only again, and `sync` returns 0 only while the volume is
mounted read-write.

The gate that proves all of this is `make test-workbench-drawer`, gate 42;
its six steps are described under [Drawer gate](#drawer-gate).

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

## The depth gadget

Phase 6E adds a 12x12 `INTUITION_DEPTH_SIZE` box four pixels left of the close
box in every title bar, so the bar now reads: drag area, depth box, close box.
A press on it sends that window to the bottom of the stacking order. The new
top window becomes active and its title bar turns amber,
`INTUITION_EVENT_DEPTH` is queued for the window that went back followed by
`INTUITION_EVENT_ACTIVATE` for the new top window, `intuition_depth_count()`
and the action count grow by one, and a reporting boot prints
`CRUGOS: wb window N sent to back`.

This differs from a title-bar press, which raises the window, and from the close
box, which removes it: the window stays open and keeps its size and content,
only its place in the stack changes. Pressing the depth box of the window that
is already at the bottom is a no-op -- no event, no count and no repaint --
because the stacking order would not change.

## The send-to-front gadget

Phase 6F completes the pair: a 12x12 `INTUITION_FRONT_SIZE` box twelve pixels
left of the depth box, at `front_box_x = close_box_x - 32` while the depth box
sits at `close_box_x - 16`, so the title bar now reads: drag area, front box,
depth box, close box. The gadget draws a white up arrow on the gadget fill.

A press raises that window to the top of the stacking order, makes it active
and turns its title bar amber -- the effect of a title-bar press, but without
starting a drag. `INTUITION_EVENT_FRONT` (type 10) is queued,
`intuition_front_count()` and the action count grow by one, and a reporting
boot prints `CRUGOS: wb window N sent to front`. Pressing the gadget of the
window that is already frontmost is a no-op -- no event, no count and no
repaint -- because the stacking order would not change. Compare the depth
gadget, which sends a window back and activates the new top window.

## The event queue

`struct intuition_event` is the typed report the desktop hands to its caller:

| field | meaning |
| --- | --- |
| `type` | `INTUITION_EVENT_ACTIVATE`, `_DRAG`, `_MENU`, `_GADGET`, `_CLOSE`, `_RESIZE`, `_ICON`, `_DEPTH`, `_ICON_MOVE` or `_FRONT` (0 = none) |
| `window` | window id the event belongs to, 0 when it has none |
| `index` | menu index, gadget index or desktop object index, -1 when unused |
| `item` | selected menu item index, 0 when unused |
| `x`, `y` | window origin, or icon tile origin on `_ICON` and `_ICON_MOVE`, when the event was generated |
| `w`, `h` | new window size, set on `_RESIZE` and 0 otherwise |

Activation and completed drags are queued as well, so a caller that drains the
queue sees the whole story of a click and not only the actions the kernel
already handled. The queue holds `INTUITION_EVENT_QUEUE` (16) events; when it
is full the newest event is dropped and counted, so `intuition_event_count()`
plus `intuition_event_dropped()` accounts for every event the device produced.
`intuition_event_poll()` returns 1 and copies the oldest event, or 0 when the
queue is empty. `intuition_init()` resets the queue and both counters. A
finished resize queues `INTUITION_EVENT_RESIZE` with the new `w` and `h`; the
other seven types leave both fields at 0. A depth press queues
`INTUITION_EVENT_DEPTH` for the window that went back and then an
`INTUITION_EVENT_ACTIVATE` for the new top window; an icon press queues `_ICON`
with the object index and its tile origin. A front press queues `_FRONT` for the
window it raised, and a finished icon drag queues `_ICON_MOVE` with the object
index and the new tile origin.

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
   | 1 | (80, 90) 360x220 | `Workbench` | `CrugOS Workbench` / `phase 6g` / `software rendered` |
   | 2 | (480, 300) 320x200 | `Shell` | `4.OS` / `crugos>`, or the shell grid when `wb.shell=1` |

   Window 1 carries the `Workbench` (About/Info/Quit) and `Window`
   (Close/New) menus plus a 96x22 `Run` gadget at body (12, 60); window 2
   carries `Shell` (New/Close) and `Workbench` and no gadget. Both tables go
   in through phase 6B's `intuition_set_menus()`. With `wb.shell=1` window 2
   also gets a body painter through `intuition_set_paint()`, and that painter
   draws the shell's text grid instead of the static body lines.

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
| `wb.acts=N` | 0..8 | actions the self-test requires (also with `wb.keys=1`) |
| `wb.keys=1` | 0/1 | hand decoded key bytes to `intuition_key_event()` (ignored when `wb.shell=1`) |
| `wb.desk=1` | 0/1 | create the `System:` desktop object and count desktop actions (reporting mode) |
| `wb.ikeys=1` | 0/1 | give the icon table the keyboard: Tab/arrows move the selection, Enter selects then activates, Escape clears it (parsed before `wb.desk=`, ignored when `wb.shell=1`) |
| `wb.dr=1` | 0/1 | give the `System:` drawer the arrow keys and `r`, and make the self-test wait for `wb.acts=` drawer operations (ignored when `wb.shell=1`) |
| `wb.fill=N` | 0..24 | create `/f00` .. `/f(N-1)` in the root once before the drawer opens |
| `wb.hold=S` | 0..3600 | seconds `wb_hold()` keeps the desktop on screen after the boot marker |

`make test-workbench` boots with `wb.enable=1 wb.wait=30 wb.drag=1`,
`make test-workbench-menu` with `wb.enable=1 wb.wait=30 wb.menu=1 wb.acts=4`,
`make test-workbench-keys` with `wb.enable=1 wb.wait=30 wb.keys=1
wb.acts=2`, and `make test-workbench-desk` with `wb.enable=1 wb.wait=30
wb.desk=1 wb.acts=3`, and `make test-workbench-icons` with `wb.enable=1
wb.wait=30 wb.desk=1 wb.ikeys=1 wb.acts=5`, and `make test-workbench-drawer`
with `wb.enable=1 wb.wait=30 wb.desk=1 wb.dr=1 wb.fill=24 wb.acts=4`. `make demo` boots
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
CRUGOS: phase 6g boot ok
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
CRUGOS: phase 6g boot ok
```

The shell gate is a third boot: it adds `CRUGOS: wb shell ready (window 2,
37x8 text)` after the desktop line and one `CRUGOS: sh run '<line>'` plus the
`CRUGOS: sh out <text>` lines of that command per command, followed by
`CRUGOS: wb selftest ok (0 drag(s), 0 action(s))` and
`CRUGOS: sh selftest ok (4 command(s))` before the phase marker.

The keys gate is a fourth boot: it resizes window 2 by its grip and then drives
the desktop from the keyboard, so the log carries the resize, focus, key-move
and menu-highlight lines:

```
CRUGOS: wb desktop ready (1024x768, 2 window(s))
CRUGOS: wb keys ready (window 2 active)
CRUGOS: wb window 2 resize start (790, 490)
CRUGOS: wb window 2 resized to 260x140
CRUGOS: wb action 1 resize window 2 (260x140)
CRUGOS: wb window 1 focused (Tab)
CRUGOS: wb window 1 key move to (88, 90)
CRUGOS: wb window 1 key move to (96, 90)
CRUGOS: wb menu 'Workbench' item 0 highlighted ('About')
CRUGOS: wb menu 'Workbench' item 1 highlighted ('Info')
CRUGOS: wb menu 'Workbench' selected ('Info')
CRUGOS: wb action 2 menu window 1 item 1
CRUGOS: wb selftest ok (0 drag(s), 2 action(s))
CRUGOS: phase 6g boot ok
```

The desktop gate is a fifth boot: it selects the `System:` object, opens its
drawer and sends the drawer to the back of the stacking order:

```
CRUGOS: wb desktop ready (1024x768, 2 window(s))
CRUGOS: wb icon ready ('System:', volume)
CRUGOS: wb icon 'System:' selected
CRUGOS: wb icon 'System:' selected
CRUGOS: wb drawer 'System:' opened (5 entries)
CRUGOS: wb action 1 icon 0
CRUGOS: wb action 2 icon 0
CRUGOS: wb window 3 sent to back
CRUGOS: wb action 3 depth window 3
CRUGOS: wb desk selftest ok (1 drawer(s))
CRUGOS: wb selftest ok (0 drag(s), 3 action(s))
CRUGOS: phase 6g boot ok
```

The icon gate is a sixth boot: it focuses the `System:` object from the
keyboard, activates it, drags it and drives both stacking gadgets:

```
CRUGOS: wb desktop ready (1024x768, 2 window(s))
CRUGOS: wb icon ready ('System:', volume)
CRUGOS: wb icons ready (1 object(s), keyboard focus)
CRUGOS: wb icon 'System:' focused
CRUGOS: wb action 1 icon 0 (keyboard)
CRUGOS: wb icon activated (keyboard)
CRUGOS: wb drawer 'System:' opened (5 entries)
CRUGOS: wb icon 'System:' drag to (168, 376)
CRUGOS: wb icon 'System:' dropped at (168, 376)
CRUGOS: wb action 2 icon 0
CRUGOS: wb action 3 icon-move 0
CRUGOS: wb window 1 sent to back
CRUGOS: wb action 4 depth window 1
CRUGOS: wb window 1 sent to front
CRUGOS: wb action 5 front window 1
CRUGOS: wb icons selftest ok (1 move(s), 1 front(s))
CRUGOS: wb selftest ok (0 drag(s), 5 action(s))
CRUGOS: phase 6g boot ok
```

The drawer gate is a seventh boot, the only one that fills the volume first:
it opens the drawer, scrolls it, resizes it by the grip so the rows reflow,
refreshes it with `r` and checks the framebuffer between steps:

```
CRUGOS: wb desktop ready (1024x768, 2 window(s))
CRUGOS: wb icon ready ('System:', volume)
CRUGOS: wb drawer mode (arrow keys scroll, r refreshes)
CRUGOS: wb icon 'System:' selected
CRUGOS: wb icon 'System:' selected
CRUGOS: storage remounted read-write
CRUGOS: wb fill 24 file(s)
CRUGOS: storage remounted read-only
CRUGOS: wb drawer 'System:' opened (29 entries)
CRUGOS: wb drawer ready (29 entries, 11 row(s))
CRUGOS: wb drawer scrolled (top 1 of 29)
CRUGOS: wb drawer scrolled (top 2 of 29)
CRUGOS: wb drawer scrolled (top 3 of 29)
CRUGOS: wb window 3 resized to 460x360
CRUGOS: wb drawer refreshed (29 entries)
CRUGOS: wb selftest ok (0 drag(s), 0 action(s))
CRUGOS: wb desk selftest ok (1 drawer(s))
CRUGOS: wb drawer selftest ok (3 scroll(s), 1 refresh(es))
CRUGOS: phase 6g boot ok
```

| marker | when |
| --- | --- |
| `CRUGOS: wb desktop ready (WxH, N window(s))` | the desktop took the screen |
| `CRUGOS: wb keys ready (window N active)` | `wb.keys=1` installed keys mode |
| `CRUGOS: wb icon ready ('<label>', kind)` | `wb.desk=1` created the desktop object |
| `CRUGOS: wb icon '<label>' selected` | a press selected a desktop object |
| `CRUGOS: wb drawer '<label>' opened (N entries)` | a double click opened the drawer |
| `CRUGOS: wb drawer '<label>' no volume` | the drawer found no mounted root |
| `CRUGOS: wb drawer mode (arrow keys scroll, r refreshes)` | `wb.dr=1` gave the drawer the keys |
| `CRUGOS: wb drawer ready (N entries, R row(s))` | drawer mode opened the drawer and wrapped its listing |
| `CRUGOS: wb fill N file(s)` | `wb.fill=N` created its files before the drawer read the root |
| `CRUGOS: wb fill skipped (read-only volume)` | the root could not be remounted read-write |
| `CRUGOS: storage remounted read-write` | a short write window opened |
| `CRUGOS: storage remounted read-only` | the write window closed and committed |
| `CRUGOS: wb drawer scrolled (top N of M)` | Up or Down moved the viewport |
| `CRUGOS: wb drawer refreshed (N entries)` | `r`/`R` reloaded the listing |
| `CRUGOS: wb drawer selftest ok (N scroll(s), M refresh(es))` | drawer mode saw its scroll and refresh counts |
| `CRUGOS: wb drawer timeout (N/M)` | the wait expired with drawer operations missing |
| `CRUGOS: wb window N sent to back` | the depth gadget was pressed |
| `CRUGOS: wb icons ready (N object(s), keyboard focus)` | `wb.ikeys=1` installed icon focus mode |
| `CRUGOS: wb icon '<label>' focused` | Tab, an arrow or Enter moved the icon selection |
| `CRUGOS: wb icon '<label>' drag to (x, y)` | a motion moved the grabbed tile |
| `CRUGOS: wb icon '<label>' dropped at (x, y)` | the release ended an icon drag |
| `CRUGOS: wb window N sent to front` | the send-to-front gadget was pressed |
| `CRUGOS: wb action N icon I (keyboard)` | an activation from the icon table counted one action |
| `CRUGOS: wb icon activated (keyboard)` | Enter in the icon table ran the selected object |
| `CRUGOS: wb icons selftest ok (N move(s), M front(s))` | icon focus mode saw its drags and front presses |
| `CRUGOS: wb desk selftest ok (N drawer(s))` | the desktop self-test saw its drawers |
| `CRUGOS: wb framebuffer absent` | `wb.enable=1` without a usable framebuffer |
| `CRUGOS: wb window N opened (x, y, WxH)` | a window was opened (reporting on) |
| `CRUGOS: wb window N active` | a left press hit window N |
| `CRUGOS: wb window N focused (Tab)` | Tab raised and activated window N |
| `CRUGOS: wb window N key move to (x, y)` | an arrow key moved the active window |
| `CRUGOS: wb window N drag start (x, y)` | the press landed on the title bar |
| `CRUGOS: wb window N drag to (x, y)` | a motion moved the dragged window |
| `CRUGOS: wb window N moved to (x, y)` | the button was released after a drag |
| `CRUGOS: wb window N resize start (x, y)` | the press landed on the resize grip |
| `CRUGOS: wb window N resized to WxH` | the resize grip was released |
| `CRUGOS: wb menu '<title>' open (window N)` | a strip was opened from the bar |
| `CRUGOS: wb menu '<title>' item N highlighted ('<item>')` | the hovered popup row changed |
| `CRUGOS: wb menu '<title>' selected ('<item>')` | a popup row was chosen |
| `CRUGOS: wb menu closed (escape)` | Escape closed a keyboard-opened popup |
| `CRUGOS: wb gadget '<label>' clicked in window N` | a gadget click completed |
| `CRUGOS: wb window N closed (gadget)` | the close gadget closed the window |
| `CRUGOS: wb action K menu\|gadget\|close\|resize\|icon\|icon-move\|depth\|front ...` | one event was drained from the queue |
| `CRUGOS: wb shell ready (window N, CxR text)` | `wb.shell=1` installed the shell painter |
| `CRUGOS: sh run '<line>'` | a shell line was executed |
| `CRUGOS: sh out <text>` | one output line of that command |
| `CRUGOS: sh command timeout (N/M)` | the wait expired with commands missing |
| `CRUGOS: sh selftest ok (N command(s))` | the requested commands ran |
| `CRUGOS: wb cursor at (x, y)` | pointer motion with reporting on |
| `CRUGOS: wb pointer timeout (drag N/M)` | the self-test wait expired with drags missing |
| `CRUGOS: wb action timeout (K/N)` | the self-test wait expired with actions missing |
| `CRUGOS: wb selftest ok (N drag(s), M action(s))` | the requested drags and actions happened |
| `CRUGOS: wb holding desktop for S s` | `wb.hold=` was given and `wb_hold()` started |
| `CRUGOS: wb hold finished` | the hold deadline expired, so the guest exits |

## The shell window

Phase 6C puts a working shell in window 2 so the desktop has something to type
at. Three pieces carry it:

| file | role |
| --- | --- |
| `include/crug/textgrid.h`, `kernel/lib/textgrid.c` | the scrollback buffer and its drawing |
| `include/crug/shell.h`, `kernel/shell/shell.c` | the line editor, the history and the command table |
| `kernel/shell/shell_ops.c` | the kernel `struct shell_io` over the mounted ext4 root |

The grid keeps `TG_LINES` (96) lines of `TG_COLS` (38) characters -- 37 usable
ones, because every line stays NUL-terminated -- and remembers `first`, `count`,
`row` and `col`. `tg_putc()` maps `\n` to a new line, `\r` to a carriage
return, `0x08`/`0x7f` to a backspace and stores any other control byte as `.`;
a character written over an existing tail keeps the rest of the tail, so
overwriting a finished line only replaces the cells that were typed. A full
grid evicts the oldest line. `tg_draw()` paints the last
`min((h - 6) / 20, 8)` lines at a 20-pixel pitch, clips every line to the body
width and draws the cell under the cursor in inverse video -- that is the whole
cursor story, there is no blinking and no separate sprite.

`struct shell` edits one 79-character line. Printable bytes are inserted at the
cursor, `\n`/`\r` executes, `0x08`/`0x7f` backspaces, `Ctrl-U` (`0x15`) erases
the line, `Ctrl-C` (`0x03`) cancels it and prints `^C`, and `Ctrl-P` (`0x10`) /
`Ctrl-N` (`0x0e`) walk the eight-entry history (the draft line comes back after
the newest entry). The shell does not use the arrow keys -- phase 6D decodes
them for the desktop, not for the line editor -- which is why the history uses
the classic Emacs control keys. `shell_feed()` returns 0 while editing,
1 when a line executed and 2 when the line was cancelled; `shell_run()` returns
0 for a known command, 1 for an unknown one and -1 for an empty line.

The command table is `help`, `ver`, `mem`, `uptime`, `ps`, `ls [path]`,
`cat <path>`, `echo <text>`, `write <path> <text>`, `sync` and `cls`; an
unknown word prints `sh: <word>: unknown command`. Every command reaches the
outside world through `struct shell_io` (`list`, `read`, `write`, `sync`,
`info`), so the host tests drive the real shell against an in-memory fake, while
`kernel/shell/shell_ops.c` supplies the kernel version: `ext4_list()`,
`ext4_read_file()`, `ext4_lookup()` + `ext4_create()` + `ext4_write_file()` and
`ext4_fsync()` on `storage_root()`, plus the heap counters, `sched_current()`
and `sched_switches()`.

`kernel/gfx/workbench.c` reads `wb.shell=1` (and `wb.sh=<n>`, the number of
commands the self-test waits for) on top of the 6A/6B tokens. It initialises the
grid, prints a one-line banner, calls `shell_init()`, installs the body painter
with `intuition_set_paint()` on window 2 -- so every repaint of that window
redraws the grid -- and prints `CRUGOS: wb shell ready (window 2, 37x8 text)`.
In shell mode `wb_selftest()` runs `wb_shell_pump()` in its wait loop: every
byte from `input_try_getc()` goes to the shell, each executed line prints
`CRUGOS: sh run '<line>'`, every output line prints `CRUGOS: sh out <text>`, and
the window is repainted after each keystroke. `wb_hold()` pumps the same loop,
so the interactive demo has a working shell as well.

## Resizing windows

Every window carries a 12x12 resize grip, `INTUITION_RESIZE_SIZE` pixels
square, in its inner bottom-right corner at
`(x + w - INTUITION_BORDER - 12 - 2, y + h - INTUITION_BORDER - 12 - 2)`. The
grip is drawn after the body, so a body painter cannot hide it, and every
window gets one.

A press inside the grip arms a size drag instead of a body press. Every motion
while it is held computes the new size from the size at press plus the pointer
delta: the width and height are floored at `INTUITION_MIN_WIDTH` /
`INTUITION_MIN_HEIGHT` (120x80), and a motion that would push the right or
bottom edge past the framebuffer is refused outright -- the size simply does
not change -- rather than clamped, so a window can never be dragged larger
than the screen that holds it. The origin never moves. The release prints
`CRUGOS: wb window %d resized to %ux%u`, increments
`intuition_resize_count()`, counts one action and queues an
`INTUITION_EVENT_RESIZE` whose `w` and `h` fields carry the final size. A
press and release on the grip with no motion in between still counts a resize,
so a click on the grip is never a silent no-op.

`struct intuition_event` gained `u32 w` and `u32 h` for this; the five
earlier types leave both fields at 0, so a caller can tell a resize from the
rest without reading a second field. The size is the window's outer size, the
same one `intuition_window_rect()` reports.

The body is repainted at the new size. The shell's text grid follows (it shows
`min((h - 6) / 20, 8)` rows), but static body text is re-laid out from the
top-left only: there is no reflow, no font scaling and no content that moves
itself inside the body.

## Keyboard navigation

`intuition_key_event(u8)` gives the desktop a keyboard of its own. The bytes
come from `input.device` like every other key: `wb_key_pump()` in
`kernel/gfx/workbench.c` drains whatever `input_try_getc()` has queued and
forwards Tab, Enter, Escape and the four `INPUT_KEY_*` arrow bytes while
`wb.keys=1` is set. If `wb.shell=1` is also given, shell mode wins: the shell
owns the keyboard, and keys mode leaves every byte to `wb_shell_pump()`.

Tab cycles the focus through the stacking order. The next window is raised,
made active, an `INTUITION_EVENT_ACTIVATE` is queued and the log carries
`CRUGOS: wb window %d focused (Tab)`. The four arrow bytes move the active
window by `INTUITION_KEY_STEP` (8) pixels and clamp it to the screen, one step
per byte: `CRUGOS: wb window %d key move to (%d, %d)`.

Enter opens the active window's first screen-top menu as a popup with item 0
highlighted, exactly the popup the pointer opens on that strip. While it is
open, Down and Up move the highlight (stopping at the last and first item) and
the log carries `CRUGOS: wb menu '<title>' item N highlighted ('<item>')`;
Enter selects the highlighted item through the normal selection path, so it
counts one action and queues `INTUITION_EVENT_MENU` exactly like a click; and
Escape closes the popup without selecting anything and prints
`CRUGOS: wb menu closed (escape)`. Menu keys only apply while a popup is open
and arrow moves only apply while no popup is open, so one byte has one meaning
at a time.

The bytes come from two decoders that do not share an order:

| key | PS/2 set-1 E0 make | USB HID usage page 0x07 | `input.device` byte |
| --- | --- | --- | --- |
| Up | `0x48` | `0x52` UpArrow | `INPUT_KEY_UP` `0x90` |
| Down | `0x50` | `0x51` DownArrow | `INPUT_KEY_DOWN` `0x91` |
| Left | `0x4B` | `0x50` LeftArrow | `INPUT_KEY_LEFT` `0x92` |
| Right | `0x4D` | `0x4F` RightArrow | `INPUT_KEY_RIGHT` `0x93` |

HID puts RightArrow at `0x4F` and LeftArrow at `0x50`, while PS/2 has Left at
`0x4B` and Right at `0x4D`. The PS/2 decoder pushes a byte for an E0 make code
and ignores E0 break codes and E0 modifier releases; the HID decoder maps the
four usages in `drivers/usb/usb_hid.c`. Both feed the same `input.device` queue
the console and the shell read.

Limits: there is no Shift-Tab, because the input layer delivers one byte per
key and carries no modifier state; there are no sub-menus to walk; the menu
keys only drive the first menu of the active window; and outside shell mode
there is no keyboard text entry, only focus, movement and menu selection.

## Host tests

`make test-host` links the real `kernel/gfx/intuition.c`,
`kernel/lib/textgrid.c` and `kernel/shell/shell.c` and reports 3075 checks.
`tests/host/test_intuition.c` replaces the framebuffer primitives with counting
stubs and the cursor accessors with a settable fake (`test_fb_fake_reset()` /
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
- resize grips: the inner bottom-right rectangle, a press on it resizing the
  window, the 120x80 floor, a motion past the screen leaving the size alone,
  the queued `RESIZE` event with its `w`/`h`, and a press-release with no
  motion still counting.
- the keyboard: Tab cycling the focus through the stacking order (raising the
  window and queueing `ACTIVATE`), the four arrow bytes moving the active
  window by 8 pixels and clamping it to the screen, and Enter/Down/Up/Escape
  opening, highlighting, selecting and dismissing the menu popup.
- desktop objects: the eight-icon table, tile clamping at every screen edge, the
  three kinds, selection and clearing, the eight-icon limit, an add without a
  framebuffer or a label failing with -1, a press covered by a window not
  reaching the tile, and the queued `ICON` event with its index and tile origin.
- the depth gadget: the 12x12 box four pixels left of the close box, a press
  sending its window to the bottom, the queued `DEPTH` followed by `ACTIVATE`
  for the new top window, and the no-op on the backmost window.
- icon dragging: a press grabbing a tile, a motion moving it by the pointer
  delta, clamping at all four desktop edges, the queued `ICON_MOVE` with the
  index and the new origin, and the move counter and action growing only when
  the release follows a real motion.
- the send-to-front gadget: the 12x12 box twelve pixels left of the depth box,
  a press raising and activating its window, the queued `FRONT` event, and the
  no-op -- no event and no count -- on the frontmost window.
- the keyboard icon table: `intuition_icon_key()` moving forward through
  Tab/Right/Down and backward through Left/Up with wraparound, Enter selecting
  the first object and then returning 1 for activation, and Escape clearing the
  selection.
- the five-frame window draw: every repaint frames the window, the close box,
  the depth box, the front box and the resize grip, so the window-open
  assertion grew to 75 frames.
- window painters: installing a painter, the body rectangle it receives, the
  gadgets still drawn after it, `intuition_redraw_window()` repainting one
  window, and a NULL painter restoring the static body text; the group is
  `test_intuition_paint`.
- the event queue: a 16-event FIFO, the drop counter on overflow, the action
  count still growing past the queue depth, and the FIFO drain order.

`tests/host/test_textgrid.c` covers the grid on its own, and
`tests/host/test_shell.c` runs the command table against a fake `shell_io`:

- the text grid: the 38-column wrap, the 96-line ring with eviction, backspace
  and carriage return, the tail-preserving overwrite, `tg_line()` truncation and
  the row clamp of `tg_draw()` (8 rows in a 176-pixel body, 3 in 66, 1 in 26)
  plus the inverse-video cursor cell.
- the shell: help/ver/echo/ls/cat/write/sync/mem/uptime/ps, unknown and empty
  lines, the 79-character overflow, Ctrl-U, Ctrl-C, the eight-entry
  Ctrl-P/Ctrl-N history and the fold of extra arguments into the last one.

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
(the phase 6g boot marker, the desktop marker, a `wb selftest ok`, at least one
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

`boot-check.py` then runs with `EXPECT_MENU=yes`, which requires the phase 6g
marker, the desktop marker, the menu open and selected markers, the gadget
click marker, the close marker, at least one `wb action N ...` line and a
`wb selftest ok (N drag(s), M action(s))` with at least one action, and
`scripts/check-disk.py` proves the ext4 root is still clean.

### Shell gate

`make test-workbench-shell` runs `scripts/run-workbench-shell.py` on the same
machine (`usb=off,i8042=off`, an `xhci` controller) with a hotplugged `usb-kbd`
and no pointer at all. It boots with `wb.enable=1 wb.wait=$(WB_SHELL_WAIT)
wb.shell=1 wb.sh=$(WB_SHELL_CMDS)`, waits for `CRUGOS: wb shell ready`,
`device_add`s a `usb-kbd` on `xhci.0`, and then types four commands one key at a
time (60 ms apart, because a HID report carries a single key and QEMU drops
bursts): `help`, `ls /`, `ver` and `echo hi`. Each command must add exactly one
`CRUGOS: sh run '<line>'` line, the `ver` output must contain the version
string, the `echo` output must contain `hi`, and two `screendump`s of the shell
body (`482,322 316x176`) must differ by at least 200 pixels while the later one
carries `0xE0E0E0` text pixels. The gate then requires
`CRUGOS: sh selftest ok (4 command(s))`, exit status 33 and a clean `e2fsck`,
and `boot-check.py` runs with `EXPECT_SHELL=yes` (the shell and desktop markers,
a `sh run`, a `CRUGOS: sh out CrugOS ` line and at least one command in the
`sh selftest ok`). It prints `ok: workbench shell commands (4 command(s), exit
33)`.

### Resize and keyboard gate

`make test-workbench-keys` runs `scripts/run-workbench-keys.py` on the same
machine (`usb=off,i8042=off`, an `xhci` controller) and does both halves in one
boot, with `wb.enable=1 wb.wait=$(WB_KEYS_WAIT) wb.keys=1
wb.acts=$(WB_KEYS_ACTS)`:

1. hotplugs a `usb-mouse`, waits for `CRUGOS: wb desktop ready` and drags
   window 2's grip from (790,490) to (730,430). The window shrinks from 320x200
   to 260x140: the log must show `wb window 2 resized to 260x140` and
   `wb action 1 resize window 2 (260x140)`, and a `screendump` must find the
   desktop background `(16,32,48)` at (798,498), where the old bottom-right
   corner used to be.
2. hotplugs a `usb-kbd` and sends Tab, so window 1 becomes the active window
   (`CRUGOS: wb window 1 focused (Tab)`).
3. sends Right twice. Window 1 moves from (80,90) to (96,90), 8 pixels per key,
   and the gate checks the two `key move to` markers.
4. sends Enter to open window 1's `Workbench` menu, Down to highlight `Info`
   and Enter to select it, requiring the highlight marker, the selection marker
   and `wb action 2 menu window 1 item 1`.
5. waits for exit status 33, then runs `scripts/boot-check.py` with
   `EXPECT_KEYS=yes` (the phase 6g marker, the desktop and keys markers, the
   resize and key-move markers, the menu markers and a two-action
   `wb selftest ok`) and `scripts/check-disk.py`, and prints
   `ok: workbench resize and keyboard navigation (exit 33)`.

Every pointer move is split into steps of at most 100 pixels and follows the
position the guest reports in `CRUGOS: wb cursor at`: QEMU turns a larger
relative motion into several 8-bit HID reports and the guest drops some of
them, the same limit the menu gate works around. The driver also never clicks
while the keyboard part runs, so a stray press cannot steal focus from window
1.

### Desktop gate

`make test-workbench-desk` runs `scripts/run-workbench-desk.py` on the same
machine (`usb=off,i8042=off`, an `xhci` controller) and adds a hotplugged
`usb-mouse` to `wb.enable=1 wb.wait=$(WB_DESK_WAIT) wb.desk=1
wb.acts=$(WB_DESK_ACTS)`:

1. waits for `CRUGOS: wb icon ready ('System:', volume)`, then takes a
   `screendump` and requires the idle tile frame `(255,255,255)` at the tile
   origin (16,240) and the inner panel `(8,16,24)` at (48,264).
2. clicks the object. Its frame and its label must turn amber, so a second
   `screendump` reads `(255,176,0)` at (16,240).
3. clicks the same point again inside `WB_DOUBLE_CLICK_NS`, which opens the
   drawer: `CRUGOS: wb drawer 'System:' opened (5 entries)`.
4. reads the overlap point (700,310): the drawer's title bar is amber there and
   its body is dark, where the Shell window used to show through.
5. clicks the drawer's depth box at (890,72), which sends the drawer to the
   back: the drawer title bar turns idle grey `(128,128,128)` and the Shell
   title bar behind it becomes amber at the same point.
6. waits for exit status 33, then runs `scripts/boot-check.py` with
   `EXPECT_DESK=yes` (the phase 6g marker, the desktop and icon markers, the
   drawer marker, the icon and depth action lines, the desk self-test line and
   a three-action `wb selftest ok`) and `scripts/check-disk.py`, and prints
   `ok: workbench desktop objects and depth (exit 33)`.

### Icon gate

`make test-workbench-icons` runs `scripts/run-workbench-icons.py` on the same
machine (`usb=off,i8042=off`, an `xhci` controller) and adds a hotplugged
`usb-mouse` and `usb-kbd` to `wb.enable=1 wb.wait=$(WB_ICONS_WAIT) wb.desk=1
wb.ikeys=1 wb.acts=$(WB_ICONS_ACTS)`:

1. types Enter, which focuses the `System:` volume
   (`CRUGOS: wb icon 'System:' focused`), and Enter again, which activates it
   and opens the drawer (`CRUGOS: wb drawer 'System:' opened (5 entries)`).
2. moves the pointer to the 64x48 tile at (16,240), presses, drags to (168,376)
   and releases: the release prints `dropped at (168, 376)` and queues the
   `ICON_MOVE` action.
3. takes a `screendump` and requires the amber selected frame `(255,176,0)` at
   (168,376), the dark window body `(8,16,24)` at (176,384) where the drawer
   now covers the tile, and the desktop colour `(16,32,48)` at the old tile
   origin, so the tile really moved and the selection really followed it.
4. clicks window 1 to raise it, then presses its depth box at (410,102): the
   title bar at (90,96) turns idle grey `(128,128,128)`.
5. presses the front gadget at (394,102): the same title bar turns amber
   `(255,176,0)` again and the log carries
   `CRUGOS: wb window 1 sent to front`.
6. waits for exit status 33, then `scripts/boot-check.py` requires the phase 6g
   marker, the icons-ready, focus, drag, drop, front and action markers, the
   icon self-test line and `EXPECT_ICONS=yes` (its ok line reads `desk no icons
   yes storage disk`), and `scripts/check-disk.py` still runs a clean `e2fsck`.
   The gate prints `ok: workbench icon dragging, send-to-front and keyboard
   focus (exit 33)`.

### Drawer gate

`make test-workbench-drawer` is gate 42 and drives
`scripts/run-workbench-drawer.py` through six steps:

1. boots with `wb.enable=1 wb.wait=30 wb.desk=1 wb.dr=1 wb.fill=24 wb.acts=4`
   and a hot-plugged mouse, double-clicks the volume icon at (48,264) and
   requires the select marker, `CRUGOS: wb drawer 'System:' opened`, the fill
   marker, and `CRUGOS: wb drawer ready (N entries, 11 row(s))`; the entry count
   must be at least 25, the row count exactly 11, and the title bar at (600,72)
   active amber.
2. hot-plugs a keyboard, waits for `CRUGOS: wb drawer mode`, presses Down three
   times, requires a `wb drawer scrolled (top N of M)` marker after each press
   and more than 200 changed pixels between screendumps inside the drawer body
   box (566,88)-(900,300).
3. reads (830,400) as desktop `(16,32,48)`, drags the resize grip from
   (904,304) to (1004,404), requires
   `CRUGOS: wb window 3 resized to 460x360` and reads the same pixel as the
   window body `(8,16,24)`.
4. counts body-text pixels `(224,224,224)` in the widened body and requires at
   least 200, proving the rows reflowed into the new height.
5. sends `r`, requires `CRUGOS: wb drawer refreshed (29 entries)`, the drawer
   self-test line and the usual `wb selftest ok` line.
6. requires guest exit status 33 and a clean `e2fsck` on the data disk;
   `boot-check.py` runs with `EXPECT_DRAWER=yes` and its final ok line includes
   `drawer yes`.

The drawer has to be filled because a 360x260 body shows 11 rows while the
stock root holds only a handful of names: with nothing below the fold the
scroll and refresh clamps would have nothing to prove. The 24 filled files plus
the five root entries give 29 entries, roughly three screens, so Down has room
to move and a refresh has entries to reload.

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
- Four windows, two menus of six items, four gadgets and eight desktop objects,
  one open popup at a time. No sub-menus and no clipboard; a menu selection,
  gadget click, close, resize, icon press, icon move, depth press or front
  press queues an event, but
  only the bring-up module drains that queue and no other program is attached
  to it yet. The device reports single presses only: the Workbench, not
  intuition.device, decides what a double click means.
- The resize grip is a fixed 12x12 box in every window and is not themeable. A
  window is resized from its bottom-right corner only; a drag that would push
  it past the screen is refused rather than clamped, so the size stops changing
  at the edge. The shell text grid repaints at the new body size, but the
  static body text is re-laid out from the top-left only: there is no reflow.
- Keyboard navigation has no Shift-Tab and no accelerators, because the input
  layer delivers one byte per key and carries no modifier state; the arrow keys
  move the active window in 8-pixel steps, there are no sub-menus to walk, and
  outside shell mode there is no keyboard text entry. The send-to-front gadget
  raises a window on a press but cannot pin it there, and Tab still cycles the
  stacking order forward, so the backmost window comes round again.
- Desktop objects can be selected, dragged by the pointer and focused from the
  keyboard (`wb.ikeys=1`), but they cannot be renamed: there is no way to
  relabel an object after `intuition_add_icon()`, no multi-select and no icon
  reflow when the screen size changes. The drawer lists at most
  `WB_DRAWER_MAX` (40) root names from a flat read of the root, scrolls one row
  per arrow key and refreshes on `r`, but there is no drag-select, the
  scrollbar cannot be dragged and only one drawer window exists. Rows sit on a
  20-pixel pitch with 8x16 glyphs, so a taller window shows more of them and a
  resize reflows the list. There is one painter slot per window, so a second
  `intuition_set_paint()` replaces the first.
- The shell has no quoting, no globs and no redirection: arguments are split on
  spaces and the extra ones fold into the last argument, `write` creates a
  regular `0644` file but never a directory, `cat` reads at most 4 KiB and
  stops after 24 lines, and there is no `cd` -- every path must be absolute.
- A popup always opens left-aligned on its strip, so a strip near the right
  edge can run its popup off screen, where the clipping primitives cut it. Menu
  and gadget tables are copied, but their strings stay caller-owned and must
  stay valid while the window is open.
- A full desktop repaint per state change, and a polled pointer path: the
  latency is the 10 ms `usb.poll` tick until interrupts or a compositor make
  it cheaper.
- Text is 8x16 with a 20-pixel line pitch and a 47-character line cap on the
  static body path; there is no word wrapping and no fonts. The shell window
  scrolls over 96 lines of 37 usable columns and the drawer scrolls over its
  listing; each has a single window: `wb.shell=1` binds the keyboard to window 2
  and keeps the serial console for diagnostics.
- Outside keys and shell mode the keyboard still reaches the serial console;
  with `wb.keys=1` it drives the desktop instead, and shell mode routes every
  byte to window 2 regardless of which window is active.
- No clipping stack: `fb_fill_rect()` and `fb_draw_text()` clip to the screen,
  so a window cannot draw outside itself into another window's area.
