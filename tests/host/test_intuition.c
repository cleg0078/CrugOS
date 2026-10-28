/* Host tests for the phase 6A intuition.device window manager
   (kernel/gfx/intuition.c).  The framebuffer primitives are replaced by
   counters in this file, so the tests check the window state machine:
   table limits, size/position clamping, hit testing, activation, raising,
   title-bar dragging and release. */
#include <crug/types.h>
#include <crug/fb.h>
#include <crug/input.h>
#include <crug/intuition.h>
#include <string.h>
#include "test.h"

/* The framebuffer fake (fb_ready/fb_dimensions/fb_cursor_*) lives in
 * tests/host/test_gfx.c; this file adds the drawing primitives intuition.device
 * calls and counts them. */
void test_fb_fake_reset(int ready, u32 width, u32 height);
void test_fb_cursor_set(u32 x, u32 y);

static u32 g_fill_calls;
static u32 g_frame_calls;
static u32 g_text_calls;

void fb_fill_rect(u32 x, u32 y, u32 w, u32 h, u32 rgb)
{
    (void)x; (void)y; (void)w; (void)h; (void)rgb;
    g_fill_calls++;
}

void fb_frame_rect(u32 x, u32 y, u32 w, u32 h, u32 rgb)
{
    (void)x; (void)y; (void)w; (void)h; (void)rgb;
    g_frame_calls++;
}

void fb_draw_text(u32 x, u32 y, const char *text, u32 fg, u32 bg)
{
    (void)x; (void)y; (void)text; (void)fg; (void)bg;
    g_text_calls++;
}

static void test_reset(u32 width, u32 height)
{
    test_fb_fake_reset(width && height, width, height);
    g_fill_calls = 0;
    g_frame_calls = 0;
    g_text_calls = 0;
    intuition_init();
}

static void move_cursor(u32 x, u32 y)
{
    test_fb_cursor_set(x, y);
}

static void pointer(struct input_pointer *p, i32 dx, i32 dy, u32 buttons,
                   u32 changes)
{
    memset(p, 0, sizeof(*p));
    p->dx = dx;
    p->dy = dy;
    p->buttons = buttons;
    p->changes = changes;
}

static void test_intuition_basics(void)
{
    i32 x = 0;
    i32 y = 0;
    u32 w = 0;
    u32 h = 0;

    test_reset(1024, 768);
    CHECK(intuition_active() == -1);
    CHECK(intuition_window_count() == 0u);
    CHECK(intuition_open(80, 90, 360, 220, "Workbench", "body") == 1);
    CHECK(intuition_active() == 1);
    CHECK(intuition_window_count() == 1u);
    CHECK(intuition_window_rect(1, &x, &y, &w, &h) == 0);
    CHECK(x == 80 && y == 90 && w == 360 && h == 220);
    CHECK(intuition_window_rect(99, &x, &y, &w, &h) == -1);
    /* The requested size is clamped up to the 120x80 minimum. */
    CHECK(intuition_open(480, 300, 50, 40, "Shell", NULL) == 2);
    CHECK(intuition_window_rect(2, &x, &y, &w, &h) == 0);
    CHECK(x == 480 && y == 300);
    CHECK(w == INTUITION_MIN_WIDTH && h == INTUITION_MIN_HEIGHT);
    CHECK(intuition_active() == 2);
    /* A window that does not fit is clamped onto the screen. */
    CHECK(intuition_open(2000, 2000, 200, 200, "Edge", NULL) == 3);
    CHECK(intuition_window_rect(3, &x, &y, &w, &h) == 0);
    CHECK(x == 824 && y == 568);
    CHECK(intuition_open(10, 10, 200, 200, "Last", NULL) == 4);
    CHECK(intuition_window_count() == 4u);
    /* The table holds four windows. */
    CHECK(intuition_open(10, 10, 200, 200, "Overflow", NULL) == -1);
    CHECK(intuition_close(3) == 0);
    CHECK(intuition_window_count() == 3u);
    CHECK(intuition_close(3) == -1);
    CHECK(intuition_close(42) == -1);
    /* Closing the active window hands activation back to the top window. */
    CHECK(intuition_close(4) == 0);
    CHECK(intuition_active() == 2);
    /* Every open and close repaints the desktop. */
    CHECK(g_fill_calls > 0u);
    /* Each open and close repaints every window bottom to top: 1+2+3+4 on the
     * four opens, then 3 and 2 as the two windows close, so 15 window draws.
     * Every draw frames the window, its close gadget (phase 6B) and its
     * resize grip (phase 6D). */
    CHECK(g_frame_calls == 45u);
}

static void test_intuition_no_framebuffer(void)
{
    i32 x = 0;
    i32 y = 0;
    u32 w = 0;
    u32 h = 0;

    test_reset(0, 0);
    CHECK(intuition_open(0, 0, 200, 200, "None", NULL) == -1);
    CHECK(intuition_window_count() == 0u);
    CHECK(intuition_window_rect(1, &x, &y, &w, &h) == -1);
    intuition_redraw(); /* must not panic without a screen */
    CHECK(intuition_active() == -1);
}

static void test_intuition_hit_test(void)
{
    struct input_pointer ev;

    test_reset(1024, 768);
    CHECK(intuition_open(100, 100, 300, 300, "One", "body") == 1);
    CHECK(intuition_open(150, 150, 300, 300, "Two", "body") == 2);
    /* A press on window 2's body activates it without starting a drag. */
    move_cursor(200, 250);
    pointer(&ev, 0, 0, 0x01u, 0x01u);
    intuition_pointer_event(&ev);
    CHECK(intuition_active() == 2);
    /* A press on window 1 where window 2 does not cover raises window 1. */
    move_cursor(120, 200);
    pointer(&ev, 0, 0, 0x01u, 0x01u);
    intuition_pointer_event(&ev);
    CHECK(intuition_active() == 1);
    /* A press on the desktop hits nothing and keeps the active window. */
    move_cursor(900, 700);
    pointer(&ev, 0, 0, 0x01u, 0x01u);
    intuition_pointer_event(&ev);
    CHECK(intuition_active() == 1);
    /* A null event is ignored. */
    intuition_pointer_event(NULL);
    CHECK(intuition_active() == 1);
}

static void test_intuition_drag(void)
{
    struct input_pointer ev;
    i32 x = 0;
    i32 y = 0;
    u32 w = 0;
    u32 h = 0;

    test_reset(1024, 768);
    CHECK(intuition_open(100, 100, 300, 300, "One", "body") == 1);
    /* Press inside the title bar: the drag offset is the press position. */
    move_cursor(120, 110);
    pointer(&ev, 0, 0, 0x01u, 0x01u);
    intuition_pointer_event(&ev);
    CHECK(intuition_drag_count() == 0u);
    /* Motion drags the window by the pointer delta. */
    move_cursor(160, 150);
    pointer(&ev, 40, 40, 0x01u, 0x00u);
    intuition_pointer_event(&ev);
    CHECK(intuition_window_rect(1, &x, &y, &w, &h) == 0);
    CHECK(x == 140 && y == 140);
    /* Motion without a button does nothing once the drag ended. */
    pointer(&ev, 0, 0, 0x00u, 0x01u);
    intuition_pointer_event(&ev);
    CHECK(intuition_drag_count() == 1u);
    move_cursor(400, 400);
    pointer(&ev, 240, 250, 0x00u, 0x00u);
    intuition_pointer_event(&ev);
    CHECK(intuition_window_rect(1, &x, &y, &w, &h) == 0);
    CHECK(x == 140 && y == 140);
    CHECK(intuition_drag_count() == 1u);
    /* A second drag is clamped to the screen edges. */
    move_cursor(150, 150);
    pointer(&ev, 0, 0, 0x01u, 0x01u);
    intuition_pointer_event(&ev);
    move_cursor(1023, 767);
    pointer(&ev, 883, 627, 0x01u, 0x00u);
    intuition_pointer_event(&ev);
    CHECK(intuition_window_rect(1, &x, &y, &w, &h) == 0);
    CHECK(x == 724 && y == 468); /* 1024-300, 768-300 */
    move_cursor(0, 0);
    pointer(&ev, -1023, -767, 0x01u, 0x00u);
    intuition_pointer_event(&ev);
    CHECK(intuition_window_rect(1, &x, &y, &w, &h) == 0);
    CHECK(x == 0 && y == 0);
    /* Release ends the drag and counts it. */
    pointer(&ev, 0, 0, 0x00u, 0x01u);
    intuition_pointer_event(&ev);
    CHECK(intuition_drag_count() == 2u);
    /* A press on the body never starts a drag. */
    move_cursor(10, 40);
    pointer(&ev, 0, 0, 0x01u, 0x01u);
    intuition_pointer_event(&ev);
    move_cursor(310, 340);
    pointer(&ev, 300, 300, 0x01u, 0x00u);
    intuition_pointer_event(&ev);
    CHECK(intuition_window_rect(1, &x, &y, &w, &h) == 0);
    CHECK(x == 0 && y == 0);
    pointer(&ev, 0, 0, 0x00u, 0x01u);
    intuition_pointer_event(&ev);
    CHECK(intuition_drag_count() == 2u);
}

static void test_intuition_close_during_drag(void)
{
    struct input_pointer ev;
    i32 x = 0;
    i32 y = 0;
    u32 w = 0;
    u32 h = 0;

    test_reset(1024, 768);
    CHECK(intuition_open(100, 100, 300, 300, "One", "body") == 1);
    move_cursor(110, 110);
    pointer(&ev, 0, 0, 0x01u, 0x01u);
    intuition_pointer_event(&ev);
    CHECK(intuition_close(1) == 0);
    /* The release of a drag whose window vanished is not counted. */
    move_cursor(200, 200);
    pointer(&ev, 90, 90, 0x00u, 0x01u);
    intuition_pointer_event(&ev);
    CHECK(intuition_drag_count() == 0u);
    CHECK(intuition_window_count() == 0u);
    CHECK(intuition_window_rect(1, &x, &y, &w, &h) == -1);
}

static void test_intuition_text_draw(void)
{
    test_reset(640, 480);
    CHECK(intuition_open(10, 10, 200, 200, "Title",
                         "first line\nsecond line") == 1);
    CHECK(g_text_calls >= 3u); /* title + two body lines */
    CHECK(g_fill_calls >= 3u); /* desktop, title bar, body */
}

/* A menu bar press opens the strip under the pointer, hover highlights a row,
 * a press on a row selects it and queues a MENU event, and every table is
 * clamped to the header limits.  Window 1 is at (100, 100) and 300x300, so its
 * menu strips start at x = 4 ('Workbench', 88 px wide) and x = 92 ('Window').
 * Item N of a popup is centred at y = 29 + 18 * N. */
static void test_intuition_menus(void)
{
    static const struct intuition_menu menus[2] = {
        {"Workbench", 3u, {"About", "Info", "Quit"}},
        {"Window", 2u, {"Close", "New"}},
    };
    /* Nine items and five menus are clamped to the INTUITION_MAX_* limits. */
    static const struct intuition_menu big[2] = {
        {"Big", 9u, {"a", "b", "c", "d", "e", "f"}},
        {"More", 2u, {"x", "y"}},
    };
    /* A table entry with no title ends the table instead of being drawn. */
    static const struct intuition_menu untitled[2] = {
        {"One", 1u, {"a"}},
        {NULL, 0u, {NULL}},
    };
    struct input_pointer ev;
    struct intuition_event out;

    test_reset(1024, 768);
    CHECK(intuition_open(100, 100, 300, 300, "One", "body") == 1);
    CHECK(intuition_open(480, 300, 320, 200, "Two", "body") == 2);
    CHECK(intuition_set_menus(99, menus, 2u) == -1);
    CHECK(intuition_set_menus(1, NULL, 2u) == -1);
    CHECK(intuition_set_menus(1, menus, 2u) == 0);
    CHECK(intuition_menu_open() == -1);
    CHECK(intuition_menu_hover() == -1);
    CHECK(intuition_menu_opens() == 0u);

    /* Window 2 is active and has no menus: a press on the bar does nothing. */
    move_cursor(30, 9);
    pointer(&ev, 0, 0, 0x01u, 0x01u);
    intuition_pointer_event(&ev);
    CHECK(intuition_menu_open() == -1);
    CHECK(intuition_menu_opens() == 0u);
    pointer(&ev, 0, 0, 0x00u, 0x01u);
    intuition_pointer_event(&ev);

    /* Press on window 1's body: it activates (queued) and owns the bar. */
    move_cursor(150, 250);
    pointer(&ev, 0, 0, 0x01u, 0x01u);
    intuition_pointer_event(&ev);
    CHECK(intuition_active() == 1);
    CHECK(intuition_event_poll(&out) == 1);
    CHECK(out.type == INTUITION_EVENT_ACTIVATE);
    CHECK(out.window == 1);
    pointer(&ev, 0, 0, 0x00u, 0x01u);
    intuition_pointer_event(&ev);

    /* Pressing between the strips opens no menu. */
    move_cursor(300, 9);
    pointer(&ev, 0, 0, 0x01u, 0x01u);
    intuition_pointer_event(&ev);
    CHECK(intuition_menu_open() == -1);
    pointer(&ev, 0, 0, 0x00u, 0x01u);
    intuition_pointer_event(&ev);

    /* The first strip opens the first menu. */
    move_cursor(30, 9);
    pointer(&ev, 0, 0, 0x01u, 0x01u);
    intuition_pointer_event(&ev);
    CHECK(intuition_menu_open() == 0);
    CHECK(intuition_menu_opens() == 1u);
    CHECK(intuition_menu_hover() == -1);
    pointer(&ev, 0, 0, 0x00u, 0x01u);
    intuition_pointer_event(&ev);

    /* Hovering row 1 highlights it; row 1 is at y = 29 + 18 = 47. */
    move_cursor(40, 47);
    pointer(&ev, 10, 38, 0x00u, 0x00u);
    intuition_pointer_event(&ev);
    CHECK(intuition_menu_hover() == 1);
    /* Moving off the popup clears the highlight. */
    move_cursor(200, 400);
    pointer(&ev, 160, 353, 0x00u, 0x00u);
    intuition_pointer_event(&ev);
    CHECK(intuition_menu_hover() == -1);
    CHECK(intuition_menu_open() == 0);

    /* A press on a row selects it, closes the popup and queues the event. */
    move_cursor(40, 29);
    pointer(&ev, 0, 0, 0x01u, 0x01u);
    intuition_pointer_event(&ev);
    CHECK(intuition_menu_open() == -1);
    CHECK(intuition_menu_hover() == -1);
    CHECK(intuition_action_count() == 1u);
    CHECK(intuition_event_poll(&out) == 1);
    CHECK(out.type == INTUITION_EVENT_MENU);
    CHECK(out.window == 1);
    CHECK(out.index == 0);
    CHECK(out.item == 0u);
    CHECK(intuition_event_poll(&out) == 0);
    /* The release after the selection is consumed and does nothing. */
    pointer(&ev, 0, 0, 0x00u, 0x01u);
    intuition_pointer_event(&ev);
    CHECK(intuition_action_count() == 1u);

    /* The second strip opens the second menu at x = 92. */
    move_cursor(110, 9);
    pointer(&ev, 0, 0, 0x01u, 0x01u);
    intuition_pointer_event(&ev);
    CHECK(intuition_menu_open() == 1);
    CHECK(intuition_menu_opens() == 2u);
    pointer(&ev, 0, 0, 0x00u, 0x01u);
    intuition_pointer_event(&ev);
    move_cursor(110, 29);
    pointer(&ev, 0, 0, 0x01u, 0x01u);
    intuition_pointer_event(&ev);
    CHECK(intuition_action_count() == 2u);
    CHECK(intuition_event_poll(&out) == 1);
    CHECK(out.index == 1);
    CHECK(out.item == 0u);
    pointer(&ev, 0, 0, 0x00u, 0x01u);
    intuition_pointer_event(&ev);

    /* Counts are clamped: five menus become two and nine items become six. */
    CHECK(intuition_set_menus(2, big, 5u) == 0);
    /* Window 2 is not active yet, so raise it before using its menu bar. */
    move_cursor(600, 400);
    pointer(&ev, 0, 0, 0x01u, 0x01u);
    intuition_pointer_event(&ev);
    CHECK(intuition_active() == 2);
    pointer(&ev, 0, 0, 0x00u, 0x01u);
    intuition_pointer_event(&ev);
    CHECK(intuition_event_poll(&out) == 1);
    CHECK(out.type == INTUITION_EVENT_ACTIVATE);
    CHECK(out.window == 2);
    CHECK(intuition_event_poll(&out) == 0);
    move_cursor(30, 9);
    pointer(&ev, 0, 0, 0x01u, 0x01u);
    intuition_pointer_event(&ev);
    CHECK(intuition_menu_open() == 0);
    pointer(&ev, 0, 0, 0x00u, 0x01u);
    intuition_pointer_event(&ev);
    /* Row 5 exists (the last of six): y = 29 + 5 * 18 = 119. */
    move_cursor(30, 119);
    pointer(&ev, 0, 0, 0x00u, 0x00u);
    intuition_pointer_event(&ev);
    CHECK(intuition_menu_hover() == 5);
    /* Row 6 does not: rel = 117 >= 6 * 18, so the press dismisses instead. */
    move_cursor(30, 137);
    pointer(&ev, 0, 0, 0x00u, 0x00u);
    intuition_pointer_event(&ev);
    CHECK(intuition_menu_hover() == -1);
    pointer(&ev, 0, 0, 0x01u, 0x01u);
    intuition_pointer_event(&ev);
    CHECK(intuition_menu_open() == -1);
    CHECK(intuition_action_count() == 2u);
    pointer(&ev, 0, 0, 0x00u, 0x01u);
    intuition_pointer_event(&ev);
    CHECK(intuition_active() == 2);

    /* 'Big' is 3 chars, so 40 px: strip 1 ('More') runs x = 44..84. */
    move_cursor(60, 9);
    pointer(&ev, 0, 0, 0x01u, 0x01u);
    intuition_pointer_event(&ev);
    CHECK(intuition_menu_open() == 1);
    pointer(&ev, 0, 0, 0x00u, 0x01u);
    intuition_pointer_event(&ev);
    CHECK(intuition_action_count() == 2u);

    /* An untitled or empty entry ends the table: only one strip is left, so
     * a press past it opens nothing instead of crashing on a NULL title. */
    CHECK(intuition_set_menus(2, untitled, 2u) == 0);
    move_cursor(110, 9);
    pointer(&ev, 0, 0, 0x01u, 0x01u);
    intuition_pointer_event(&ev);
    CHECK(intuition_menu_open() == -1);
    pointer(&ev, 0, 0, 0x00u, 0x01u);
    intuition_pointer_event(&ev);
}

/* Gadgets are hit in screen coordinates, only count as clicked when the
 * release happens inside the gadget, and every click queues a GADGET event.
 * Window 1 is at (100, 100) with a 2 px border and a 20 px title, so a gadget
 * at body-relative (12, 60) is screen (114, 182). */
static void test_intuition_gadgets(void)
{
    struct input_pointer ev;
    struct intuition_event out;

    test_reset(1024, 768);
    CHECK(intuition_open(100, 100, 300, 300, "One", "body") == 1);
    CHECK(intuition_add_gadget(99, 12, 60, 96, 22, "Run") == -1);
    CHECK(intuition_add_gadget(1, 12, 60, 0, 22, "Run") == -1);
    CHECK(intuition_add_gadget(1, 12, 60, 96, 22, "Run") == 0);
    CHECK(intuition_add_gadget(1, 12, 90, 96, 22, "Stop") == 1);
    /* A gadget wider than the body is cut down to the body width. */
    CHECK(intuition_add_gadget(1, 10, 120, 500, 22, "Wide") == 2);
    CHECK(intuition_add_gadget(1, 10, 150, 96, 22, "Last") == 3);
    CHECK(intuition_add_gadget(1, 10, 180, 96, 22, "Over") == -1);
    CHECK(intuition_gadget_clicks() == 0u);

    /* Press and release on Run: one click, one queued event. */
    move_cursor(162, 193);
    pointer(&ev, 0, 0, 0x01u, 0x01u);
    intuition_pointer_event(&ev);
    CHECK(intuition_gadget_clicks() == 0u);
    pointer(&ev, 0, 0, 0x00u, 0x01u);
    intuition_pointer_event(&ev);
    CHECK(intuition_gadget_clicks() == 1u);
    CHECK(intuition_action_count() == 1u);
    CHECK(intuition_event_poll(&out) == 1);
    CHECK(out.type == INTUITION_EVENT_GADGET);
    CHECK(out.window == 1);
    CHECK(out.index == 0);
    CHECK(intuition_event_poll(&out) == 0);

    /* Press on Run, move off it and release elsewhere: no click. */
    move_cursor(162, 193);
    pointer(&ev, 0, 0, 0x01u, 0x01u);
    intuition_pointer_event(&ev);
    move_cursor(162, 300);
    pointer(&ev, 0, 107, 0x00u, 0x00u);
    intuition_pointer_event(&ev);
    pointer(&ev, 0, 0, 0x00u, 0x01u);
    intuition_pointer_event(&ev);
    CHECK(intuition_gadget_clicks() == 1u);

    /* Stop sits at body-relative y = 90, so screen (150, 220). */
    move_cursor(150, 220);
    pointer(&ev, 0, 0, 0x01u, 0x01u);
    intuition_pointer_event(&ev);
    pointer(&ev, 0, 0, 0x00u, 0x01u);
    intuition_pointer_event(&ev);
    CHECK(intuition_gadget_clicks() == 2u);
    CHECK(intuition_event_poll(&out) == 1);
    CHECK(out.index == 1);
    CHECK(intuition_event_poll(&out) == 0);

    /* A press outside every gadget is a body click and queues nothing. */
    move_cursor(300, 400);
    pointer(&ev, 0, 0, 0x01u, 0x01u);
    intuition_pointer_event(&ev);
    pointer(&ev, 0, 0, 0x00u, 0x01u);
    intuition_pointer_event(&ev);
    CHECK(intuition_gadget_clicks() == 2u);
}

/* The 12x12 close box at the right end of the title bar closes its window,
 * queues a CLOSE event, counts as one action and hands activation back to the
 * top remaining window.  Window 2 at (480, 300) and 320x200 puts it at
 * (780, 306); window 1 at (100, 100) and 300x300 at (380, 106). */
static void test_intuition_close_gadget(void)
{
    struct input_pointer ev;
    struct intuition_event out;

    test_reset(1024, 768);
    CHECK(intuition_open(100, 100, 300, 300, "One", "body") == 1);
    CHECK(intuition_open(480, 300, 320, 200, "Two", "body") == 2);
    CHECK(intuition_active() == 2);

    move_cursor(786, 312);
    pointer(&ev, 0, 0, 0x01u, 0x01u);
    intuition_pointer_event(&ev);
    CHECK(intuition_close_count() == 1u);
    CHECK(intuition_window_count() == 1u);
    CHECK(intuition_window_rect(2, NULL, NULL, NULL, NULL) == -1);
    CHECK(intuition_active() == 1);
    CHECK(intuition_action_count() == 1u);
    CHECK(intuition_event_poll(&out) == 1);
    CHECK(out.type == INTUITION_EVENT_CLOSE);
    CHECK(out.window == 2);
    /* The release after the closing press does nothing. */
    pointer(&ev, 0, 0, 0x00u, 0x01u);
    intuition_pointer_event(&ev);
    CHECK(intuition_close_count() == 1u);
    CHECK(intuition_event_poll(&out) == 0);

    /* A press just outside the box starts a title-bar drag instead. */
    move_cursor(370, 112);
    pointer(&ev, 0, 0, 0x01u, 0x01u);
    intuition_pointer_event(&ev);
    CHECK(intuition_close_count() == 1u);
    pointer(&ev, 0, 0, 0x00u, 0x01u);
    intuition_pointer_event(&ev);
    CHECK(intuition_drag_count() == 1u);
    CHECK(intuition_window_count() == 1u);

    /* Closing the last window leaves no active window and no window at all. */
    move_cursor(386, 112);
    pointer(&ev, 0, 0, 0x01u, 0x01u);
    intuition_pointer_event(&ev);
    CHECK(intuition_close_count() == 2u);
    CHECK(intuition_window_count() == 0u);
    CHECK(intuition_active() == -1);
    CHECK(intuition_action_count() == 2u);
}

/* The resize grip lives in the inner bottom-right corner of every window:
 * a 12x12 box at (x + w - BORDER - 12 - 2, y + h - BORDER - 12 - 2).
 * Dragging it grows or shrinks the window within the 120x80 minimum and the
 * screen bounds; the release queues a RESIZE event with the new size. */
static void test_intuition_resize(void)
{
    struct input_pointer ev;
    struct intuition_event out;
    i32 x = 0;
    i32 y = 0;
    u32 w = 0;
    u32 h = 0;

    test_reset(1024, 768);
    CHECK(intuition_open(100, 100, 300, 300, "One", "body") == 1);
    CHECK(intuition_resize_count() == 0u);
    /* The grip of the 300x300 window starts at (384, 384). */
    move_cursor(390, 390);
    pointer(&ev, 0, 0, 0x01u, 0x01u);
    intuition_pointer_event(&ev);
    /* Dragging the grip by (30, 30) grows the window to 330x330. */
    move_cursor(420, 420);
    pointer(&ev, 30, 30, 0x01u, 0x00u);
    intuition_pointer_event(&ev);
    CHECK(intuition_window_rect(1, &x, &y, &w, &h) == 0);
    CHECK(x == 100 && y == 100 && w == 330 && h == 330);
    /* Motion that would push the window past the screen edge is refused,
       leaving the size untouched. */
    move_cursor(1023, 767);
    pointer(&ev, 603, 347, 0x01u, 0x00u);
    intuition_pointer_event(&ev);
    CHECK(intuition_window_rect(1, &x, &y, &w, &h) == 0);
    CHECK(w == 330 && h == 330);
    pointer(&ev, 0, 0, 0x00u, 0x01u);
    intuition_pointer_event(&ev);
    CHECK(intuition_resize_count() == 1u);
    CHECK(intuition_event_poll(&out) == 1);
    CHECK(out.type == INTUITION_EVENT_RESIZE);
    CHECK(out.window == 1);
    CHECK(out.w == 330 && out.h == 330);
    CHECK(out.x == 100 && out.y == 100);
    /* Shrinking below the 120x80 minimum clamps to it. */
    move_cursor(418, 418); /* grip of the 330x330 window at (414, 414) */
    pointer(&ev, 0, 0, 0x01u, 0x01u);
    intuition_pointer_event(&ev);
    move_cursor(0, 0);
    pointer(&ev, -418, -418, 0x01u, 0x00u);
    intuition_pointer_event(&ev);
    CHECK(intuition_window_rect(1, &x, &y, &w, &h) == 0);
    CHECK(w == INTUITION_MIN_WIDTH && h == INTUITION_MIN_HEIGHT);
    pointer(&ev, 0, 0, 0x00u, 0x01u);
    intuition_pointer_event(&ev);
    CHECK(intuition_resize_count() == 2u);
    CHECK(intuition_event_poll(&out) == 1);
    CHECK(out.type == INTUITION_EVENT_RESIZE);
    CHECK(out.w == INTUITION_MIN_WIDTH && out.h == INTUITION_MIN_HEIGHT);
    /* A press on the grip counts as a resize even when nothing moves. */
    move_cursor(208, 168); /* grip of the 120x80 window */
    pointer(&ev, 0, 0, 0x01u, 0x01u);
    intuition_pointer_event(&ev);
    pointer(&ev, 0, 0, 0x00u, 0x01u);
    intuition_pointer_event(&ev);
    CHECK(intuition_resize_count() == 3u);
    CHECK(intuition_action_count() == 3u);
    CHECK(intuition_event_poll(&out) == 1);
    CHECK(out.type == INTUITION_EVENT_RESIZE);
    /* A body press that is not on the grip never resizes or drags. */
    move_cursor(150, 150);
    pointer(&ev, 0, 0, 0x01u, 0x01u);
    intuition_pointer_event(&ev);
    move_cursor(400, 400);
    pointer(&ev, 250, 250, 0x01u, 0x00u);
    intuition_pointer_event(&ev);
    pointer(&ev, 0, 0, 0x00u, 0x01u);
    intuition_pointer_event(&ev);
    CHECK(intuition_resize_count() == 3u);
    CHECK(intuition_drag_count() == 0u);
}

/* Phase 6D keyboard navigation: Tab cycles the focus (and raises the window),
 * the arrow keys move the active window by INTUITION_KEY_STEP pixels, and
 * Enter/Escape/Down/Up drive the menu popup of the active window. */
static void test_intuition_keys(void)
{
    static const struct intuition_menu menus[1] = {
        {"Workbench", 3u, {"About", "Info", "Quit"}},
    };
    struct intuition_event out;
    i32 x = 0;
    i32 y = 0;
    u32 w = 0;
    u32 h = 0;
    u32 i;

    test_reset(1024, 768);
    CHECK(intuition_open(100, 100, 300, 300, "One", "body") == 1);
    CHECK(intuition_open(500, 100, 300, 300, "Two", "body") == 2);
    CHECK(intuition_set_menus(1, menus, 1u) == 0);
    CHECK(intuition_set_menus(2, menus, 1u) == 0);
    CHECK(intuition_active() == 2);
    /* Tab cycles from window 2 to window 1 and raises it. */
    intuition_key_event('\t');
    CHECK(intuition_active() == 1);
    CHECK(intuition_event_poll(&out) == 1);
    CHECK(out.type == INTUITION_EVENT_ACTIVATE);
    CHECK(out.window == 1);
    /* The arrow keys move the active window by 8 px per press. */
    intuition_key_event(INPUT_KEY_RIGHT);
    intuition_key_event(INPUT_KEY_RIGHT);
    intuition_key_event(INPUT_KEY_DOWN);
    CHECK(intuition_window_rect(1, &x, &y, &w, &h) == 0);
    CHECK(x == 116 && y == 108);
    intuition_key_event(INPUT_KEY_LEFT);
    intuition_key_event(INPUT_KEY_UP);
    CHECK(intuition_window_rect(1, &x, &y, &w, &h) == 0);
    CHECK(x == 108 && y == 100);
    /* The keys stop at the screen edges. */
    for (i = 0; i < 40; i++) {
        intuition_key_event(INPUT_KEY_LEFT);
        intuition_key_event(INPUT_KEY_UP);
    }
    CHECK(intuition_window_rect(1, &x, &y, &w, &h) == 0);
    CHECK(x == 0 && y == 0);
    /* Enter opens the first menu with item 0 highlighted; Down and Up move
       the highlight and Enter selects the highlighted item. */
    CHECK(intuition_menu_open() == -1);
    intuition_key_event('\n');
    CHECK(intuition_menu_open() == 0);
    CHECK(intuition_menu_hover() == 0);
    intuition_key_event(INPUT_KEY_DOWN);
    CHECK(intuition_menu_hover() == 1);
    intuition_key_event(INPUT_KEY_UP);
    CHECK(intuition_menu_hover() == 0);
    intuition_key_event('\n');
    CHECK(intuition_menu_open() == -1);
    CHECK(intuition_event_poll(&out) == 1);
    CHECK(out.type == INTUITION_EVENT_MENU);
    CHECK(out.window == 1);
    CHECK(out.index == 0);
    CHECK(out.item == 0);
    /* Escape closes an open menu without queueing an event. */
    intuition_key_event('\r');
    CHECK(intuition_menu_open() == 0);
    intuition_key_event(0x1b);
    CHECK(intuition_menu_open() == -1);
    CHECK(intuition_event_count() == 0u);
    /* Closing every window leaves the keyboard with nothing to focus, and
       the keys are ignored entirely without a framebuffer. */
    CHECK(intuition_close(1) == 0);
    CHECK(intuition_close(2) == 0);
    intuition_key_event('\t');
    intuition_key_event(INPUT_KEY_RIGHT);
    CHECK(intuition_active() == -1);
    test_reset(0, 0);
    intuition_key_event('\t');
    CHECK(intuition_active() == -1);
}
/* The queue is a 16-entry FIFO: actions keep counting after it fills, the
 * extra events are dropped and reported, and polling drains in order. */
static void test_intuition_event_queue(void)
{
    struct input_pointer ev;
    struct intuition_event out;
    u32 i;
    u32 total = INTUITION_EVENT_QUEUE + 4u;

    test_reset(1024, 768);
    CHECK(intuition_open(100, 100, 300, 300, "One", "body") == 1);
    CHECK(intuition_add_gadget(1, 12, 60, 96, 22, "Run") == 0);
    for (i = 0; i < total; i++) {
        move_cursor(162, 193);
        pointer(&ev, 0, 0, 0x01u, 0x01u);
        intuition_pointer_event(&ev);
        pointer(&ev, 0, 0, 0x00u, 0x01u);
        intuition_pointer_event(&ev);
    }
    CHECK(intuition_gadget_clicks() == total);
    CHECK(intuition_action_count() == total);
    CHECK(intuition_event_count() == INTUITION_EVENT_QUEUE);
    CHECK(intuition_event_dropped() == total - INTUITION_EVENT_QUEUE);
    for (i = 0; i < INTUITION_EVENT_QUEUE; i++) {
        CHECK(intuition_event_poll(&out) == 1);
        CHECK(out.type == INTUITION_EVENT_GADGET);
        CHECK(out.window == 1);
    }
    CHECK(intuition_event_count() == 0u);
    CHECK(intuition_event_poll(&out) == 0);
}

void test_intuition_functions(void)
{
    printf("- intuition basics / clamping / limits\n");
    test_intuition_basics();
    printf("- intuition without a framebuffer\n");
    test_intuition_no_framebuffer();
    printf("- intuition hit testing / activation\n");
    test_intuition_hit_test();
    printf("- intuition title-bar drag\n");
    test_intuition_drag();
    printf("- intuition close during drag\n");
    test_intuition_close_during_drag();
    printf("- intuition text rendering calls\n");
    test_intuition_text_draw();
    printf("- intuition menus / hover / selection\n");
    test_intuition_menus();
    printf("- intuition gadgets\n");
    test_intuition_gadgets();
    printf("- intuition close gadget\n");
    test_intuition_close_gadget();
    printf("- intuition resize grip\n");
    test_intuition_resize();
    printf("- intuition keyboard focus / keys / menu popup\n");
    test_intuition_keys();
    printf("- intuition event queue\n");
    test_intuition_event_queue();
}
