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
     * four opens, then 3 and 2 as the two windows close. */
    CHECK(g_frame_calls == 15u);
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
}
