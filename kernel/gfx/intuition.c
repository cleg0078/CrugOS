/* intuition.device: a small software window manager (phase 6A).
   Windows are plain rectangles with a title bar and body text.  The device
   keeps up to four of them, renders bottom to top, raises and activates the
   window under a left click, and drags a window by its title bar.  All drawing
   goes through the framebuffer primitives, so the host tests can replace them
   and check the state machine without a framebuffer. */
#include <crug/types.h>
#include <crug/string.h>
#include <crug/kernel.h>
#include <crug/fb.h>
#include <crug/input.h>
#include <crug/intuition.h>

struct window {
    int used;
    int id;
    i32 x;
    i32 y;
    u32 w;
    u32 h;
    const char *title;
    const char *body;
};

static struct window g_win[INTUITION_MAX_WINDOWS];
static int g_order[INTUITION_MAX_WINDOWS]; /* bottom to top, -1 empty */
static u32 g_order_count;
static int g_active;      /* window id or -1 */
static int g_drag;        /* window id being dragged or -1 */
static i32 g_drag_dx;
static i32 g_drag_dy;
static int g_reporting;
static u64 g_drags;
static u32 g_screen_w;
static u32 g_screen_h;

static struct window *win_find(int id)
{
    u32 i;

    for (i = 0; i < INTUITION_MAX_WINDOWS; i++) {
        if (g_win[i].used && g_win[i].id == id) {
            return &g_win[i];
        }
    }
    return NULL;
}

static void win_raise(int id)
{
    u32 i;
    u32 pos = g_order_count;

    for (i = 0; i < g_order_count; i++) {
        if (g_order[i] == id) {
            pos = i;
            break;
        }
    }
    if (pos == g_order_count) {
        return;
    }
    for (i = pos; i + 1u < g_order_count; i++) {
        g_order[i] = g_order[i + 1u];
    }
    g_order[g_order_count - 1u] = id;
}

static void win_unlink(int id)
{
    u32 i;
    u32 pos = g_order_count;

    for (i = 0; i < g_order_count; i++) {
        if (g_order[i] == id) {
            pos = i;
            break;
        }
    }
    if (pos == g_order_count) {
        return;
    }
    for (i = pos; i + 1u < g_order_count; i++) {
        g_order[i] = g_order[i + 1u];
    }
    g_order_count--;
    if (g_order_count < INTUITION_MAX_WINDOWS) {
        g_order[g_order_count] = -1;
    }
}

static int win_hit(i32 px, i32 py)
{
    u32 i;

    for (i = g_order_count; i > 0u; i--) {
        struct window *w = win_find(g_order[i - 1u]);

        if (!w) {
            continue;
        }
        if (px >= w->x && py >= w->y && px < w->x + (i32)w->w &&
            py < w->y + (i32)w->h) {
            return w->id;
        }
    }
    return -1;
}

static void win_clamp(struct window *w)
{
    i32 max_x;
    i32 max_y;

    if (!w) {
        return;
    }
    max_x = (i32)g_screen_w - (i32)w->w;
    max_y = (i32)g_screen_h - (i32)w->h;
    if (max_x < 0) {
        max_x = 0;
    }
    if (max_y < 0) {
        max_y = 0;
    }
    if (w->x > max_x) {
        w->x = max_x;
    }
    if (w->y > max_y) {
        w->y = max_y;
    }
    if (w->x < 0) {
        w->x = 0;
    }
    if (w->y < 0) {
        w->y = 0;
    }
}

static void win_draw_title(struct window *w)
{
    int active = (w->id == g_active);
    u32 rgb = active ? INTUITION_TITLE_ACTIVE_RGB : INTUITION_TITLE_IDLE_RGB;
    u32 tx = (u32)w->x + INTUITION_BORDER + 6u;
    u32 ty = (u32)w->y + INTUITION_BORDER + 2u;

    fb_fill_rect((u32)w->x + INTUITION_BORDER, (u32)w->y + INTUITION_BORDER,
                 w->w - 2u * INTUITION_BORDER, INTUITION_TITLE_HEIGHT, rgb);
    if (w->title) {
        fb_draw_text(tx, ty, w->title, INTUITION_TITLE_TEXT_RGB, rgb);
    }
    /* A small close-ish gadget keeps the title bar visually anchored. */
    fb_fill_rect((u32)w->x + w->w - INTUITION_BORDER - 18u,
                 (u32)w->y + INTUITION_BORDER + 4u, 12u, 12u,
                 active ? INTUITION_WINDOW_RGB : INTUITION_DESKTOP_RGB);
}

static void win_draw_body(struct window *w)
{
    u32 bx = (u32)w->x + INTUITION_BORDER;
    u32 by = (u32)w->y + INTUITION_BORDER + INTUITION_TITLE_HEIGHT;
    u32 bw = w->w - 2u * INTUITION_BORDER;
    u32 bh = w->h - 2u * INTUITION_BORDER - INTUITION_TITLE_HEIGHT;
    u32 line_y = by + 6u;
    const char *line = w->body;

    if (bw == 0u || bh <= INTUITION_TITLE_HEIGHT) {
        return;
    }
    fb_fill_rect(bx, by, bw, bh, INTUITION_WINDOW_RGB);
    while (line && *line && line_y + 16u <= by + bh) {
        char text[48];
        u32 n = 0;

        while (*line && *line != '\n' && n + 1u < sizeof(text)) {
            text[n++] = *line++;
        }
        text[n] = '\0';
        if (*line == '\n') {
            line++;
        }
        fb_draw_text(bx + 6u, line_y, text, INTUITION_BODY_TEXT_RGB,
                     INTUITION_WINDOW_RGB);
        line_y += 20u;
    }
}

static void win_draw(struct window *w)
{
    fb_frame_rect((u32)w->x, (u32)w->y, w->w, w->h, INTUITION_FRAME_RGB);
    win_draw_title(w);
    win_draw_body(w);
}

void intuition_init(void)
{
    u32 i;
    u32 width = 0;
    u32 height = 0;

    for (i = 0; i < INTUITION_MAX_WINDOWS; i++) {
        memset(&g_win[i], 0, sizeof(g_win[i]));
        g_win[i].id = (int)i + 1;
        g_order[i] = -1;
    }
    g_order_count = 0;
    g_active = -1;
    g_drag = -1;
    g_drag_dx = 0;
    g_drag_dy = 0;
    g_reporting = 0;
    g_drags = 0;
    g_screen_w = 0;
    g_screen_h = 0;
    if (fb_dimensions(&width, &height) == 0) {
        g_screen_w = width;
        g_screen_h = height;
    }
}

int intuition_open(i32 x, i32 y, u32 w, u32 h, const char *title,
                   const char *body)
{
    u32 i;

    if (!g_screen_w || !g_screen_h) {
        return -1;
    }
    for (i = 0; i < INTUITION_MAX_WINDOWS; i++) {
        if (!g_win[i].used) {
            struct window *win = &g_win[i];

            if (w < INTUITION_MIN_WIDTH) {
                w = INTUITION_MIN_WIDTH;
            }
            if (h < INTUITION_MIN_HEIGHT) {
                h = INTUITION_MIN_HEIGHT;
            }
            if (w > g_screen_w) {
                w = g_screen_w;
            }
            if (h > g_screen_h) {
                h = g_screen_h;
            }
            win->used = 1;
            win->x = x;
            win->y = y;
            win->w = w;
            win->h = h;
            win->title = title;
            win->body = body;
            win_clamp(win);
            g_order[g_order_count++] = win->id;
            g_active = win->id;
            if (g_reporting) {
                kprintf("CRUGOS: wb window %d opened (%d, %d, %ux%u)\n",
                        win->id, (int)win->x, (int)win->y, (unsigned)win->w,
                        (unsigned)win->h);
            }
            intuition_redraw();
            return win->id;
        }
    }
    return -1;
}

int intuition_close(int id)
{
    struct window *w = win_find(id);

    if (!w) {
        return -1;
    }
    win_unlink(id);
    w->used = 0;
    if (g_drag == id) {
        g_drag = -1;
    }
    if (g_active == id) {
        g_active = g_order_count ? g_order[g_order_count - 1u] : -1;
    }
    intuition_redraw();
    return 0;
}

void intuition_redraw(void)
{
    u32 i;

    if (!g_screen_w || !g_screen_h) {
        return;
    }
    fb_fill_rect(0, 0, g_screen_w, g_screen_h, INTUITION_DESKTOP_RGB);
    for (i = 0; i < g_order_count; i++) {
        struct window *w = win_find(g_order[i]);

        if (w) {
            win_draw(w);
        }
    }
}

void intuition_pointer_event(const struct input_pointer *event)
{
    i32 px;
    i32 py;

    if (!event) {
        return;
    }
    px = (i32)fb_cursor_x();
    py = (i32)fb_cursor_y();
    if (event->changes & 0x01u) {
        if (event->buttons & 0x01u) {
            int id = win_hit(px, py);

            if (id > 0) {
                struct window *w = win_find(id);
                i32 title_y = w->y + (i32)INTUITION_BORDER;

                win_raise(id);
                g_active = id;
                if (py >= title_y &&
                    py < title_y + (i32)INTUITION_TITLE_HEIGHT) {
                    g_drag = id;
                    g_drag_dx = px - w->x;
                    g_drag_dy = py - w->y;
                    if (g_reporting) {
                        kprintf("CRUGOS: wb window %d drag start (%d, %d)\n",
                                id, (int)px, (int)py);
                    }
                }
                if (g_reporting) {
                    kprintf("CRUGOS: wb window %d active\n", id);
                }
                intuition_redraw();
            }
            return;
        }
        if (g_drag > 0) {
            struct window *w = win_find(g_drag);

            if (w && g_reporting) {
                kprintf("CRUGOS: wb window %d moved to (%d, %d)\n", g_drag,
                        (int)w->x, (int)w->y);
            }
            g_drag = -1;
            g_drags++;
        }
        return;
    }
    if (g_drag > 0 && (event->dx != 0 || event->dy != 0)) {
        struct window *w = win_find(g_drag);
        i32 nx;
        i32 ny;

        if (!w) {
            g_drag = -1;
            return;
        }
        nx = px - g_drag_dx;
        ny = py - g_drag_dy;
        if (nx != w->x || ny != w->y) {
            w->x = nx;
            w->y = ny;
            win_clamp(w);
            if (g_reporting) {
                kprintf("CRUGOS: wb window %d drag to (%d, %d)\n", g_drag,
                        (int)w->x, (int)w->y);
            }
            intuition_redraw();
        }
    }
}

int intuition_window_rect(int id, i32 *x, i32 *y, u32 *w, u32 *h)
{
    struct window *win = win_find(id);

    if (!win) {
        return -1;
    }
    if (x) {
        *x = win->x;
    }
    if (y) {
        *y = win->y;
    }
    if (w) {
        *w = win->w;
    }
    if (h) {
        *h = win->h;
    }
    return 0;
}

int intuition_active(void)
{
    return g_active;
}

void intuition_set_reporting(int on)
{
    g_reporting = on ? 1 : 0;
}

u64 intuition_drag_count(void)
{
    return g_drags;
}

u32 intuition_window_count(void)
{
    return g_order_count;
}
