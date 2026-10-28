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
/* Phase 6D: the resize grip in the inner bottom-right corner of a window,
   and how far one arrow key moves the active window. */
#define INTUITION_RESIZE_SIZE 12u
#define INTUITION_KEY_STEP 8u

/* Phase 6E: desktop objects (disks, drawers and files as icons) and the depth
   gadget that sends a window to the back of the stacking order. */
#define INTUITION_MAX_ICONS 8
#define INTUITION_ICON_WIDTH 64u
#define INTUITION_ICON_HEIGHT 48u
#define INTUITION_ICON_LABEL_GAP 4u
#define INTUITION_ICON_SELECTED_BORDER_RGB 0x00FFB000u
/* The depth gadget in a title bar, four pixels left of the close gadget. */
#define INTUITION_DEPTH_SIZE 12u

/* What a desktop object represents; the Workbench decides what a press on it
   means, intuition.device only reports the icon index. */
#define INTUITION_ICON_VOLUME 0u
#define INTUITION_ICON_DRAWER 1u
#define INTUITION_ICON_FILE 2u

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
#define INTUITION_ICON_RGB 0x00C0C0C0u
#define INTUITION_ICON_INNER_RGB 0x00081018u
#define INTUITION_ICON_TEXT_RGB 0x00E0E0E0u

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
#define INTUITION_EVENT_RESIZE 6u   /* a resize-grip drag finished */
#define INTUITION_EVENT_ICON 7u     /* a desktop icon was pressed */
#define INTUITION_EVENT_DEPTH 8u    /* a window was sent to the back */

struct intuition_event {
    u32 type;
    int window; /* window id, 0 when the event has none */
    int index;  /* menu index or gadget index, -1 when unused */
    u32 item;   /* selected menu item index, 0 when unused */
    i32 x;      /* window origin when the event was generated */
    i32 y;
    u32 w;      /* new window size, resize events only */
    u32 h;
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
/* Add a desktop object (phase 6E): a labelled icon drawn on the background
   under every window, hit tested only where no window covers it.  The label
   is drawn under the box and the string is caller owned.  Returns the icon
   index or -1 when the table is full or the framebuffer is absent. */
int intuition_add_icon(i32 x, i32 y, const char *label, u32 kind);
int intuition_icon_rect(int index, i32 *x, i32 *y, u32 *w, u32 *h);
/* Index of the selected desktop object, or -1 when none is selected. */
int intuition_icon_selected(void);
u32 intuition_icon_count(void);
u64 intuition_icon_clicks(void);
/* Windows sent to the back through their depth gadget. */
u64 intuition_depth_count(void);
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
/* Consume one decoded key: Tab cycles the window focus, the arrow keys move
   the active window by INTUITION_KEY_STEP pixels, and Enter/Escape/Down/Up
   drive the menu popup (phase 6D). */
void intuition_key_event(u8 key);
/* Pop one queued event; returns 1 when an event was copied to out. */
int intuition_event_poll(struct intuition_event *out);
int intuition_window_rect(int id, i32 *x, i32 *y, u32 *w, u32 *h);
int intuition_active(void);
void intuition_set_reporting(int on);
u64 intuition_drag_count(void);
/* Resize-grip drags that changed a window size. */
u64 intuition_resize_count(void);
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
