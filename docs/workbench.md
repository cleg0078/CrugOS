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
and a resize reflows the rows. Phase 6H adds a generic body-input hook to
`intuition.device`, which the drawer uses to select a row on a click and to drag
its scrollbar thumb. Phase 6J adds the Commodities Exchange: a fixed table of
global hotkey utilities, an `Exchange` manager window whose rows toggle them,
and F1..F12 decoded from both the PS/2 and the USB HID keyboard. There are no
application tasks behind the
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

## Drawer row selection and the draggable scrollbar

Phase 6H makes the drawer interactive: a click selects a row and the scrollbar
thumb can be dragged. Both are clients of a new generic hook, so
`intuition.device` itself stays feature-neutral.

### The body-input hook

A window body is inert to `intuition.device`: a press inside it activates the
window and nothing else moves. `intuition_set_body_input()` in
`include/crug/intuition.h` hands one body to a handler:

    int intuition_set_body_input(int id,
        void (*fn)(int id, u32 kind, i32 x, i32 y, void *ctx), void *ctx);

| kind | value | when |
|---|---|---|
| `INTUITION_BODY_PRESS` | 0 | the left button went down inside the body |
| `INTUITION_BODY_MOVE` | 1 | the pointer moved while that button is still held |
| `INTUITION_BODY_RELEASE` | 2 | the button came up |

`x` and `y` are body-relative -- `(0,0)` is the first pixel inside the
2-pixel border, below the title bar -- and `ctx` is the pointer the caller
registered. A `NULL` `fn` clears the handler and an unknown window id returns
`-1`.

With a handler installed, a body press records that window as the internal body
window, forwards `PRESS`, repaints and returns before the title-bar drag, resize
and gadget branches, so a body press can never also start a drag, a resize or a
gadget click. While the button is held every pointer motion is forwarded as
`MOVE` instead of moving a window, and the release forwards `RELEASE`,
repaints and returns before the icon, resize, gadget and drag branches.
`win_draw_body()` and the new `body_rect()` helper share one definition of the
body rectangle,
`x + BORDER, y + BORDER + TITLE_HEIGHT, w - 2*BORDER, h - 2*BORDER - TITLE_HEIGHT`,
so the handler and the painter cannot disagree about where the body is.

### The drawer as the first client

`wb.dsel=1` points that hook at the drawer. The drawer window is opened at
(560,60,360x260); with the two 2-pixel borders and the 20-pixel title bar its
body is 356x236, which holds 11 of the 29 entries (five root entries plus the 24
that `wb.fill=` creates) on the 20-pixel row pitch, so the maximum top row is
18. A click at body `y` selects index `top + (y - 6) / 20` and paints an amber
band (`INTUITION_MENU_HIGHLIGHT_RGB` `0x00FFB000`) across the row with the text
in the window-background colour, and prints
`CRUGOS: wb drawer selected '<name>' (index N of M)`.

The scrollbar gains a draggable thumb: the 4-pixel track (`WB_TRACK_W`) sits at
body-local `x = body width - 8`, starts at `y + 6` and is `body - 12` tall,
with a thumb `max(12, track * rows / entries)` pixels high whose offset is
proportional to the first visible row. A press on the track jumps the view to
that position and grabs the thumb at the press point, printing
`CRUGOS: wb drawer scrollbar grabbed`; dragging follows the pointer and
repaints, and the release prints the final top row as
`CRUGOS: wb drawer scrollbar to (top N of M)`.

Select mode also prints `CRUGOS: wb select mode (click a row, drag the
scrollbar)` right after `CRUGOS: wb desktop ready`, and makes the Workbench
self-test wait for two selections and one scrollbar drag, ending with
`CRUGOS: wb select selftest ok (2 select(s), 1 scrollbar drag(s))` or
`CRUGOS: wb select timeout (N/M)`. The drawer itself still opens from
`wb.desk=1` alone, so a boot that wants select mode passes both tokens.

The gate that proves the selection and the drag is
`make test-workbench-select`, gate 43; its eight steps are described under
[Select gate](#select-gate).

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

## Screens

Phase 6I gives Intuition more than one screen. Up to `INTUITION_MAX_SCREENS` (3)
screens live in memory at once, each with its own name, background colour,
window list and desktop icons; only the front screen is painted, and a screen
that is not in front keeps every window, icon and piece of state it had. Screens
are never destroyed, so a program can keep a workspace on a second screen and
switch back to it.

The API in `include/crug/intuition.h`:

| function | result |
| --- | --- |
| `intuition_add_screen(name, bg_rgb)` | the new screen index, or -1 when there is no framebuffer, `name` is NULL, or three screens already exist |
| `intuition_set_screen(index)` | 0, or -1 when `index` is out of range; selects the target screen for later `intuition_open()` and icon calls |
| `intuition_screen_switch(index)` | 0, or -1 when `index` is out of range; brings that screen to the front |
| `intuition_move_window_screen(id, index)` | 0, or -1 for an unknown window or screen; re-parents one window |
| `intuition_screen_front()`, `_count()`, `_switches()`, `_moves()` | the front index, the screen count and the two counters |
| `intuition_window_screen(id)` | the screen a window lives on, or -1 for an unknown id |

Out-of-range screen indexes and unknown window ids are refused with -1, and
because only the front screen is painted a press aimed at a window on a covered
screen reaches nothing and never activates it.

A switch drops any pending menu, drag, resize, gadget, body or icon interaction,
activates the top window of the new screen, counts one switch, queues
`INTUITION_EVENT_SCREEN` with window 0 and item = the new screen index, prints
`CRUGOS: wb screen %u front (%u window(s))` and repaints. Switching to the
screen that is already in front is a no-op -- no event, no count and no repaint.
A move re-parents the window, drops the interactions it owned, hands activation
to the next window of the front screen, counts one move, queues
`INTUITION_EVENT_SCREEN` with window = the id and item = the target screen,
prints `CRUGOS: wb window %d moved to screen %u` and repaints.

The screen strip sits at the right end of the menu bar, one box per screen with
the front screen's box highlighted. A box is `strlen(name) * 8 + 12` pixels
wide, the strip starts at `screen_width - total_width - 6`, and its hit area is
`0 <= y < 18`. A press on a box that is not in front switches screens and
counts one action; a press on the front screen's own box does nothing.

Four colours define the desktop:

| what | value | RGB |
| --- | --- | --- |
| screen 0 background | `INTUITION_DESKTOP_RGB` `0x00102030` | (16,32,48) |
| Tools screen background | `0x00203010` | (32,48,16) |
| window body | `INTUITION_WINDOW_RGB` `0x00081018` | (8,16,24) |
| active title bar | `INTUITION_TITLE_ACTIVE_RGB` `0x00FFB000` | (255,176,0) |

`wb.screens=1` (default 0, the plain single-screen desktop) makes
`kernel/gfx/workbench.c` add a second screen called `Tools` with background
`0x00203010`, open a 340x200 `Tools` window at (160,140) on it, give that
window the `Tools` {Move, Close} and `Workbench` {About, Info, Quit} menus and
a body-relative (12,60,160,22) `Send to Workbench` gadget, return to screen 0 and
print `CRUGOS: wb screens ready (%u screen(s), front %d)`. A press on the gadget
moves the Tools window to screen 0, which is the programmatic way to show a move
without dragging the window's title bar across a screen edge. In screens mode the
Workbench window body reads `phase s1` and the self-test ends with
`CRUGOS: wb screens selftest ok (%u switch(es), %u move(s))`.

## Commodities Exchange

Phase 6J adds a global hotkey registry and a manager window for it. A
*commodity* is a small named utility with one function key attached; pressing
the key runs it from anywhere on the desktop, and the `Exchange` window lists
the registered commodities so a row click can toggle one on or off.

`include/crug/commodity.h` and `kernel/gfx/commodities.c` are pure: no
framebuffer, no block I/O and no timers, so `tests/host/test_commodity.c` links
the real code and drives it directly. The table is fixed at `COMMODITY_MAX`
entries; space is reserved up front, and a registered commodity starts enabled
with zero counters.

| constant | value | meaning |
| --- | --- | --- |
| `COMMODITY_MAX` | 4 | registry slots |
| `COMMODITY_NAME_MAX` | 16 | bytes in a name, including the NUL |
| `COMMODITY_HEADER_H` | 24 | height of the Exchange header |
| `COMMODITY_ROW_TOP` | 28 | offset of the first row from the body top |
| `COMMODITY_ROW_H` | 26 | row height |
| `COMMODITY_ROW_X` | 6 | left inset of a row's text |

`struct commodity` carries the state: `char name[COMMODITY_NAME_MAX]`,
`u8 hotkey`, `u8 enabled`, `u32 fires` and `u32 blocked`. The API in
`include/crug/commodity.h`:

| function | result |
| --- | --- |
| `commodity_reset()` | clears every slot, the two counters and the enabled state |
| `commodity_register(name, hotkey)` | the new index, or -1 for a NULL, empty or 16-byte name, a hotkey outside `INPUT_KEY_F1`..`INPUT_KEY_F12`, a duplicate name or a duplicate hotkey |
| `commodity_toggle(index)` | the new enabled state, or -1 for an out-of-range index |
| `commodity_set_enabled(index, on)` | 0, or -1 for an out-of-range index |
| `commodity_hotkey(key, &fired)` | the owning index or -1; `*fired` is 1 when the action ran and 0 when the commodity was disabled |
| `commodity_count()`, `commodity_at(i)` | the registered count and one slot |
| `commodity_index_of_hotkey(key)` | the owning index without running anything, or -1 |
| `commodity_fires()`, `commodity_blocked()`, `commodity_enabled_count()` | the two global counters and the number of enabled slots |
| `commodity_key_name(key)` | `"F1"`..`"F12"`, or NULL for any other byte |
| `commodity_row_at(body_relative_y)` | the row under a body-relative y, or -1 above the first row or past the last |

Four built-ins are registered by `kernel/gfx/workbench.c`:

| commodity | key | action |
| --- | --- | --- |
| `Volume up` | F2 | raise the audio mixer by `WB_CX_VOLUME_STEP` (600), clamped at `AUDIO_VOLUME_MAX` (0) |
| `Volume down` | F3 | lower the mixer by the same step, clamped at `AUDIO_VOLUME_MIN` (-7680) |
| `Mute` | F4 | toggle the mixer mute |
| `Clock` | F6 | report the monotonic uptime |

A key is dispatched only while the Workbench runs with the exchange enabled and
no other text consumer owns the keyboard. `commodity_hotkey()` finds the owner
first: a disabled commodity bumps its `blocked` count and prints
`CRUGOS: wb commodity %s blocked (hotkey off)` without running anything, while
an enabled one bumps `fires`, runs the action and prints
`CRUGOS: wb action %u commodity %s fired (...)`. A key no commodity owns is
handed to the desktop, so F2..F6 keep their old meaning when the exchange is
off. Volume up and Volume down print the new mixer value in units, Mute prints
`fired (muted)` or `fired (unmuted)`, and Clock prints `fired (uptime %u s)`.

The manager is the `Exchange` window at (560,120), size 344x196. It is opened
last, so it starts on top of the desktop. Its header is
`COMMODITY_HEADER_H` (24) pixels high; rows start `COMMODITY_ROW_TOP` (28)
pixels below the body top and are `COMMODITY_ROW_H` (26) pixels apart. A row
paints its name at `x + COMMODITY_ROW_X`, the key name right-aligned at
`x + w - 96` and `[on]`/`[off]` at `x + w - 44`, amber when the commodity is
enabled and grey when it is not. `commodity_row_at()` maps a body-relative y to
a row, and a click on a row toggles it and prints
`CRUGOS: wb action %u commodity %s on|off`.

The keys themselves are decoded in `include/crug/input.h` and the two drivers:
`INPUT_KEY_F1` `0x94u` through `INPUT_KEY_F12` `0x9Fu` sit above the arrow
keys. PS/2 set-1 make codes `0x3B`..`0x44` map to F1..F10, `0x57` to F11 and
`0x58` to F12; break codes yield no byte and Shift/Ctrl do not change the make
code. USB HID boot usages `0x3A`..`0x45` map to F1..F12, and `0x46` (F13) is
ignored.

`wb.cx=<0|1>` (default 0, only 0 disables) turns the exchange on, and it
requires `wb.enable=1`. It prints
`CRUGOS: wb cx ready (%u commodity(ies))` once the four built-ins are
registered. With `wb.acts=` set, the self-test also waits for two row toggles and
two fires and ends with
`CRUGOS: wb cx selftest ok (%u toggle(s), %u fire(s))`; a missing action prints
`CRUGOS: wb cx timeout (%u/%u)` and panics the boot. The exchange pump keeps
running while `wb_hold()` has the desktop open, so the demo boots react to
F2..F6 as well.

## Application bundles

Phase 7A turns a program into a desktop object. `kernel/app/app.c` scans
`/Applications` once during boot, parses each bundle's `manifest.toml`, checks
that the payload program exists and keeps the accepted bundles in a fixed
registry of `APP_MAX` (8) entries sorted by id. The registry is not part of the
Workbench: the desktop and the ring-3 shell are two front ends onto the same
table, and [apps.md](apps.md) describes the bundle format, the parser and its
rejection reasons.

`wb.app=<0|1>` (default 0, only 0 disables) adds the `Applications:` icon at
(16,300), drawn 64x48 with `INTUITION_ICON_DRAWER`, and it requires
`wb.enable=1`. Double-clicking the icon opens the `Applications:` window at
(560,320), 340x240, on first use. The window lists the registry in id order,
one row per application, `WB_APP_ROW_H` (20) pixels apart, with the name at
`x + 6` and the version right-aligned at `x + w - 76`; an empty registry draws
`no applications` in the idle title colour instead. A single click on a row
selects it and paints an amber band (`INTUITION_MENU_HIGHLIGHT_RGB`), and a
second click on the same row within `WB_DOUBLE_CLICK_NS` (600 ms) launches it
through `app_launch()`: the program's argv[0] is the bundle directory, so it
can find its own files, and the desktop reaps it with
`CRUGOS: wb app reaped tid N exit E` once it turns into a zombie.

Only one launch is in flight at a time; a click while a program still runs
prints `CRUGOS: wb app busy (tid N)` and is refused, and a launch that fails
prints `CRUGOS: wb app launch <id> failed (<rc>)`. With `wb.acts=` set, the
self-test waits for that many reaps and then ends with
`CRUGOS: wb app selftest ok (%u click(s), %u launch(es))`; a wait that expires
first prints `CRUGOS: wb app timeout (%u/%u)` and panics the boot.

## Packages window (phase 7B, a toggle since 7C)

Phase 7B gives the desktop an installer and phase 7C turns the body press into a
toggle. `wb.pkg=<0|1>` (default 0, only 0
disables) opens a `Packages` window and requires `wb.enable=1`. The window is
created in `wb_init()` at (40,330), 420x200, titled `Packages` with the body
heading `CrugOS Packages`, and that happens before `pkg_init()` has read
`/System/pkgdb`, which is why the boot line reads
`CRUGOS: wb pkg ready (window N, 0 package(s))` even on a disk that carries
packages: the registry fills later in `main.c` and the next body paint shows
it. A window that cannot be created prints
`CRUGOS: wb pkg window failed` instead.

The body prints `click to install captest.crpkg` under the heading and then one
row per installed package, `WB_PKG_ROW_H` (18) pixels apart: the id at
`x + 6` and the version right-aligned at `x + w - 150`, in the same palette
and row layout as the Applications window next door. A successful install writes
`installed` in the amber highlight colour
(`INTUITION_MENU_HIGHLIGHT_RGB`, `0x00FFB000`) at `y + h - 16`; a
failed one writes `pkg_reason_text()` of the reason there instead, which is
the same sentence the kernel prints on the serial line.

A body press is the whole interface. `wb_pkg_toggle()` counts the operation
and looks the id up in the registry: when `WB_PKG_DEMO_ID` (`captest`) is
absent it calls `pkg_install()` on `WB_PKG_DEMO`
(`/System/packages/captest.crpkg`); when it is present it calls
`pkg_remove("captest")` instead. A success prints
`CRUGOS: wb pkg installed /System/packages/captest.crpkg (%u package(s))` or
`CRUGOS: wb pkg removed captest (%u package(s))`, and a refusal prints
`CRUGOS: wb pkg rejected <id> (<reason>)`. The body paints `installed`
after an install and `removed` after a removal, so the same press keeps
flipping the row.

With `wb.acts=` set (the gate sets it to 3: install, remove, install) the
self-test waits for the presses: it ends with
`CRUGOS: wb pkg selftest ok (%u install(s), %u remove(s), %u package(s))`, or
with `CRUGOS: wb pkg timeout (%u/%u)` and a panicked boot when nothing
arrives inside `wb.wait=`. The finished desktop is held for 1.5 s
before the exit so the last screendump can land. `make test-workbench-pkg` is
gate 59; [packages.md](packages.md) describes the package format, the trust list,
the update and removal pipeline, and [userspace.md](userspace.md) the
`SYS_PKG_*` syscalls.

## GPU-backed scanout (phase 8A)

Phase 8A lets the desktop put its pixels on a virtio-gpu 2D device instead of
the firmware framebuffer. `drivers/gpu/virtio_gpu.c` creates one
`B8G8R8X8_UNORM` resource (id 1) sized to the boot mode, attaches contiguous
backing pages (1000 at 1280x800) as a linear framebuffer and hands the
resource to `SET_SCANOUT`; `include/crug/fb.h` then attaches it as the console
and desktop surface through `fb_attach_linear()`.

The desktop code itself is unchanged. `fb_store()` still writes pixels, but it
merges every write into one damage rectangle (`struct gpu_damage`, a bounding
box with exclusive x1/y1) and `intuition_redraw()` /
`intuition_redraw_window()` bracket a repaint with `fb_present_begin()` and
`fb_present_end()`, which clip the rectangle, transfer it with
`TRANSFER_TO_HOST_2D` and flush it with `RESOURCE_FLUSH`. One repaint is one
transfer and one flush; the console flushes once per line rather than once per
glyph. Window body text paints `phase s1`, and the boot prints
`CRUGOS: virtio-gpu ready (1280x800, 1000 backing page(s), resource 1, modern pci)` and
`CRUGOS: gpu scanout ok (1280x800, 1 transfer(s), 1 flush(es))` before the desktop
marker.

Gate 60 `make test-gpu` is the proof: it boots with `-vga none -device`
`virtio-gpu-pci,id=gpu0` and `wb.enable=1 wb.wait=30 gpu.enable=1 gpu.test=1`, so
no firmware framebuffer exists and every pixel of the desktop came from the
device; `scripts/run-gpu.py` takes a QMP `screendump` and asserts the desktop
background, the amber shell title, the idle Workbench title and its frame
before the guest exits 33. The Workbench still renders in software -- the
device only carries the finished pixels. Gate 61 `make test-gpu-usb` repeats
it with a hotplugged `usb-mouse` on an xHCI controller and proves the
keyboard and mouse path that the 64-bit BAR fix repaired. See [gpu.md](gpu.md).

## The event queue

The event queue is the typed report the desktop hands to its caller:

| field | meaning |
| --- | --- |
| `type` | `INTUITION_EVENT_ACTIVATE`, `_DRAG`, `_MENU`, `_GADGET`, `_CLOSE`, `_RESIZE`, `_ICON`, `_DEPTH`, `_ICON_MOVE`, `_FRONT` or `_SCREEN` (0 = none) |
| `window` | window id the event belongs to, 0 when it has none |
| `index` | menu index, gadget index, desktop object index or screen index, -1 when unused |
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
other types leave both fields at 0. A switch queues `INTUITION_EVENT_SCREEN`
with window 0 and the new screen index in `item`; a window move queues
`INTUITION_EVENT_SCREEN` with the window id and the target screen in `item`. A
depth press queues
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
   | 1 | (80, 90) 360x220 | `Workbench` | `CrugOS Workbench` / `phase s1` / `software rendered` |
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
| `wb.dsel=1` | 0/1 | give the drawer body the click-and-drag handler: a row click selects, the scrollbar thumb can be dragged (parsed before `wb.shell=`, and the drawer must be opened too) |
| `wb.hold=S` | 0..3600 | seconds `wb_hold()` keeps the desktop on screen after the boot marker |
| `wb.screens=1` | 0/1 | add a `Tools` screen, the `Send to Workbench` gadget and the screens self-test |
| `wb.cx=1` | 0/1 | register the four commodity hotkey utilities, open the `Exchange` manager and run the cx self-test (needs `wb.enable=1`) |
| `wb.app=1` | 0/1 | add the `Applications:` icon and its bundle window, and run the app self-test (needs `wb.enable=1`) |
| `wb.pkg=1` | 0/1 | open the `Packages` window, install the demo package on a body press and run the package self-test (needs `wb.enable=1`) |

`make test-workbench` boots with `wb.enable=1 wb.wait=30 wb.drag=1`,
`make test-workbench-menu` with `wb.enable=1 wb.wait=30 wb.menu=1 wb.acts=4`,
`make test-workbench-keys` with `wb.enable=1 wb.wait=30 wb.keys=1
wb.acts=2`, and `make test-workbench-desk` with `wb.enable=1 wb.wait=30
wb.desk=1 wb.acts=3`, and `make test-workbench-icons` with `wb.enable=1
wb.wait=30 wb.desk=1 wb.ikeys=1 wb.acts=5`, and `make test-workbench-drawer`
with `wb.enable=1 wb.wait=30 wb.desk=1 wb.dr=1 wb.fill=24 wb.acts=4`, and
`make test-workbench-select` with `wb.enable=1 wb.wait=30 wb.desk=1 wb.dr=1
wb.dsel=1 wb.fill=24 wb.acts=3`, and `make test-workbench-screens` with
`wb.enable=1 wb.wait=30 wb.screens=1 wb.acts=4`, and `make test-workbench-cx` with
`wb.enable=1 wb.wait=30 wb.cx=1 wb.acts=4`, and `make test-app-bundles` with
`wb.enable=1 wb.wait=30 wb.app=1 wb.acts=1`, and `make test-workbench-pkg` with
`wb.enable=1 wb.wait=30 wb.pkg=1`. `make demo` boots the whole desktop
instead -- `wb.enable=1 wb.wait=0 wb.desk=1 wb.dr=1 wb.dsel=1 wb.screens=1
wb.cx=1 wb.shell=1 wb.fill=<files> wb.hold=<seconds>`: `wb.wait=0`
turns both self-tests into no-ops, so the interactive desktop needs neither a
drag nor an action before the boot marker, `wb.hold=` keeps the guest alive
afterwards, and the other tokens make the object, the drawer, both screens, the
Exchange and a live Shell appear at boot.

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
CRUGOS: phase s1 boot ok
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
CRUGOS: phase s1 boot ok
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
CRUGOS: phase s1 boot ok
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
CRUGOS: phase s1 boot ok
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
CRUGOS: phase s1 boot ok
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
CRUGOS: phase s1 boot ok
```

The select gate is an eighth boot: same filled drawer, but the pointer now
selects rows and drags the thumb instead of the keys scrolling it:

```
CRUGOS: wb desktop ready (1024x768, 2 window(s))
CRUGOS: wb select mode (click a row, drag the scrollbar)
CRUGOS: wb icon ready ('System:', volume)
CRUGOS: storage remounted read-write
CRUGOS: wb fill 24 file(s)
CRUGOS: storage remounted read-only
CRUGOS: wb drawer 'System:' opened (29 entries)
CRUGOS: wb drawer ready (29 entries, 11 row(s))
CRUGOS: wb icon 'System:' selected
CRUGOS: wb icon 'System:' selected
CRUGOS: wb drawer selected '<name>' (index 2 of 29)
CRUGOS: wb drawer scrollbar grabbed
CRUGOS: wb drawer scrollbar to (top 18 of 29)
CRUGOS: wb drawer selected '<name>' (index 18 of 29)
CRUGOS: wb desk selftest ok (1 drawer(s))
CRUGOS: wb select selftest ok (2 select(s), 1 scrollbar drag(s))
CRUGOS: phase s1 boot ok
```

The screens gate is a ninth boot: two screens, a switch and a move between them:

```
CRUGOS: wb desktop ready (1024x768, 2 window(s))
CRUGOS: wb screens ready (2 screen(s), front 0)
CRUGOS: wb screen 1 front (1 window(s))
CRUGOS: wb action 1 screen 1 front
CRUGOS: wb window 3 moved to screen 0
CRUGOS: wb action 3 screen-move window 3 to 0
CRUGOS: wb screen 0 front (3 window(s))
CRUGOS: wb action 4 screen 0 front
CRUGOS: wb screens selftest ok (2 switch(es), 1 move(s))
CRUGOS: phase s1 boot ok
```

The commodities gate is a tenth boot: four registered utilities, a manager
window whose rows toggle them, and the F4/F6 hotkeys:

```
CRUGOS: wb desktop ready (1024x768, 2 window(s))
CRUGOS: wb cx ready (4 commodity(ies))
CRUGOS: wb action 1 commodity Mute off
CRUGOS: wb commodity Mute blocked (hotkey off)
CRUGOS: wb action 2 commodity Mute on
CRUGOS: wb action 3 commodity Mute fired (muted)
CRUGOS: wb action 4 commodity Clock fired (uptime 1 s)
CRUGOS: wb cx selftest ok (2 toggle(s), 2 fire(s))
CRUGOS: wb selftest ok (0 drag(s), 4 action(s))
CRUGOS: phase s1 boot ok
```

The app-bundle gate boots the desktop with a three-bundle registry
(`Broken.app` is refused for having no version) and double-clicks the icon and
then the first row:

```
CRUGOS: wb desktop ready (1024x768, 2 window(s))
CRUGOS: wb app icon ready (Applications:)
CRUGOS: app rejected Broken.app (no-version)
CRUGOS: app registry ready (2 app(s), 1 rejected)
CRUGOS: wb app ready (2 application(s))
CRUGOS: wb app selected hello (index 0 of 2)
CRUGOS: wb app launch hello tid 11
CRUGOS: wb app reaped tid 11 exit 0
CRUGOS: wb selftest ok (0 drag(s), 0 action(s))
CRUGOS: wb app selftest ok (2 click(s), 1 launch(es))
CRUGOS: phase s1 boot ok
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
| `CRUGOS: wb select mode (click a row, drag the scrollbar)` | `wb.dsel=1` installed the drawer's body-input handler |
| `CRUGOS: wb drawer selected '<name>' (index N of M)` | a click selected a drawer row |
| `CRUGOS: wb drawer scrollbar grabbed` | a press on the track grabbed the thumb |
| `CRUGOS: wb drawer scrollbar to (top N of M)` | the thumb drag settled on a new top row |
| `CRUGOS: wb select selftest ok (N select(s), M scrollbar drag(s))` | select mode saw its row clicks and its scrollbar drag |
| `CRUGOS: wb select timeout (N/M)` | the wait expired with selections or the drag missing |
| `CRUGOS: wb screens ready (N screen(s), front F)` | `wb.screens=1` added the Tools screen |
| `CRUGOS: wb screen N front (K window(s))` | a press on the screen strip switched screens |
| `CRUGOS: wb window N moved to screen M` | a window was re-parented to another screen |
| `CRUGOS: wb action N screen N front` | a strip switch counted one action |
| `CRUGOS: wb action N screen-move window W to S` | a screen move counted one action |
| `CRUGOS: wb screens selftest ok (N switch(es), M move(s))` | screens mode saw its switches and moves |
| `CRUGOS: wb cx ready (N commodity(ies))` | `wb.cx=1` registered the four commodity utilities |
| `CRUGOS: wb action N commodity '<name>' on\|off` | a manager row click toggled a commodity |
| `CRUGOS: wb commodity '<name>' blocked (hotkey off)` | a disabled commodity's key was pressed and refused |
| `CRUGOS: wb action N commodity '<name>' fired (...)` | an enabled commodity's key ran its action |
| `CRUGOS: wb cx selftest ok (N toggle(s), M fire(s))` | the exchange self-test saw its toggles and fires |
| `CRUGOS: wb cx timeout (N/M)` | the wait expired with toggles or fires missing |
| `CRUGOS: wb app icon ready (Applications:)` | `wb.app=1` added the Applications icon |
| `CRUGOS: wb app ready (N application(s))` | a double click opened the bundle window |
| `CRUGOS: wb app selected '<id>' (index N of M)` | a click selected a bundle row |
| `CRUGOS: wb app launch '<id>' tid N` | a second click on the row launched the bundle |
| `CRUGOS: wb app reaped tid N exit E` | the desktop collected the child's exit code |
| `CRUGOS: wb app busy (tid N)` | a launch was attempted while a program was still running |
| `CRUGOS: wb app selftest ok (N click(s), M launch(es))` | the app self-test saw its clicks and launches |
| `CRUGOS: wb app timeout (N/M)` | the wait expired before a launch was reaped |
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
`kernel/gfx/commodities.c`, `kernel/app/manifest.c`, `kernel/app/pkg.c`,
`kernel/lib/caps.c`, `kernel/lib/perms.c`, `kernel/crypto/sha512.c`,
`kernel/crypto/ed25519.c`, `kernel/lib/textgrid.c` and
`kernel/shell/shell.c` and reports 8528 checks.
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
- the body-input hook: `test_intuition_body_input` (printed as
  `- intuition body input`) registers a handler on a window at (100,100,300,300)
  and checks the body-relative PRESS and MOVE coordinates (48,78 then 68,108),
  that nothing is forwarded after the release, that a title-bar press and the
  close gadget never reach the handler, that a closed window leaves no stale
  interaction behind, that `NULL` clears the handler and that
  `intuition_set_body_input(99, ...)` returns -1.
- the event queue: a 16-event FIFO, the drop counter on overflow, the action
  count still growing past the queue depth, and the FIFO drain order.
- the screen model: a maximum of three screens, `intuition_set_screen()`
  choosing the target for later windows, `intuition_window_screen()` reporting
  where a window lives, only the front screen being painted (a body painter on
  a covered window is not called and a press aimed at it hits nothing and does
  not activate it), a window move queueing one `SCREEN` event and re-handing
  activation, out-of-range window and screen indexes refused, switching to the
  front screen being a no-op that still sets the target, and a drag pending
  across a switch dropped.

`tests/host/test_textgrid.c` covers the grid on its own, and
`tests/host/test_shell.c` runs the command table against a fake `shell_io`:

- the text grid: the 38-column wrap, the 96-line ring with eviction, backspace
  and carriage return, the tail-preserving overwrite, `tg_line()` truncation and
  the row clamp of `tg_draw()` (8 rows in a 176-pixel body, 3 in 66, 1 in 26)
  plus the inverse-video cursor cell.
- the shell: help/ver/echo/ls/cat/write/sync/mem/uptime/ps, unknown and empty
  lines, the 79-character overflow, Ctrl-U, Ctrl-C, the eight-entry
  Ctrl-P/Ctrl-N history and the fold of extra arguments into the last one.
- the commodity registry: registration limits (NULL, empty and 16-byte
  names, a hotkey outside F1..F12, duplicate names and hotkeys), the toggle and
  the enabled count, a disabled commodity's key refused with `blocked` rising
  and no action, an enabled one running with `fires` rising, the row hit test
  below the header and past the last row, and the key names.
- the app manifest parser: `tests/host/test_app.c` accepts the shipped Hello
  and Orbit manifests (with and without a final newline, padded keys and
  comments), rejects the Broken bundle, walks the reason table (missing or
  uppercase id, duplicate key, two-part or missing version, an absolute or
  `..` program, an unknown section or key, a 5-digit colour, a ninth tool type,
  a 16-byte key) and checks `app_path_join()`, the version comparison, the
  `app_format_info()` record and `app_tooltype_find()`.
- packages and capabilities: `tests/host/test_pkg.c`'s `test_pkg_functions`
  checks the SHA-512 digests against the standard vectors, the Ed25519
  signature vector (a known seed, public key and message), the capability
  table (`cap_parse_list()`, `cap_format()` and `cap_intersect()`), the
  permission policy (`perms_parse()`'s `default` and per-id rules, its
  `perms-line`/`perms-id`/`perms-cap`/`perms-dup`/`perms-full` reasons,
  comments and blank lines, and `perms_granted()` falling back to the
  default), the trust list (a `pub <hex> <publisher>` line, its
  `trust-key`/`trust-hex`/`trust-line`/`trust-full` reasons and the full-key
  lookup), the 512-byte header (magic, size, flags, the five text fields,
  the payload size window, the capability mask and the key id), the payload
  container (paths, per-file and total sizes, the file-count cap and the
  trailing-byte check), `pkg_format_info()` / `pkg_reason_text()`
  and the version-comparison cases the update and the `downgrade` refusal
  depend on.

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
(the phase s1 boot marker, the desktop marker, a `wb selftest ok`, at least one
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

`boot-check.py` then runs with `EXPECT_MENU=yes`, which requires the phase s1
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
   `EXPECT_KEYS=yes` (the phase s1 marker, the desktop and keys markers, the
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
   `EXPECT_DESK=yes` (the phase s1 marker, the desktop and icon markers, the
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
6. waits for exit status 33, then `scripts/boot-check.py` requires the phase s1
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

### Select gate

`make test-workbench-select` is gate 43 and drives
`scripts/run-workbench-select.py` on the same machine setup (an `xhci`
controller, a hot-plugged `usb-mouse`) through eight steps:

1. boots with `wb.enable=1 wb.wait=$(WB_SELECT_WAIT) wb.desk=1 wb.dr=1
   wb.dsel=1 wb.fill=$(WB_SELECT_FILL) wb.acts=$(WB_SELECT_ACTS)`, so the root
   is filled and the drawer overflows its 11 rows, and double-clicks the
   `System:` desktop icon to open the drawer at (560,60,360x260).
2. takes a `screendump` and requires row 0 of the body to be the
   window-background colour `(8,16,24)`, so nothing is selected before a click.
3. clicks row 2 at (600,136) and requires the amber band `(255,176,0)` at
   (890,136) while row 0 stays dark, plus
   `CRUGOS: wb drawer selected '<name>' (index 2 of 29)`.
4. presses the scrollbar thumb at (911,100) and drags it to (911,280): the log
   reports `CRUGOS: wb drawer scrollbar to (top 18 of 29)`, the thumb moved and
   row 2's amber band is gone because that row is no longer in view.
5. clicks the new row 0 at (600,96), which is index 18, and requires its band to
   turn amber, proving the click map follows the scrolled top row.
6. waits for
   `CRUGOS: wb select selftest ok (2 select(s), 1 scrollbar drag(s))`.
7. requires guest exit status 33 and prints
   `ok: workbench drawer selection and scrollbar dragging (exit 33)`.
8. runs `scripts/boot-check.py` with `EXPECT_SELECT=yes` (its ok line reads
   `select yes`, everything else `no`) and `scripts/check-disk.py`, which must
   leave the root image with a clean `e2fsck`.

The gate reuses the same 24-file fill as the drawer gate, because the click map
and the thumb geometry only diverge from a static list once the entries exceed
the 11 visible rows. Its knobs are `EXPECT_SELECT` (`no`), `WB_SELECT_WAIT`
(30), `WB_SELECT_ACTS` (3), `WB_SELECT_FILL` (24) and
`WB_SELECT_RUN_TIMEOUT_SEC` (240); see [build.md](build.md).

### Screens gate

`make test-workbench-screens` is gate 54 and drives
`scripts/run-workbench-screens.py` on the same machine setup (an `xhci`
controller, a hot-plugged `usb-mouse`) through five steps:

1. boots with `wb.enable=1 wb.wait=$(WB_SCREEN_WAIT) wb.screens=1
   wb.acts=$(WB_SCREEN_ACTS)` on the 1024x768 framebuffer and requires
   `CRUGOS: wb screens ready (2 screen(s), front 0)` before anything is
   clicked.
2. clicks the `Tools` box at (992,9) -- the strip starts at 882 -- and requires
   `CRUGOS: wb screen 1 front (1 window(s))` and
   `CRUGOS: wb action 1 screen 1 front`, the Tools background `(32,48,16)` in
   the frame and no Workbench window left visible, because only the front
   screen is painted.
3. presses the `Send to Workbench` gadget at (254,233): the log reports
   `CRUGOS: wb window 3 moved to screen 0` and
   `CRUGOS: wb action 3 screen-move window 3 to 0`.
4. clicks the `Workbench` box at (924,9) and requires
   `CRUGOS: wb screen 0 front (3 window(s))` and
   `CRUGOS: wb action 4 screen 0 front`; a screendump then shows the Workbench
   background `(16,32,48)` and the moved window's active title bar
   `(255,176,0)` above its body `(8,16,24)`.
5. waits for `CRUGOS: wb screens selftest ok (2 switch(es), 1 move(s))`, guest
   exit status 33 and the line
   `ok: workbench screens and a window moving between them (exit 33)`, then runs
   `scripts/boot-check.py --expect-screen=yes` (a flag, not a positional
   argument, so the argument order and the argc range are unchanged; its ok line
   reads `screen yes`) and `scripts/check-disk.py`.

Its knobs are `WB_SCREEN_WAIT` (30), `WB_SCREEN_ACTS` (4) and
`WB_SCREEN_RUN_TIMEOUT_SEC` (240), plus the shared `WB_QEMU_EXTRA`; see
[build.md](build.md).

### Commodities gate

`make test-workbench-cx` is gate 55 and drives
`scripts/run-workbench-cx.py` on the same machine setup (an `xhci` controller,
`usb=off,i8042=off`, a hot-plugged `usb-mouse` and a hot-plugged `usb-kbd`)
through six steps:

1. boots with `wb.enable=1 wb.wait=$(WB_CX_WAIT) wb.cx=1 wb.acts=$(WB_CX_ACTS)`
   and requires `CRUGOS: wb cx ready (4 commodity(ies))` before anything is
   clicked.
2. hot-plugs `mouse0` and `kbd0` on `xhci.0`, reads the starting cursor from
   `CRUGOS: gfx ready` and moves the pointer to the `Mute` row of the manager
   (the window is at (560,120) and its body starts at (562,142), so row 2
   centres on (600,235)).
3. clicks the row and requires `CRUGOS: wb action 1 commodity Mute off`, then
   presses F4 and requires `CRUGOS: wb commodity Mute blocked (hotkey off)`:
   a disabled commodity must not run.
4. clicks the same row again (`CRUGOS: wb action 2 commodity Mute on`), presses
   F4 for `CRUGOS: wb action 3 commodity Mute fired (muted)` and F6 for
   `CRUGOS: wb action 4 commodity Clock fired (uptime ...)`.
5. takes a `screendump` and requires the Exchange body at (700,250) to be
   (8,16,24) and its active title bar at (600,130) to be (255,176,0).
6. waits for `CRUGOS: wb cx selftest ok (2 toggle(s), 2 fire(s))`, guest exit
   status 33 and the line
   `ok: workbench commodities exchange (exit 33)`, then runs
   `scripts/boot-check.py --expect-cx=yes` (a flag, not a positional argument,
   so the argument order and the argc range are unchanged; its ok line reads
   `cx yes`) and `scripts/check-disk.py`.

Its knobs are `WB_CX_WAIT` (30), `WB_CX_ACTS` (4) and
`WB_CX_RUN_TIMEOUT_SEC` (240), plus the shared `WB_QEMU_EXTRA`; the script
refuses any `--acts` other than 4 because its sequence performs exactly four
actions. See [build.md](build.md).

### App-bundle gate

`make test-app-bundles` is gate 56 and has two legs, both required. The first
drives `scripts/run-workbench-app.py` on the same machine setup (an `xhci`
controller, `usb=off,i8042=off`, a hot-plugged `usb-mouse`) through five
steps:

1. boots with `wb.enable=1 wb.wait=$(WB_APP_WAIT) wb.app=1
   wb.acts=$(WB_APP_ACTS)` and requires the registry line
   `CRUGOS: app registry ready (2 app(s), 1 rejected)`, the
   `Applications:` icon and `CRUGOS: wb app ready (2 application(s))`
   before anything is clicked.
2. hot-plugs `mouse0` on `xhci.0`, reads the starting cursor from
   `CRUGOS: gfx ready` and double-clicks the icon at (48,324): two presses
   0.12 s apart, moving the pointer in short steps first.
3. takes a `screendump` and requires the bundle window body at (700,360) to
   be (8,16,24) and its active title bar at (600,335) to be (255,176,0).
4. double-clicks the first row at (600,358) and requires
   `CRUGOS: wb app selected hello (index 0 of 2)`, the launch and reap
   markers, and `CRUGOS: wb app selftest ok (2 click(s), 1 launch(es))`.
5. waits for exit status 33 and the line
   `ok: app bundles and tool types (exit 33)`, then runs
   `scripts/boot-check.py --expect-app=yes` (a flag, not a positional
   argument, so the argument order and the argc range are unchanged; its ok
   line reads `app yes`) and `scripts/check-disk.py`.

The second leg never starts the desktop: `make run CMDLINE_EXTRA="user.test=7"`
boots the ring-3 shell with `/etc/app.rc`, which runs `app list`, `app info
orbit` and `app run orbit`, and the kernel requires the shell's exit code 9
(`CRUGOS: user app ok (exit 9)`). It runs `boot-check.py` with the
`EXPECT_USER` through `EXPECT_FORKBIG` drivers and then `check-disk.py` as
well; see [build.md](build.md).

Its knobs are `WB_APP_WAIT` (30), `WB_APP_ACTS` (1),
`WB_APP_RUN_TIMEOUT_SEC` (240) and `APP_RUN_TIMEOUT_SEC` (300), plus the
shared `WB_QEMU_EXTRA`; the driver refuses any `--acts` other than 1 because
its sequence launches exactly one bundle. See [apps.md](apps.md) for the
manifest format and the registry itself.

### Package gate

`make test-workbench-pkg` is gate 59. Like the app gate it drives
`scripts/run-workbench-pkg.py` on a machine with an `xhci` controller, no PS/2
(`usb=off,i8042=off`) and a hot-plugged `usb-mouse`, this time over a QMP
socket, in six steps:

1. boots with `wb.enable=1 wb.wait=$(WB_PKG_WAIT) wb.pkg=1 wb.acts=3`
   and requires `CRUGOS: wb desktop ready` and `CRUGOS: wb pkg ready`
   before it touches the pointer.
2. hot-plugs `mouse0` on `xhci.0` and reads the starting cursor from
   `CRUGOS: gfx ready`, moving in steps of at most 100 pixels so the 8-bit
   HID reports do not overflow.
3. takes a `screendump` and requires the window frame at (40,330) to be
   (255,255,255) and zero amber pixels in the body scan region
   (44,360)-(456,526): the window must start with no `installed` line.
4. presses the body at (250,430) three times: the first press installs
   `captest`, the second removes it, the third installs it again. It waits
   for each serial line in turn and retries the first press once, so the run
   tolerates a lost click, and it requires the removal line to appear in the
   log, which is what proves the press is a toggle and not an idempotent
   install.
5. takes a second `screendump` and requires amber (255,176,0) pixels in the
   same region, then the serial lines
   `CRUGOS: wb pkg installed /System/packages/captest.crpkg`,
   `CRUGOS: wb pkg removed captest`,
   `CRUGOS: wb pkg selftest ok (2 install(s), 1 remove(s), 1 package(s))` and
   `CRUGOS: wb selftest ok`, and waits for exit status 33.
6. runs `scripts/boot-check.py --expect-wbpkg=yes` (a flag, not a positional
   argument, so the argument order and the argc range are unchanged; its ok
   line reads `wbpkg yes`) and `scripts/check-disk.py`, and prints
   `ok: workbench package toggle (2 installs, 1 remove, exit 33)`.

Its knobs are `WB_PKG_WAIT` (30) and `WB_PKG_RUN_TIMEOUT_SEC` (240), plus the
shared `WB_QEMU_EXTRA`. See [packages.md](packages.md) for the format and
[build.md](build.md) for the target itself.

## Interactive demo

`make demo` boots the whole Workbench in a real QEMU window instead of a
headless gate: `DEMO_QEMU_EXTRA` plugs a keyboard and a mouse into the xHCI
controller before boot, `DEMO_DISPLAY` (GTK) opens the window, and
`DEMO_CMDLINE` carries `gpu.enable=1 gpu.size=$(DEMO_GPU_SIZE) wb.enable=1
wb.wait=0 wb.desk=1 wb.dr=1 wb.dsel=1 wb.screens=1 wb.cx=1 wb.shell=1
wb.fill=$(DEMO_FILL_FILES) wb.hold=$(DEMO_HOLD_SEC)`, i.e. every desktop
feature at once. The window is the virtio-gpu 2D scanout, not firmware
memory: `DEMO_GPU_QEMU_EXTRA` adds `-vga none -device
virtio-gpu-pci,id=gpu0`, so there is no GOP framebuffer and the log starts with
`CRUGOS: virtio-gpu ready (1024x768, 768 backing page(s), resource 1, modern
pci)` and `CRUGOS: gpu scanout ok`; `DEMO_GPU=0` boots the software
framebuffer instead. The desktop
appears once `CRUGOS: wb desktop ready` is printed, the ready markers for the
object, the drawer modes, the screens, the Exchange and the Shell follow, and
`wb_hold()` then polls input once per timer tick until the deadline, so all
four windows stay on screen and the pointer and the keyboard are live (pointer
motion prints `CRUGOS: usb mouse move`, a title-bar press starts a drag,
`F2`/`F3`/`F4`/`F6` fire the commodities). Double-clicking the `System:` object
opens its drawer and the fill runs the first time; because the Shell owns the
keyboard, the drawer's arrow keys are inert and its rows and scrollbar are
driven by the mouse. After the hold the guest prints
`CRUGOS: wb hold finished`, exits with status 33 and leaves
`build/x86_64/serial.log` behind. See [build.md](build.md).

## Limits

- Disabled by default: the desktop is enabled by `wb.enable=1` (the gates and
  `make demo` pass it, a normal boot does not) and the demo windows are
  hardcoded. There are no application tasks, no Workbench userspace and no
  window content owned by another program yet.
- Three screens at most, and a screen is never destroyed or resized. Every
  window still lives in one kernel-wide table, so window ids are global rather
  than per screen, and the menu bar and the screen strip are shared by every
  screen. Each screen is a region of the same framebuffer, not a separate
  Workbench process: a crash or a block in the Workbench takes down or stalls
  every screen, and per-screen processes are future work. Icons are per screen
  but, like windows, they live in global tables.
- Four commodities at most in a fixed table, with no persistence across
  boots: the registry is rebuilt every boot and a commodity stays enabled until
  a row click disables it. Hotkeys are global to the desktop rather than per
  window, the exchange exists only while the Workbench runs, the four built-in
  actions are fixed (no plugin loading), and there is no way to add, remove or
  rename a commodity from the desktop. The exchange is off unless `wb.cx=1`.
- The `Applications:` window shows the boot-time registry and nothing more: eight
  bundles at most, only one launch in flight at a time, and no way to install or
  remove one from the desktop. The icon is a colour plus a text label rather
  than icon art, and a rejected bundle is simply absent from the list -- only the
  serial log names the reason. See [apps.md](apps.md).
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
  per arrow key and refreshes on `r`, and since phase 6H a click selects a row
  and the scrollbar thumb can be dragged. The selection is single: there is no
  marquee, no Shift-click and no multi-select, and a refresh paints no selection
  instead of restoring the previous name. The track has no page-step buttons (a
  press pages the view and grabs the thumb), and the arrow keys still move the
  viewport rather than a row cursor, so keyboard row navigation is not wired to
  the drawer yet. Rows sit on a 20-pixel pitch with 8x16 glyphs, so a taller
  window shows more of them and a resize reflows the list. Only one drawer
  window exists at a time. There is one painter slot per window, so a second
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
