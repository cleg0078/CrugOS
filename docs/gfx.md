# Graphics: the framebuffer console and the pointer cursor

CrugOS has no GPU driver yet. The console writes glyphs straight into the GOP
framebuffer that the loader hands over in `boot_info`, and phase 5D adds a
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
- One 12x19 sprite, software composited. No hardware cursor, no double
  buffering, no damage tracking: a move costs one restore plus one composite.
- No GPU driver, no 2D acceleration, no windowing: the framebuffer has exactly
  one text console and one sprite today.
- Relative motion only, no acceleration curve, no wheel scrolling of the
  console, no click handling beyond recording the button mask.
- The sprite is clipped at the screen edge; there is no wrap-around.
