/* intuition.device: windows, menus, gadgets and the event queue (phase 6A/6B).
   Windows are software rendered on the GOP framebuffer by the primitives in
   fb.h; the device owns the window list, the stacking order, the menu bar,
   the open menu, the gadgets and the pointer state machine, and is driven by
   gfx.device's pointer hook.  Inputs that the desktop itself does not consume
   are delivered to a small event queue that the caller drains with
   intuition_event_poll(). */
#ifndef CRUG_INTUITION_H
#define CRUG_INTUITION_H

#include <crug/types.h>
#include <crug/input.h>

#define INTUITION_MAX_WINDOWS 4
#define INTUITION_MAX_MENUS 2
#define INTUITION_MAX_ITEMS 6
#define INTUITION_MAX_GADGETS 4
#define INTUITION_EVENT_QUEUE 16
#define INTUITION_TITLE_HEIGHT 20u
#define INTUITION_BORDER 2u
#define INTUITION_MIN_WIDTH 120u
#define INTUITION_MIN_HEIGHT 80u
#define INTUITION_MENU_BAR_HEIGHT 18u
#define INTUITION_MENU_ITEM_HEIGHT 18u
#define INTUITION_MENU_ITEM_WIDTH 150u

/* Workbench palette, 0x00RRGGBB. */
#define INTUITION_DESKTOP_RGB 0x00102030u
#define INTUITION_WINDOW_RGB 0x00081018u
#define INTUITION_FRAME_RGB 0x00FFFFFFu
#define INTUITION_TITLE_ACTIVE_RGB 0x00FFB000u
#define INTUITION_TITLE_IDLE_RGB 0x00808080u
#define INTUITION_TITLE_TEXT_RGB 0x00000000u
#define INTUITION_BODY_TEXT_RGB 0x00E0E0E0u
#define INTUITION_MENUBAR_RGB 0x00C0C0C0u
#define INTUITION_MENU_BG_RGB 0x00C0C0C0u
#define INTUITION_MENU_TEXT_RGB 0x00000000u
#define INTUITION_MENU_HIGHLIGHT_RGB 0x00FFB000u
#define INTUITION_GADGET_RGB 0x00A0A0A0u
#define INTUITION_GADGET_PRESSED_RGB 0x00606060u
#define INTUITION_GADGET_TEXT_RGB 0x00000000u

/* One screen-top menu: a title and its items.  The strings are owned by the
   caller and must stay valid while the window is open (intuition.device
   copies the table itself). */
struct intuition_menu {
    const char *title;
    u32 item_count;
    const char *items[INTUITION_MAX_ITEMS];
};

/* Event types delivered to the queue. */
#define INTUITION_EVENT_NONE 0u
#define INTUITION_EVENT_ACTIVATE 1u /* a window was activated */
#define INTUITION_EVENT_DRAG 2u     /* a title-bar drag finished */
#define INTUITION_EVENT_MENU 3u     /* a menu item was selected */
#define INTUITION_EVENT_GADGET 4u   /* a body gadget was clicked */
#define INTUITION_EVENT_CLOSE 5u    /* the close gadget was clicked */

struct intuition_event {
    u32 type;
    int window; /* window id, 0 when the event has none */
    int index;  /* menu index or gadget index, -1 when unused */
    u32 item;   /* selected menu item index, 0 when unused */
    i32 x;      /* window origin when the event was generated */
    i32 y;
};

void intuition_init(void);
/* Open a window; returns its id (1..INTUITION_MAX_WINDOWS) or -1 when the
   window table is full or the framebuffer is absent. */
int intuition_open(i32 x, i32 y, u32 w, u32 h, const char *title,
                   const char *body);
int intuition_close(int id);
/* Attach up to INTUITION_MAX_MENUS screen-top menus to a window.  They are
   shown in the menu bar while the window is active. */
int intuition_set_menus(int id, const struct intuition_menu *menus, u32 count);
/* Add a labelled gadget inside the window body (body-relative coordinates);
   returns the gadget index or -1. */
int intuition_add_gadget(int id, i32 x, i32 y, u32 w, u32 h,
                         const char *label);
/* Install a body painter: the callback receives the body rectangle in screen
   coordinates and owns every pixel inside it (phase 6C renders the shell text
   grid this way).  A NULL callback restores the static body text. */
int intuition_set_paint(int id,
                        void (*fn)(int id, i32 x, i32 y, u32 w, u32 h,
                                   void *ctx),
                        void *ctx);
/* Repaint the whole desktop: background, windows, menu bar, open menu. */
void intuition_redraw(void);
/* Repaint one window only; cheap enough for a keystroke. */
void intuition_redraw_window(int id);
/* Consume one pointer report: menus, gadgets, close gadget, activation and
   title-bar dragging. */
void intuition_pointer_event(const struct input_pointer *event);
/* Pop one queued event; returns 1 when an event was copied to out. */
int intuition_event_poll(struct intuition_event *out);
int intuition_window_rect(int id, i32 *x, i32 *y, u32 *w, u32 *h);
int intuition_active(void);
void intuition_set_reporting(int on);
u64 intuition_drag_count(void);
u32 intuition_window_count(void);
u32 intuition_event_count(void);
u32 intuition_event_dropped(void);
/* Menu selections, gadget clicks and close-gadget clicks. */
u64 intuition_action_count(void);
u32 intuition_menu_opens(void);
u32 intuition_gadget_clicks(void);
u64 intuition_close_count(void);
/* Open menu: index within the active window's menu table, or -1. */
int intuition_menu_open(void);
int intuition_menu_hover(void);

#endif /* CRUG_INTUITION_H */
