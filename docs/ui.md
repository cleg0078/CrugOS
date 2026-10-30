# The user interface: themes, the canvas and the wallpaper

Phase UI stage A (`v0.49.0`) is the first stage of the specification's
user-interface phase.  Until stage A the desktop's colours and sizes were
`#define` constants baked into `kernel/gfx/intuition.c`, the window chrome was
a handful of hand-placed rectangles and the desktop background was one flat
colour.  Stage A replaces them with a theme table, gives the framebuffer a small
compositing canvas the window manager draws through, and paints a procedural
sunset wallpaper and procedural icons behind the desktop.  Everything is still
software rendered into the same framebuffer the console uses; there is no GPU
compute and no alpha-composited font.

The default theme is the light `glass` theme.  `wb.theme=classic` restores
the phase 6 look, and gate 69 proves it is still pixel for pixel what phase 6
painted.  `make test-theme-glass` reads the glass palette out of a QMP
screendump (gate 70) and `make test-wallpaper` proves the sunset follows the
screen size (gate 71).  The older desktop gates append `wb.theme=classic` to
their command line, so their phase 6 pixel assertions keep meaning what they
meant.

## The theme system

A theme is two tables in `include/crug/theme.h`, both filled by
`kernel/gfx/theme.c`: a palette of colours and a set of metrics.  Everything
the desktop paints is read from them.

The palette (`struct theme_palette`) is a list of plain `0x00RRGGBB`
colours:

- `desktop`, `window`, `frame`, `frame_highlight`
- `title_active_top`, `title_active_bottom`, `title_active_text`
- `title_idle_top`, `title_idle_bottom`, `title_idle_text`
- `body_text`, `accent`
- `menubar`, `menu_bg`, `menu_text`, `menu_highlight`
- `gadget`, `gadget_pressed`, `gadget_text`
- `icon`, `icon_inner`, `icon_text`, `icon_selected`
- `sidebar`, `toolbar`, `list_selection`
- `terminal_bg`, `terminal_fg`, `terminal_prompt`
- `dock_fill`, `dock_border`, `dock_hover` (reserved for stage B)
- `cursor_border`, `cursor_body`

The metrics (`struct theme_metrics`) are the pixel sizes the chrome is built
from: `title_height`, `border`, `radius`, `gadget_size`,
`menubar_height`, `menu_item_height`, `icon_width` and `icon_height`,
`icon_label_gap`, `depth_size`, `front_size`, `shadow_depth`,
`shadow_strength` and `shadow_floor`, `screen_pad`,
`screen_glyph_w` and `dock_height`.

Two themes exist.  `THEME_ID_CLASSIC` (0) is the phase 6 palette and geometry;
`THEME_ID_GLASS` (1) is the light theme the desktop boots with;
`THEME_COUNT` is 2.  `theme_get()` returns the active theme,
`theme_by_id()` one of the two, `theme_set()` switches, `theme_id()` and
`theme_name()` report the active one, and `theme_parse(text, len)` accepts
exactly `classic` or `glass` and returns -1 for anything else.

The important colours:

| field | classic | glass |
| --- | --- | --- |
| desktop | `#102030` | `#1B2A3A` (only when the wallpaper is off) |
| window body | `#081018` | `#F4F4F5` |
| frame | `#FFFFFF` | `#8A8A8F` |
| active title | `#FFB000` flat | `#FFFFFF` to `#E4E6E8` |
| active title text | `#000000` | `#1E1E22` |
| idle title | `#808080` flat | `#F7F7F7` to `#E9E9E9`, text `#8E8E93` |
| body text | `#E0E0E0` | `#23232A` |
| accent and selection | `#FFB000` | `#ECBF46` |
| menu bar | `#C0C0C0` | `#D9D9D9` |
| gadget | `#A0A0A0` | `#EFEFF1`, pressed `#D3D3D8` |
| icon | `#C0C0C0` | `#EAEAEC`, inner `#FFFFFF` |
| terminal | `#000000` on `#E0E0E0` | `#000000` on `#E8E8E8` |

And the metrics that differ:

| metric | classic | glass |
| --- | --- | --- |
| title_height | 20 | 24 |
| border | 2 | 1 |
| radius | 0 | 3 |
| gadget_size | 12 | 14 |
| menubar_height | 18 | 24 |
| menu_item_height | 18 | 20 |
| icon_width x icon_height | 64 x 48 | 48 x 48 |
| depth_size / front_size | 12 / 12 | 14 / 14 |
| shadow depth / strength / floor | 0 / 0 / 0 | 2 / 96 / 24 |
| screen_pad | 6 | 8 |
| dock_height | 0 | 56 (reserved, no dock yet) |

`kernel/gfx/intuition.c` did not have to change its call sites for this.
The old `INTUITION_*` colour and size names still exist, but they are now
macros that expand to `theme_get()->palette.` or `theme_get()->metrics.`, so
the paint code kept its shape and the classic theme reproduces the phase 6
pixels because every classic entry equals the old constant.  Adding a theme is
therefore a table edit, not a paint edit.

The theme is chosen once, during desktop bring-up, from `wb.theme=`; there is
no runtime theme switch control yet.  The active theme is printed as
`CRUGOS: theme glass active`.

## The canvas

`include/crug/fb.h` already had the console's primitives (`fb_fill_rect`,
`fb_frame_rect`, `fb_draw_text`).  Phase UI adds the ones the theme chrome
needs:

- `fb_clip_push(x, y, w, h)`, `fb_clip_pop()`, `fb_clip_reset()` and
  `fb_clip_depth()` manage a four-deep clip stack (`FB_CLIP_MAX` 4).  A pushed
  rectangle is intersected with the screen and with the rectangle on top, and
  may be empty, in which case drawing into it is skipped.
- `fb_blend_rect(x, y, w, h, rgb, alpha)` fills a rectangle with a source
  colour at an alpha of 0 to 255.
- `fb_round_rect(x, y, w, h, radius, fill, border)` draws a rounded rectangle;
  a zero `fill` or `border` skips that part.
- `fb_vgradient_rect(x, y, w, h, top_rgb, bottom_rgb)` draws a vertical ramp
  from the top row to the bottom row.
- `fb_shadow_box(x, y, w, h, depth)` draws the soft black shadow that extends
  to the right and below a window box.
- `fb_blit(x, y, w, h, src, stride_px)` copies a pixel block whose stride is
  given in pixels; `fb_blit_keyed()` does the same and skips the key colour.
- `fb_draw_text_transparent(x, y, text, fg)` draws the glyph bits only, with
  no opaque background.
- `fb_draw_text_clipped(x, y, text, fg, bg, max_w)` draws text in at most
  `max_w` pixels and turns the tail into `...` when it does not fit.
- `fb_text_width(text)` returns the width the full string would need.
- `fb_sample(x, y, &rgb)` reads one pixel back and returns -1 outside the
  display.

Every primitive is software.  Colours are `0x00RRGGBB` and are translated to
the display format inside `fb_store()`, so a caller never sees the device
order.  Each primitive clips against the screen and the clip stack, restores
the software cursor before it writes and recomposites it afterwards, so drawing
can never leave a hole where the pointer was; with `gpu.cursor=hw` the device
cursor ops own the pointer and the save is a no-op.

The damage model is phase 8A's.  `fb_store()` folds every written pixel into
one damage rectangle, and each primitive ends with `fb_present_flush()`;
`intuition_redraw()` brackets a whole repaint with `fb_present_begin()` and
`fb_present_end()`, so a repaint is one transfer and one flush however many
primitives it used.  Outside such a bracket (the console, for example) each
call flushes on its own.

Two pure helpers do the arithmetic the primitives share and are host-tested on
their own:

- `kernel/gfx/color.c`: `color_pack` / `color_unpack` (device order
  `CRUG_COLOR_RGBX` or `CRUG_COLOR_BGRX`), `color_blend` and
  `color_blend_device` (alpha 0 returns the destination, `CRUG_COLOR_ALPHA_MAX`
  or more returns the source, otherwise the rounded 8-bit mix), and
  `color_lerp` (a ramp from `from_rgb` to `to_rgb` over `index` of
  `count`, exact at both ends).
- `kernel/gfx/paint.c`: `paint_round_inset(radius, dy)` returns how many
  pixels the row `dy` of a rounded corner is inset, `paint_gradient_rgb`
  is the colour of one gradient row, and
  `paint_shadow_alpha(band, depth, strength, floor)` walks an alpha ramp from
  `strength` at the box edge down to `floor` at band `depth`.

## The glass chrome

Glass keeps the phase 6 window layout and changes what it is made of.

- Title bar: a 24-pixel bar with a `#FFFFFF` to `#E4E6E8` vertical
  gradient, a one-pixel white highlight along its top edge, transparent 8x16
  title text in `#1E1E22`, and a 10x10 rounded accent square before the
  title.  The classic bar is a 20-pixel solid amber bar with black text.
- Gadgets: flat 14x14 buttons (`#EFEFF1`, pressed `#D3D3D8`) with thin
  dividers between them; from left to right they are zoom, send-to-front,
  depth and close.  The close gadget is an X, zoom a square, front a triangle
  and depth a bar, all drawn with the canvas primitives.  Classic keeps its
  12x12 gadgets.
- Frame and body: a one-pixel `#8A8A8F` rounded rectangle (radius 3) around a
  `#F4F4F5` body, with a two-pixel soft shadow to the right and below it
  (alpha 96 next to the box, fading to 24).  Classic keeps a two-pixel white
  frame and no shadow.
- Menu bar: a 24-pixel `#D9D9D9` bar with a one-pixel highlight line; an open
  menu is a white rounded box with a border and 20-pixel rows, highlighted in
  `#ECBF46` under the pointer.
- Desktop icons: the 48x48 procedural icon (see below), an accent selection
  ring when selected, and the label drawn in white on a black plate at alpha
  96 so it reads over the wallpaper.  Classic keeps the 64x48 icons and its own
  colours.

The zoom gadget is new in stage A.  It sits left of the send-to-front gadget,
saves the window rectangle, fills the screen with the window on the next press,
prints `CRUGOS: wb window N zoomed`, and queues
`INTUITION_EVENT_RESIZE`.  Pressing it again restores the saved rectangle.
It is drawn and hit-tested only in the glass theme
(`kernel/gfx/intuition.c`:433-448 and the hit test at :1527), so the classic
theme stays the phase 6 window manager.

## Wallpaper

`include/crug/wallpaper.h` knows two kinds: `WALLPAPER_OFF` (0) and
`WALLPAPER_SUNSET` (1).  `wallpaper_parse(text, len)` accepts exactly
`off` or `sunset`, `wallpaper_name()` returns the name, and the header's zero
value (`WALLPAPER_OFF`) is the default.  The desktop chooses: glass turns the
sunset on, classic keeps it off unless `wb.wallpaper=sunset` asks for it
explicitly.

The picture is generated, not decoded.  `wallpaper_render()` allocates one
full-screen 32-bit buffer through `pmm_alloc_pages()` because a 1280x800
picture is 4 MiB and the kernel heap's large-block path stops at 1 MiB; the
buffer lives for the rest of the run and `wallpaper_draw()` re-blits it
(returning 1 when it drew).  If the allocation fails the desktop prints
`CRUGOS: wallpaper unavailable, desktop colour kept` and falls back to the
theme's desktop colour.

The sunset is a few ramps and two discs, evaluated per pixel by
`wallpaper_sunset_rgb(x, y, w, h)`:

- The horizon is 58% of the height.  The sky above it is three ramps:
  `#10182E` to `#785078` over the first 45% of the sky, on to `#E89060`
  at 78%, and on to `#FFC878` at the horizon.  The sea below is one ramp from
  `#463C60` at the horizon to `#0C1020` at the bottom.
- The sun sits at 72% of the width, seven percent of the screen height above
  the horizon, with a radius of 9% of the height (at least four pixels).  Its
  disc is `#FFF0C0` with a `#FFD890` rim four pixels deep, and a halo of alpha
  70 fades to nothing at twice the radius.
- A ringed planet sits at 15% of the width and 21% of the height with a radius
  of 10% of the height; its ring is an ellipse roughly six times as wide as
  it is tall.
- A soft reflection column under the sun covers the top 18% of the sea and
  fades downwards.

There is no image decoding, no animation and no clock; the wallpaper is painted
once and re-blitted.  Gate 71 boots the same code at 1152x864 and proves the
picture is generated against the real screen size rather than a cached 1280x800
bitmap.

## Icons

`include/crug/icons.h` has three kinds: `ICONS_VOLUME` (0),
`ICONS_DRAWER` (1) and `ICONS_FILE` (2).  `icons_draw(kind, x, y,
size)` scales a 48-pixel master (`ICONS_SIZE`) down or up to any size of at
least eight pixels.

- A volume is a grey box (`#D8D8DE`) with a white label slot, a green LED
  (`#4CC86C`) and two dark feet.  The desktop's `System:` disk uses it.
- A drawer is an amber folder (`#ECBF46`) with a lighter top edge
  (`#FFE49A`), a darker bottom (`#B08818`) and a lighter inner face.
- A file is a white sheet with a folded corner (`#C8C8D0`) and three grey
  text lines (`#787880`).

All three are rectangles drawn through `fb_fill_rect()`, scaled by
`value * size / 48`.  No bitmap, no image file and no decoder is involved.

## Tokens

Both tokens are read with the other desktop tokens during bring-up.

| token | values | default |
| --- | --- | --- |
| `wb.theme=` | `glass`, `classic` | `glass` |
| `wb.wallpaper=` | `off`, `sunset` | `sunset` with glass, `off` with classic |

The guest announces what it chose:

```
CRUGOS: theme glass
CRUGOS: theme glass active
CRUGOS: wallpaper sunset
CRUGOS: wallpaper sunset 1280x800
```

The first line is the token echo, printed where the token is parsed; the
`active` line comes from `wb_init()` after `intuition_init()`; the
wallpaper lines come from the token echo and from `wallpaper_render()` once
the buffer exists, with the size it allocated.  A bad value is ignored, the
default stays and the guest says so:

```
CRUGOS: theme ignored (use wb.theme=classic|glass)
CRUGOS: wallpaper ignored (use wb.wallpaper=off|sunset)
```

## Gates 69 to 71

All three gates drive `scripts/run-theme.py`: it boots the Workbench on a
virtio-gpu scanout with `wb.enable=1 wb.wait=0 wb.hold=20 wb.theme=<t>
wb.wallpaper=<w>`, waits for `CRUGOS: wb desktop ready`, takes a QMP
screendump of `gpu0`, checks pixels and then lets the guest continue; they
also run `scripts/boot-check.py` with `--expect-theme=` and
`--expect-wallpaper=`.

- `make test-theme-classic` (gate 69) boots `wb.theme=classic
  wb.wallpaper=off`.  It requires the title bar pixel at (160, 96) to be the
  amber `(255,176,0)` or the grey `(128,128,128)`, the frame at (80, 150)
  to be exactly `(255,255,255)`, the desktop at the bottom right to be
  exactly `(16,32,48)`, the wallpaper marker `off` and no sunset buffer.
- `make test-theme-glass` (gate 70) boots `wb.theme=glass
  wb.wallpaper=sunset`.  It requires the body pixel at (200, 150) within 6 of
  `(244,244,245)`, the border at (80, 150) within 6 of `(138,138,143)`,
  the title pixel at (160, 92) brighter than the one at (160, 110) with every
  channel at least 200, and the menu bar at (600, 10) within 6 of
  `(217,217,217)`.  It also runs the wallpaper checks below.
- `make test-wallpaper` (gate 71) boots the glass theme on a 1152x864 device
  (`THEME_WALLPAPER_QEMU_EXTRA`).  The guest's
  `CRUGOS: wallpaper sunset 1152x864` must match the scanout size, the sky
  pixel at the top quarter must be brighter than the sea pixel near the bottom,
  the sky near the horizon must be warm (blue below red) and the corner pixel
  must differ from the flat theme desktop colour.

The knobs are `THEME_WB_HOLD` (20, how long the guest holds the finished
desktop still for the screendump), `THEME_RUN_TIMEOUT_SEC` (300, shared by
the three gates) and `THEME_WALLPAPER_QEMU_EXTRA` (the 1152x864 virtio-gpu
command line).  The suite is 71 gates.

A screendump is the only judge here.  A guest counter cannot prove that the
right pixels reached the scanout, and a wrong pixel is exactly the kind of bug
this stage could introduce.

## Limits

- Text is still the baked 8x16 monospace font.  There is no scalable font, no
  alpha-composited text and no text shaping; the theme only changes colours and
  the sizes around the text.
- The wallpaper is procedural, not a bitmap.  There is no image decoder and no
  way to supply a picture at boot.
- The icons are procedural too; there is no icon set on disk and no bitmap
  icon path.
- Glass and classic are the only themes, and both are compiled in.  There is no
  theme file, no partial override and no runtime switch.
- Stage A does not add a dock, widgets or windows beyond the existing
  Workbench and Shell.  `dock_height` and the dock colours are reserved for
  stage B; nothing draws a dock yet.
- The desktop is single-buffered.  A primitive batch becomes one damage
  rectangle and one transfer, but a slow transfer can still tear.
- The theme tables cost `.text` and `.rodata`: the kernel now loads at
  1025378 B (1001 KiB) and files at 372000 B (363 KiB), about 23 KiB under the
  1 MiB stretch goal, so the next stage has little room for code.
- `make test-host` covers the pure helpers (colour arithmetic, paint
  geometry, the theme tables and the wallpaper generator) and reports 8833
  checks; the gates cover the rendered result on the scanout.
