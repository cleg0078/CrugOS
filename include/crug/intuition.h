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
#include <crug/theme.h>

#define INTUITION_MAX_WINDOWS 4
#define INTUITION_MAX_MENUS 2
#define INTUITION_MAX_ITEMS 6
#define INTUITION_MAX_GADGETS 4
#define INTUITION_EVENT_QUEUE 16
/* Phase UI: the chrome metrics come from the active theme, so the same code
   paints the classic 20 px title bar and the glass 24 px one. */
#define INTUITION_TITLE_HEIGHT (theme_get()->metrics.title_height)
#define INTUITION_BORDER (theme_get()->metrics.border)
#define INTUITION_MIN_WIDTH 120u
#define INTUITION_MIN_HEIGHT 80u
#define INTUITION_MENU_BAR_HEIGHT (theme_get()->metrics.menubar_height)
#define INTUITION_MENU_ITEM_HEIGHT (theme_get()->metrics.menu_item_height)
#define INTUITION_MENU_ITEM_WIDTH 150u
/* Phase 6D: the resize grip in the inner bottom-right corner of a window,
   and how far one arrow key moves the active window. */
#define INTUITION_RESIZE_SIZE 12u
#define INTUITION_KEY_STEP 8u

/* Phase 6E: desktop objects (disks, drawers and files as icons) and the depth
   gadget that sends a window to the back of the stacking order. */
#define INTUITION_MAX_ICONS 8
#define INTUITION_ICON_WIDTH (theme_get()->metrics.icon_width)
#define INTUITION_ICON_HEIGHT (theme_get()->metrics.icon_height)
#define INTUITION_ICON_LABEL_GAP (theme_get()->metrics.icon_label_gap)
#define INTUITION_ICON_SELECTED_BORDER_RGB (theme_get()->palette.icon_selected)
/* The depth gadget in a title bar, four pixels left of the close gadget. */
#define INTUITION_DEPTH_SIZE (theme_get()->metrics.depth_size)
/* Phase UI: the title bar gadgets are theme sized (12 px classic, 14 px
   glass) and the glass chrome rounds its corners by this radius. */
#define INTUITION_GADGET_SIZE (theme_get()->metrics.gadget_size)
/* Phase 6F: the send-to-front gadget, sixteen pixels left of the depth box,
   and how far a pointer must move before an icon press becomes a drag. */
#define INTUITION_FRONT_SIZE (theme_get()->metrics.front_size)
#define INTUITION_ICON_DRAG_MIN 4u

/* Phase 6I: full screens.  Every screen owns its background colour, its own
   desktop objects and its own window stack; only the front screen is drawn.
   A screen gadget at the right end of the menu bar shows every screen name
   (the front one highlighted) and swaps the front screen on a click, exactly
   like the Amiga screen depth arrangement. */
#define INTUITION_MAX_SCREENS 3
#define INTUITION_SCREEN_GLYPH_W (theme_get()->metrics.screen_glyph_w)
#define INTUITION_SCREEN_PAD (theme_get()->metrics.screen_pad)

/* Phase 6H: a body painter can also own its window's pointer input.  The
   callback receives body-relative coordinates and one of these kinds, so a
   list can select a row and drag its own scrollbar without intuition.device
   knowing what is painted there. */
#define INTUITION_BODY_PRESS 0u
#define INTUITION_BODY_MOVE 1u
#define INTUITION_BODY_RELEASE 2u

/* What a desktop object represents; the Workbench decides what a press on it
   means, intuition.device only reports the icon index. */
#define INTUITION_ICON_VOLUME 0u
#define INTUITION_ICON_DRAWER 1u
#define INTUITION_ICON_FILE 2u

/* Workbench palette, 0x00RRGGBB, kept as macros so the chrome code reads the
   same and the active theme decides the values (phase UI).  TITLE_ACTIVE_RGB
   is the flat colour a classic title bar fills with; the glass title bar
   ramps from TITLE_ACTIVE_TOP_RGB to TITLE_ACTIVE_BOTTOM_RGB. */
#define INTUITION_DESKTOP_RGB (theme_get()->palette.desktop)
#define INTUITION_WINDOW_RGB (theme_get()->palette.window)
#define INTUITION_FRAME_RGB (theme_get()->palette.frame)
#define INTUITION_FRAME_HIGHLIGHT_RGB (theme_get()->palette.frame_highlight)
#define INTUITION_TITLE_ACTIVE_RGB (theme_get()->palette.title_active_top)
#define INTUITION_TITLE_ACTIVE_TOP_RGB (theme_get()->palette.title_active_top)
#define INTUITION_TITLE_ACTIVE_BOTTOM_RGB (theme_get()->palette.title_active_bottom)
#define INTUITION_TITLE_IDLE_RGB (theme_get()->palette.title_idle_top)
#define INTUITION_TITLE_IDLE_TOP_RGB (theme_get()->palette.title_idle_top)
#define INTUITION_TITLE_IDLE_BOTTOM_RGB (theme_get()->palette.title_idle_bottom)
#define INTUITION_TITLE_TEXT_RGB (theme_get()->palette.title_active_text)
#define INTUITION_TITLE_TEXT_IDLE_RGB (theme_get()->palette.title_idle_text)
#define INTUITION_BODY_TEXT_RGB (theme_get()->palette.body_text)
#define INTUITION_ACCENT_RGB (theme_get()->palette.accent)
#define INTUITION_MENUBAR_RGB (theme_get()->palette.menubar)
#define INTUITION_MENU_BG_RGB (theme_get()->palette.menu_bg)
#define INTUITION_MENU_TEXT_RGB (theme_get()->palette.menu_text)
#define INTUITION_MENU_HIGHLIGHT_RGB (theme_get()->palette.menu_highlight)
#define INTUITION_GADGET_RGB (theme_get()->palette.gadget)
#define INTUITION_GADGET_PRESSED_RGB (theme_get()->palette.gadget_pressed)
#define INTUITION_GADGET_TEXT_RGB (theme_get()->palette.gadget_text)
#define INTUITION_ICON_RGB (theme_get()->palette.icon)
#define INTUITION_ICON_INNER_RGB (theme_get()->palette.icon_inner)
#define INTUITION_ICON_TEXT_RGB (theme_get()->palette.icon_text)

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
#define INTUITION_EVENT_ICON_MOVE 9u /* a desktop icon was dragged */
#define INTUITION_EVENT_FRONT 10u   /* a window was sent to the front */
#define INTUITION_EVENT_SCREEN 11u  /* the front screen changed, or a window
                                       moved between screens (phase 6I) */

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
/* Icon drags that moved a tile (phase 6F). */
u64 intuition_icon_moves(void);
/* Windows sent to the front through their front gadget (phase 6F). */
u64 intuition_front_count(void);
/* Drive the desktop icon table from the keyboard (phase 6F): Tab,
   Right/Down and Left/Up move the selection, Enter returns 1 when the caller
   should activate the selected object, Escape clears the selection. */
int intuition_icon_key(u8 key);
/* Windows sent to the back through their depth gadget. */
u64 intuition_depth_count(void);
/* Install a body painter: the callback receives the body rectangle in screen
   coordinates and owns every pixel inside it (phase 6C renders the shell text
   grid this way).  A NULL callback restores the static body text. */
int intuition_set_paint(int id,
                        void (*fn)(int id, i32 x, i32 y, u32 w, u32 h,
                                   void *ctx),
                        void *ctx);
/* Install a body input handler (phase 6H): a press inside the window body is
   reported with INTUITION_BODY_PRESS, every pointer move while the button is
   held with INTUITION_BODY_MOVE and the release with INTUITION_BODY_RELEASE,
   all relative to the body rectangle.  A NULL handler restores the plain
   activate-and-raise behaviour; unknown window ids return -1. */
int intuition_set_body_input(int id,
                             void (*fn)(int id, u32 kind, i32 x, i32 y,
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

/* Phase 6I screens.  intuition_init creates screen 0, named "Workbench" with
   the default desktop colour; every window and every desktop object opened
   before the first intuition_add_screen() belongs to it.  Adding a screen
   returns its index (1..INTUITION_MAX_SCREENS-1) or -1 when the table is full,
   the framebuffer is absent or the name is NULL.  The name is caller owned. */
int intuition_add_screen(const char *name, u32 bg_rgb);
/* Target screen for the windows and objects created next; 0 by default. */
int intuition_set_screen(u32 index);
/* Bring one screen to the front.  The windows, objects and background of the
   old screen stay alive and come back untouched when it is fronted again;
   the active window becomes the front screen's topmost window. */
int intuition_screen_switch(u32 index);
/* Move one window to another screen without changing the front screen. */
int intuition_move_window_screen(int id, u32 index);
int intuition_screen_front(void);
int intuition_window_screen(int id);
u32 intuition_screen_count(void);
/* Front-screen swaps and windows that changed screen (phase 6I). */
u64 intuition_screen_switches(void);
u64 intuition_screen_moves(void);

#endif /* CRUG_INTUITION_H */
