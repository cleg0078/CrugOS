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


/* Phase 8A: a present callback receives the damaged rectangle in pixels; the
   virtio-gpu driver uses it to transfer and flush only what changed. */
typedef void (*fb_present_fn)(u32 x, u32 y, u32 w, u32 h);

/* Adopt a linear buffer the caller owns (the GPU resource backing store).
   The present callback may be NULL for a purely local buffer.  Returns 0 on
   success and -1 for an unusable geometry. */
int fb_attach_linear(u8 *base, u32 width, u32 height, u32 stride, u32 format,
                     fb_present_fn present);

/* Flush the accumulated damage rectangle through the present callback.  While
   a begin/end bracket is open, fb_present() is a no-op so a whole desktop
   repaint transfers once at the end instead of once per primitive. */
void fb_present(void);
void fb_present_begin(void);
void fb_present_end(void);

/* Phase 8B: hardware cursor plane.  When the display device can move a cursor
   itself the driver installs these callbacks and the sprite is no longer
   composited into the framebuffer: fb_cursor_show_at() forwards the position
   to the device, and fb_cursor_visible() keeps reporting whether the pointer
   is on screen.  Removing the callbacks hides the device cursor and returns to
   the software sprite. */
struct fb_cursor_ops {
    int (*show)(u32 x, u32 y);
    int (*move)(u32 x, u32 y);
    void (*hide)(void);
};

int fb_set_cursor_ops(const struct fb_cursor_ops *ops);
int fb_cursor_hardware(void);

/* The software sprite, exposed so a device cursor image can be built from the
   same pixels: fb_cursor_sprite() returns 0 for a transparent pixel, 1 for the
   border and 2 for the body; the colours are the RGB values used for them. */
u32 fb_cursor_width(void);
u32 fb_cursor_height(void);
u32 fb_cursor_sprite(u32 x, u32 y);
u32 fb_cursor_border_rgb(void);
u32 fb_cursor_body_rgb(void);
#endif /* CRUG_FB_H */