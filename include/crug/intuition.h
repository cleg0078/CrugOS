/* intuition.device: windows, activation, title-bar dragging (phase 6A).
   Windows are software rendered on the GOP framebuffer by the primitives in
   fb.h; the device owns the window list, the stacking order and the drag
   state, and is driven by gfx.device's pointer hook. */
#ifndef CRUG_INTUITION_H
#define CRUG_INTUITION_H

#include <crug/types.h>
#include <crug/input.h>

#define INTUITION_MAX_WINDOWS 4
#define INTUITION_TITLE_HEIGHT 20u
#define INTUITION_BORDER 2u
#define INTUITION_MIN_WIDTH 120u
#define INTUITION_MIN_HEIGHT 80u

/* Workbench palette, 0x00RRGGBB. */
#define INTUITION_DESKTOP_RGB 0x00102030u
#define INTUITION_WINDOW_RGB 0x00081018u
#define INTUITION_FRAME_RGB 0x00FFFFFFu
#define INTUITION_TITLE_ACTIVE_RGB 0x00FFB000u
#define INTUITION_TITLE_IDLE_RGB 0x00808080u
#define INTUITION_TITLE_TEXT_RGB 0x00000000u
#define INTUITION_BODY_TEXT_RGB 0x00E0E0E0u

void intuition_init(void);
/* Open a window; returns its id (1..INTUITION_MAX_WINDOWS) or -1 when the
   window table is full or the framebuffer is absent. */
int intuition_open(i32 x, i32 y, u32 w, u32 h, const char *title,
                   const char *body);
int intuition_close(int id);
/* Repaint the whole desktop: background, then the windows bottom to top. */
void intuition_redraw(void);
/* Consume one pointer report: activate, start or continue a title-bar drag. */
void intuition_pointer_event(const struct input_pointer *event);
int intuition_window_rect(int id, i32 *x, i32 *y, u32 *w, u32 *h);
int intuition_active(void);
void intuition_set_reporting(int on);
u64 intuition_drag_count(void);
u32 intuition_window_count(void);

#endif /* CRUG_INTUITION_H */
