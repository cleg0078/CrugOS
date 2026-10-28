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

#endif /* CRUG_FB_H */
