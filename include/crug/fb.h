/* Framebuffer cursor: a software sprite composited over the text console. */
#ifndef CRUG_FB_H
#define CRUG_FB_H

#include <crug/types.h>

/* Size of the pointer sprite in pixels. */
#define FB_CURSOR_WIDTH 12u
#define FB_CURSOR_HEIGHT 19u

/* Dimensions of the linear framebuffer chosen by the loader; 0 when the
   firmware supplied no usable mode. */
int fb_dimensions(u32 *width, u32 *height);

/* Show the pointer sprite with its top-left pixel at (x, y), moving it when it
   is already visible. Pixels underneath are saved and restored again by
   fb_cursor_hide(). The console restores and re-composites the sprite around
   any glyph it writes, so text never stays eaten by the cursor. */
void fb_cursor_show_at(u32 x, u32 y);
void fb_cursor_hide(void);
int fb_cursor_visible(void);
u32 fb_cursor_x(void);
u32 fb_cursor_y(void);

/* Restore or recomposite the sprite around direct framebuffer drawing (the
   screen primitives below do this themselves, but a caller that draws a whole
   desktop can bracket its work with these). */
void fb_cursor_begin(void);
void fb_cursor_end(void);

/* The text console shares the framebuffer with the phase 6A desktop.  When the
   desktop owns the screen the console keeps writing to the serial port but
   stops touching the framebuffer, so boot text cannot smear a window. */
void fb_set_text_enabled(int enabled);
int fb_text_enabled(void);

/* Clipped drawing primitives for the Workbench desktop: an opaque rectangle, a
   one-pixel frame and 8x16 text with an explicit background colour.  RGB is
   0x00RRGGBB and is translated to the framebuffer's pixel format internally. */
void fb_fill_rect(u32 x, u32 y, u32 w, u32 h, u32 rgb);
void fb_frame_rect(u32 x, u32 y, u32 w, u32 h, u32 rgb);
void fb_draw_text(u32 x, u32 y, const char *text, u32 fg, u32 bg);

#endif /* CRUG_FB_H */
