/* Phase UI: the desktop wallpaper.  Two kinds exist: 'off' falls back to the
   plain theme desktop colour, and 'sunset' is rendered procedurally at boot
   into a full screen buffer that intuition.device blits whenever it repaints
   the desktop.  The buffer comes from the physical allocator: HEAP_MAX_ALLOC
   is one MiB and a 1280x800 screen needs four. */
#ifndef CRUG_WALLPAPER_H
#define CRUG_WALLPAPER_H

#include <crug/types.h>

#define WALLPAPER_OFF 0
#define WALLPAPER_SUNSET 1

/* Parse "off" or "sunset"; returns the kind, or -1 when the text is not a
   wallpaper name. */
int wallpaper_parse(const char *text, u32 len);
const char *wallpaper_name(int kind);

/* Render the active wallpaper for a w x h screen into the off-screen buffer
   and blit it once.  Returns 0 on success, -1 when the buffer cannot be had
   (the desktop then keeps its flat colour). */
int wallpaper_render(int kind, u32 w, u32 h);

/* Paint the desktop: the wallpaper when one was rendered for this size,
   otherwise nothing and the caller fills the theme colour itself.  Returns 1
   when it drew the wallpaper. */
int wallpaper_draw(u32 w, u32 h);

/* The colour the procedural sunset produces at a screen position, 0x00RRGGBB.
   Exposed so the host tests can check the artwork without a framebuffer. */
u32 wallpaper_sunset_rgb(u32 x, u32 y, u32 w, u32 h);

#endif