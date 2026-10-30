# Graphics: the framebuffer console and the pointer cursor

The console path is software. The console writes glyphs straight into the GOP
framebuffer that the loader hands over in `boot_info` (since phase 8A the same
pixels can also be presented on a virtio-gpu scanout; see [gpu.md](gpu.md)), and
phase 5D adds a
small pointer sprite on top of it: `gfx.device` turns the relative motion that
the phase 5B USB HID boot mouse reports into cursor movement, and the console
composites the sprite around its text so neither overwrites the other.

```
   USB HID boot mouse (drivers/usb/xhci.c)
            |  usb_hid_mouse_decode()
            v
      input.device pointer state (kernel/input/input.c)
            |  input_pointer_read()
            v
        gfx.device (kernel/gfx/gfx.c)
            |  fb_cursor_show_at()
            v
   framebuffer console (kernel/arch/x86_64/fb.c)
```

## The sprite

`include/crug/fb.h` fixes the sprite at 12x19 pixels. The shape is a table in
`kernel/arch/x86_64/fb.c`: `0` is transparent, `1` is the white outline and
`2` is the amber body (`0x00ffd040`). 71 pixels are body and 49 are outline,
which is what the hotplug gate counts in its screendumps.

The console owns the memory underneath the sprite:

- `fb_cursor_composite()` saves the pixels it is about to cover into
  `g_cursor_saved[][]` and then writes the border and body colours.
- `fb_cursor_restore()` writes the saved pixels back and clears the
  "drawn" flag.
- Every text path restores, writes and composites again: `fb_putc()` is a thin
  wrapper around `fb_putc_raw()`, and `fb_clear()` / `fb_scroll()` do the same
  around their own work. A scrolling console therefore never smears the sprite
  and the sprite never eats a glyph.

Pixel packing is the one detail that is easy to get wrong.
`CRUG_PIXEL_BGRX8888` stores blue in the lowest byte, so the natural
`(r << 16) | (g << 8) | b` packing matches it and `CRUG_PIXEL_RGBX8888` needs the
swap. Phase 5D found the polarity inverted (the console had only ever used
greys, so nothing noticed); the amber sprite is what exposed it, and the gate
compares the framebuffer colour, not just the coordinates, so the encoding stays
honest.

## The canvas

Phase UI stage A turns the framebuffer into a small compositing canvas.
`include/crug/fb.h` keeps the original fill, text and cursor calls and adds:

| primitive | purpose |
| --- | --- |
| `fb_clip_push(x, y, w, h)` / `fb_clip_pop()` / `fb_clip_reset()` | a four-deep clip stack (`FB_CLIP_MAX` 4); every primitive intersects with the top rectangle and `fb_clip_depth()` reports the depth |
| `fb_blend_rect(x, y, w, h, rgb, alpha)` | alpha-blended fill, alpha 0 to `CRUG_COLOR_ALPHA_MAX` (255) |
| `fb_round_rect(x, y, w, h, radius, fill, border)` | rounded rectangle; a zero `fill` or `border` skips that part |
| `fb_vgradient_rect(x, y, w, h, top, bottom)` | vertical gradient, one colour per row |
| `fb_shadow_box(x, y, w, h, depth)` | the soft drop shadow under a window |
| `fb_blit(x, y, w, h, src, stride_px)` / `fb_blit_keyed(..., key_rgb)` | copy a pixel block, with a colour key for transparent pixels |
| `fb_draw_text_clipped(x, y, text, fg, bg, max_w)` | text that ends in `...` when it does not fit |
| `fb_draw_text_transparent(x, y, text, fg)` | glyph bits only, no opaque background |
| `fb_text_width(text)` | the pixel width of a string |
| `fb_sample(x, y, &rgb)` | read one pixel back, -1 outside the screen |

Everything is software. A primitive writes through `fb_store()`, whose pixels
fold into the single damage rectangle phase 8A transfers to the GPU, so a batch
of primitives in one repaint is still one transfer and one flush. Each call also
saves and restores the software cursor around its own work, so a primitive can
never leave a hole where the pointer was. Colours are `0x00RRGGBB` and are
translated to the device format inside `fb_store()`; the two pure helpers,
`kernel/gfx/color.c` (pack, unpack, blend, lerp) and `kernel/gfx/paint.c`
(rounded-corner inset, gradient colour, shadow alpha), do the arithmetic that
the primitives share and are host-tested on their own.

The palette itself is not in `fb.c`: `kernel/gfx/theme.c` owns the theme
tables and `kernel/gfx/intuition.c` reads them through macros, so the canvas
draws whatever the current theme asks for. See [ui.md](ui.md) for the tables.

## gfx.device

`include/crug/gfx.h` exposes:

| function | meaning |
| --- | --- |
| `int gfx_init(void)` | require a ready framebuffer, centre the cursor, report `CRUGOS: gfx ready (WxH, cursor at X,Y)` |
| `void gfx_cmdline(const boot_info *bi)` | parse `gfx.wait=` and `gfx.moves=` |
| `void gfx_pointer_poll(void)` | drain `input_pointer_read()` into cursor motion and buttons |
| `void gfx_show(int visible)` | composite or hide the sprite |
| `int gfx_selftest(void)` | wait for the pointer reports a gate asked for |
| `u64 gfx_move_count(void)` | pointer reports that moved the cursor |
| `const struct gfx_cursor *gfx_cursor_state(void)` | current `x`, `y`, `visible`, `moves`, `buttons` |

Motion is one pixel per input unit (`GFX_CURSOR_STEP`), clamped to
`[0, width-1]` and `[0, height-1]`. A report with no motion (a wheel-only event)
does not count as a move; a button transition updates `buttons` and prints
`CRUGOS: gfx pointer buttons 0xNN` in gate mode.

`gfx_selftest()` is silent unless the command line asked for work: with no
`gfx.wait=` and no `gfx.moves=` it returns immediately, so every gate that does
not care about graphics boots exactly as before. With a request it shows the
cursor, prints `CRUGOS: gfx cursor shown at (X, Y)`, pumps
`input_poll()` + `gfx_pointer_poll()` until `gfx.moves` pointer reports arrived
or `gfx.wait` seconds passed, and prints one `CRUGOS: gfx cursor at (X, Y)` line
per move. After success it lingers for 1.5 s so the gate's last screendump is not
racing the rest of the boot to the QEMU exit.

## Command line

| token | default | meaning |
| --- | --- | --- |
| `gfx.wait=SECONDS` | 0 (off) | how long the boot self-test waits for pointer motion, capped at 120 |
| `gfx.moves=N` | 0 (off) | how many pointer reports the self-test wants, capped at 16 |

The loader hands the raw `cmdline.txt` contents over, trailing newline included,
so the tokeniser treats \n and \r as separators as well as spaces and tabs.

## Boot integration

`kernel/core/main.c` calls `gfx_init(); gfx_cmdline(bi);` after
`usb_init(); usb_cmdline(bi);`, and `kernel/core/selftest.c` appends
`if (!gfx_selftest()) panic("selftest: gfx");` after the audio block.

| marker | when |
| --- | --- |
| `CRUGOS: gfx framebuffer absent` | no GOP framebuffer was handed over |
| `CRUGOS: gfx ready (1024x768, cursor at 512,384)` | init found the framebuffer |
| `CRUGOS: gfx cursor shown at (512, 384)` | the self-test composited the sprite |
| `CRUGOS: gfx cursor at (572, 424)` | one pointer report moved the cursor (gate mode) |
| `CRUGOS: gfx pointer buttons 0x01` | a button transition (gate mode) |
| `CRUGOS: gfx pointer timeout (moves 1/3)` | the wait expired first |
| `CRUGOS: gfx selftest ok (3 move(s), buttons 0x00)` | the requested motion arrived |

## make test-gfx

The gate is a real hotplug and a real frame comparison:

1. QEMU boots with `MACHINE_EXTRA=usb=off,i8042=off` and
   `QEMU_EXTRA="-qmp unix:...,server=on,wait=off -device qemu-xhci,id=xhci"`, so
   no keyboard or PS/2 controller exists and QMP can drive the guest directly.
2. The script waits for `CRUGOS: usb controller ready`, then `device_add`s a
   `usb-mouse` on `xhci.0`.
3. It waits for `CRUGOS: usb mouse ready` and `CRUGOS: gfx cursor shown at`,
   then takes a baseline `screendump`.
4. For each requested move it sends relative `input-send-event` motion, waits
   for the guest's `CRUGOS: gfx cursor at (X, Y)` marker, takes another
   screendump and compares the amber centroid of the two images with the guest
   coordinates.
5. `scripts/boot-check.py` then requires the ready/shown/ok markers, at least
   two cursor positions and QEMU exit status 33, and `scripts/check-disk.py`
   still runs a clean `e2fsck`.

The centroid tolerance is 2.0 pixels, which absorbs the sprite's asymmetric
pixel mass at the screen edges; the coordinate check itself is exact.

## Host tests

`make test-host` links `kernel/gfx/gfx.c` against a fake framebuffer and the
real `input.device`. Because the kernel's idle path is `sti; hlt; cli`,
`include/crug/cpu.h` substitutes `host_cpu_wait_for_irq()` under
`CRUG_HOST_TEST`, which advances the shared fake clock by 100 ms so the
self-test linger and timeout loops terminate instead of sleeping. The suite
covers init without a framebuffer, centring, show/hide, clamping at all four
edges, wheel-only reports, button transitions, the silent no-gate path, the
`gfx.wait`/`gfx.moves` parser (including the trailing newline and the caps) and
both self-test outcomes.

## Limits

- Polled only: the console asks for a character, `usb_poll()` runs, and the
  cursor moves at human typing rates. There is no idle loop and no interrupt
  driven refresh, so motion only advances while the console is polled.
- One 12x19 sprite, software composited and drawn into the scanout by default
  (`gpu.cursor=sw`); `gpu.cursor=hw` hands the pointer to the device's cursor
  plane instead, which a remote viewer may not composite. No double buffering:
  a move costs one restore plus one composite, and the damage rectangle folds
  the result into the next GPU transfer.
- No 2D acceleration: the framebuffer has one text console,
  one sprite and, from phase 6A, the software desktop described in
  [workbench.md](workbench.md) (menus, gadgets and the event queue since
  phase 6B, the shell window since phase 6C, resize grips and keyboard
  navigation since phase 6D, desktop objects with a depth gadget since phase
  6E, and icon dragging, a send-to-front gadget and keyboard focus for the icon
  table since phase 6F, whose `make test-workbench-desk` and
  `make test-workbench-icons` gates check the icon frame, the selected frame,
  the drawer, the stacking-order title bars and a dragged tile in
  `screendump`s, since phase 6G the `System:` drawer scrolls its rows with a
  scrollbar and reflows them when the window is resized, which
  `make test-workbench-drawer` checks in `screendump`s, and since phase 6H its
  rows are click-selectable -- an amber `INTUITION_MENU_HIGHLIGHT_RGB` band on
  the selected row -- and its scrollbar thumb can be dragged, through a generic
  body-input hook, which `make test-workbench-select` checks in the same
  `screendump`s, and since phase 6I Intuition keeps up to three screens, each
  with its own name, background colour, windows and desktop icons, with a screen
  strip at the right end of the menu bar and a gadget that moves a window from
  one screen to another, which `make test-workbench-screens` checks in
  `screendump`s, and since phase 6J four commodity hotkey utilities with an
  Exchange manager window whose rows toggle state, which
  `make test-workbench-cx` checks in `screendump`s).
- Relative motion only, no acceleration curve, no wheel scrolling of the
  console, no click handling beyond recording the button mask.
- The sprite is clipped at the screen edge; there is no wrap-around.
