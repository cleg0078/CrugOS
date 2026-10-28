/* intuition.device: a small software window manager (phase 6A/6B).
   Windows are plain rectangles with a title bar and body text; a window can
   also own screen-top menus and labelled body gadgets.  The device keeps up
   to four windows, renders bottom to top with the menu bar and the open menu
   on top, raises and activates the window under a left click, drags a window
   by its title bar, opens and selects menus, runs gadgets, closes a window
   from its close gadget and queues what it consumed as typed events.  Phase
   6E adds desktop objects (icons drawn under the windows) and the depth gadget
   that sends a window to the back.  All
   drawing goes through the framebuffer primitives, so the host tests can
   replace them and check the state machine without a framebuffer. */
#include <crug/types.h>
#include <crug/string.h>
#include <crug/kernel.h>
#include <crug/fb.h>
#include <crug/input.h>
#include <crug/intuition.h>

struct gadget {
    const char *label;
    i32 x; /* body-relative */
    i32 y;
    u32 w;
    u32 h;
};

struct icon {
    const char *label;
    i32 x;
    i32 y;
    u32 kind;
};

struct window {
    int used;
    int id;
    i32 x;
    i32 y;
    u32 w;
    u32 h;
    const char *title;
    const char *body;
    struct intuition_menu menus[INTUITION_MAX_MENUS];
    u32 menu_count;
    struct gadget gadgets[INTUITION_MAX_GADGETS];
    u32 gadget_count;
    /* Optional body painter (phase 6C): the shell renders its text grid here
       instead of the static body string. */
    void (*paint)(int id, i32 x, i32 y, u32 w, u32 h, void *ctx);
    void *paint_ctx;
    /* Optional body input handler (phase 6H): a body press, the moves while
       the button is held and the release, in body-relative coordinates. */
    void (*body_input)(int id, u32 kind, i32 x, i32 y, void *ctx);
    void *body_input_ctx;
};

static struct icon g_icons[INTUITION_MAX_ICONS];
static u32 g_icon_count;
static int g_icon_sel = -1;
static u64 g_icon_clicks;
/* Phase 6F: icon dragging, the send-to-front gadget and keyboard icon focus. */
static u64 g_icon_moves;
static u64 g_fronts;
static int g_icon_drag = -1;
static i32 g_icon_dx;
static i32 g_icon_dy;
static int g_icon_drag_moved;
static u64 g_depths;

static struct window g_win[INTUITION_MAX_WINDOWS];
static int g_order[INTUITION_MAX_WINDOWS]; /* bottom to top, -1 empty */
static u32 g_order_count;
static int g_active;      /* window id or -1 */
static int g_drag;        /* window id being dragged or -1 */
static i32 g_drag_dx;
static i32 g_drag_dy;
/* Phase 6D: the resize-grip drag, with the size the window had at press. */
static int g_resize;      /* window id being resized or -1 */
static i32 g_resize_px;
static i32 g_resize_py;
static u32 g_resize_w;
static u32 g_resize_h;
static u64 g_resizes;
static int g_reporting;
static u64 g_drags;
static u32 g_screen_w;
static u32 g_screen_h;

static int g_menu_win;   /* window owning the open menu, 0 when closed */
static int g_menu_index; /* index in that window's menu table, -1 closed */
static int g_menu_hover; /* highlighted item index, -1 none */
static u32 g_menu_opens;
static u64 g_actions;
static u32 g_gadget_clicks;
static u64 g_close_count;
static int g_gadget_win; /* window whose gadget is pressed, 0 when none */
static int g_gadget;
/* Phase 6H: the window whose body owns the current pointer press, or -1. */
static int g_body_win = -1;

static struct intuition_event g_events[INTUITION_EVENT_QUEUE];
static u32 g_event_head;
static u32 g_event_count;
static u32 g_event_dropped;

static void event_push(u32 type, int window, int index, u32 item, i32 x,
                       i32 y)
{
    struct intuition_event *e;

    if (g_event_count >= INTUITION_EVENT_QUEUE) {
        g_event_dropped++;
        return;
    }
    e = &g_events[(g_event_head + g_event_count) % INTUITION_EVENT_QUEUE];
    e->type = type;
    e->window = window;
    e->index = index;
    e->item = item;
    e->x = x;
    e->y = y;
    e->w = 0u;
    e->h = 0u;
    g_event_count++;
}

/* Resize events are the only ones that carry a window size: push the event,
   then patch the record that was just queued. */
static void event_push_resize(int id, i32 x, i32 y, u32 w, u32 h)
{
    u32 idx;

    event_push(INTUITION_EVENT_RESIZE, id, -1, 0u, x, y);
    if (g_event_count == 0u) {
        return;
    }
    idx = (g_event_head + g_event_count - 1u) % INTUITION_EVENT_QUEUE;
    g_events[idx].w = w;
    g_events[idx].h = h;
}

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

static u32 menu_strip_width(const struct intuition_menu *m)
{
    return (u32)strlen(m->title) * 8u + 16u;
}

static u32 menu_strip_x(const struct window *w, u32 index)
{
    u32 x = 4u;
    u32 i;

    for (i = 0; i < index && i < w->menu_count; i++) {
        x += menu_strip_width(&w->menus[i]);
    }
    return x;
}

static i32 close_box_x(const struct window *w)
{
    return w->x + (i32)w->w - (i32)INTUITION_BORDER - 18;
}

static int close_hit(const struct window *w, i32 px, i32 py)
{
    i32 cx = close_box_x(w);
    i32 cy = w->y + (i32)INTUITION_BORDER + 4;

    return px >= cx && px < cx + 12 && py >= cy && py < cy + 12;
}

/* The depth gadget (phase 6E): a 12x12 box four pixels left of the close
   gadget.  Clicking it sends the window to the back of the stacking order. */
static i32 depth_box_x(const struct window *w)
{
    return close_box_x(w) - 16;
}

static int depth_hit(const struct window *w, i32 px, i32 py)
{
    i32 cx = depth_box_x(w);
    i32 cy = w->y + (i32)INTUITION_BORDER + 4;

    return px >= cx && px < cx + (i32)INTUITION_DEPTH_SIZE && py >= cy &&
           py < cy + (i32)INTUITION_DEPTH_SIZE;
}

/* The front gadget (phase 6F): a 12x12 box another sixteen pixels to the left.
   Pressing it raises the window above every other window; pressing it on the
   frontmost window changes nothing. */
static i32 front_box_x(const struct window *w)
{
    return close_box_x(w) - 32;
}

static int front_hit(const struct window *w, i32 px, i32 py)
{
    i32 cx = front_box_x(w);
    i32 cy = w->y + (i32)INTUITION_BORDER + 4;

    return px >= cx && px < cx + (i32)INTUITION_FRONT_SIZE && py >= cy &&
           py < cy + (i32)INTUITION_FRONT_SIZE;
}

/* Move one window to the bottom of the stack without touching the others.
   Returns 1 when the stack changed and 0 when the window was already at the
   bottom (or unknown), so a depth press on the backmost window is a no-op. */
static int win_send_to_back(int id)
{
    u32 i;
    u32 pos = g_order_count;

    for (i = 0; i < g_order_count; i++) {
        if (g_order[i] == id) {
            pos = i;
            break;
        }
    }
    if (pos == g_order_count || pos == 0u) {
        return 0;
    }
    for (i = pos; i > 0u; i--) {
        g_order[i] = g_order[i - 1u];
    }
    g_order[0] = id;
    return 1;
}

/* The resize grip: a 12x12 box in the inner bottom-right corner of every
   window.  Dragging it changes the window size, never its origin. */
static void resize_rect(const struct window *w, i32 *gx, i32 *gy, u32 *gw,
                        u32 *gh)
{
    *gx = w->x + (i32)w->w - (i32)INTUITION_BORDER -
          (i32)INTUITION_RESIZE_SIZE - 2;
    *gy = w->y + (i32)w->h - (i32)INTUITION_BORDER -
          (i32)INTUITION_RESIZE_SIZE - 2;
    *gw = INTUITION_RESIZE_SIZE;
    *gh = INTUITION_RESIZE_SIZE;
}

static int resize_hit(const struct window *w, i32 px, i32 py)
{
    i32 gx;
    i32 gy;
    u32 gw;
    u32 gh;

    if (!w) {
        return 0;
    }
    resize_rect(w, &gx, &gy, &gw, &gh);
    return px >= gx && px < gx + (i32)gw && py >= gy &&
           py < gy + (i32)gh;
}

static void gadget_rect(const struct window *w, int index, i32 *gx, i32 *gy,
                        u32 *gw, u32 *gh)
{
    const struct gadget *g = &w->gadgets[index];

    *gx = w->x + (i32)INTUITION_BORDER + g->x;
    *gy = w->y + (i32)INTUITION_BORDER + (i32)INTUITION_TITLE_HEIGHT + g->y;
    *gw = g->w;
    *gh = g->h;
}

static int gadget_contains(const struct window *w, int index, i32 px, i32 py)
{
    i32 gx;
    i32 gy;
    u32 gw;
    u32 gh;

    if (!w || index < 0 || (u32)index >= w->gadget_count) {
        return 0;
    }
    gadget_rect(w, index, &gx, &gy, &gw, &gh);
    return px >= gx && py >= gy && px < gx + (i32)gw &&
           py < gy + (i32)gh;
}

static int gadget_hit(const struct window *w, i32 px, i32 py)
{
    u32 i;

    for (i = 0; i < w->gadget_count; i++) {
        if (gadget_contains(w, (int)i, px, py)) {
            return (int)i;
        }
    }
    return -1;
}

/* Desktop objects (phase 6E): a plain box with a darker inner rectangle and
   the label under it.  A selected icon gets an amber frame and label. */
static void icon_rect_of(int index, i32 *x, i32 *y, u32 *w, u32 *h)
{
    const struct icon *ic = &g_icons[index];

    *x = ic->x;
    *y = ic->y;
    *w = INTUITION_ICON_WIDTH;
    *h = INTUITION_ICON_HEIGHT;
}

static void draw_icon(u32 index)
{
    const struct icon *ic = &g_icons[index];
    int selected = ((int)index == g_icon_sel);
    u32 frame = selected ? INTUITION_ICON_SELECTED_BORDER_RGB
                         : INTUITION_FRAME_RGB;
    u32 label_rgb = selected ? INTUITION_ICON_SELECTED_BORDER_RGB
                             : INTUITION_ICON_TEXT_RGB;

    fb_fill_rect((u32)ic->x, (u32)ic->y, INTUITION_ICON_WIDTH,
                 INTUITION_ICON_HEIGHT, INTUITION_ICON_RGB);
    fb_frame_rect((u32)ic->x, (u32)ic->y, INTUITION_ICON_WIDTH,
                  INTUITION_ICON_HEIGHT, frame);
    fb_fill_rect((u32)ic->x + 8, (u32)ic->y + 8, INTUITION_ICON_WIDTH - 16u,
                 INTUITION_ICON_HEIGHT - 16u, INTUITION_ICON_INNER_RGB);
    if (ic->label) {
        fb_draw_text((u32)ic->x,
                     (u32)ic->y + INTUITION_ICON_HEIGHT +
                         INTUITION_ICON_LABEL_GAP,
                     ic->label, label_rgb, INTUITION_DESKTOP_RGB);
    }
}

static int icon_hit(i32 px, i32 py)
{
    u32 i;

    for (i = 0; i < g_icon_count; i++) {
        const struct icon *ic = &g_icons[i];

        if (px >= ic->x && py >= ic->y &&
            px < ic->x + (i32)INTUITION_ICON_WIDTH &&
            py < ic->y + (i32)INTUITION_ICON_HEIGHT) {
            return (int)i;
        }
    }
    return -1;
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
    /* The close gadget: a small box that ends the title bar. */
    fb_fill_rect((u32)close_box_x(w), (u32)w->y + INTUITION_BORDER + 4u,
                 12u, 12u,
                 active ? INTUITION_WINDOW_RGB : INTUITION_DESKTOP_RGB);
    fb_frame_rect((u32)close_box_x(w), (u32)w->y + INTUITION_BORDER + 4u,
                  12u, 12u, INTUITION_TITLE_TEXT_RGB);
    /* The depth gadget: a second small box with a filled centre. */
    fb_fill_rect((u32)depth_box_x(w), (u32)w->y + INTUITION_BORDER + 4u, 12u,
                 12u, active ? INTUITION_WINDOW_RGB : INTUITION_DESKTOP_RGB);
    fb_frame_rect((u32)depth_box_x(w), (u32)w->y + INTUITION_BORDER + 4u,
                  12u, 12u, INTUITION_TITLE_TEXT_RGB);
    fb_fill_rect((u32)depth_box_x(w) + 4, (u32)w->y + INTUITION_BORDER + 8u,
                 4u, 4u, INTUITION_TITLE_TEXT_RGB);
    /* The front gadget (phase 6F): a box with a small up arrow, which lifts
       this window above every other window. */
    fb_fill_rect((u32)front_box_x(w), (u32)w->y + INTUITION_BORDER + 4u, 12u,
                 12u, active ? INTUITION_WINDOW_RGB : INTUITION_DESKTOP_RGB);
    fb_frame_rect((u32)front_box_x(w), (u32)w->y + INTUITION_BORDER + 4u,
                  12u, 12u, INTUITION_TITLE_TEXT_RGB);
    fb_fill_rect((u32)front_box_x(w) + 4, (u32)w->y + INTUITION_BORDER + 9u,
                 4u, 2u, INTUITION_TITLE_TEXT_RGB);
    fb_fill_rect((u32)front_box_x(w) + 5, (u32)w->y + INTUITION_BORDER + 7u,
                 2u, 2u, INTUITION_TITLE_TEXT_RGB);
}

static void win_draw_gadgets(struct window *w)
{
    u32 i;

    for (i = 0; i < w->gadget_count; i++) {
        i32 gx;
        i32 gy;
        u32 gw;
        u32 gh;
        u32 rgb;

        gadget_rect(w, (int)i, &gx, &gy, &gw, &gh);
        rgb = (g_gadget_win == w->id && g_gadget == (int)i)
                  ? INTUITION_GADGET_PRESSED_RGB
                  : INTUITION_GADGET_RGB;
        fb_fill_rect((u32)gx, (u32)gy, gw, gh, rgb);
        fb_frame_rect((u32)gx, (u32)gy, gw, gh, INTUITION_GADGET_TEXT_RGB);
        if (w->gadgets[i].label) {
            u32 ty = gh > 16u ? (u32)gy + (gh - 16u) / 2u : (u32)gy + 1u;

            fb_draw_text((u32)gx + 6u, ty, w->gadgets[i].label,
                         INTUITION_GADGET_TEXT_RGB, rgb);
        }
    }
}

/* Draw the resize grip after the body so the painter cannot hide it. */
static void win_draw_resize(struct window *w)
{
    i32 gx;
    i32 gy;
    u32 gw;
    u32 gh;

    if (w->w < INTUITION_MIN_WIDTH || w->h < INTUITION_MIN_HEIGHT) {
        return;
    }
    resize_rect(w, &gx, &gy, &gw, &gh);
    fb_frame_rect((u32)gx, (u32)gy, gw, gh, INTUITION_FRAME_RGB);
    fb_fill_rect((u32)(gx + 6), (u32)(gy + 6), 3u, 3u, INTUITION_FRAME_RGB);
}

/* The body rectangle of a window in screen coordinates: everything inside the
   frame and below the title bar.  The painter, the body input handler and the
   static text path all measure a body this way. */
static void body_rect(const struct window *w, i32 *bx, i32 *by, u32 *bw,
                      u32 *bh)
{
    *bx = w->x + (i32)INTUITION_BORDER;
    *by = w->y + (i32)INTUITION_BORDER + (i32)INTUITION_TITLE_HEIGHT;
    *bw = w->w - 2u * INTUITION_BORDER;
    *bh = w->h - 2u * INTUITION_BORDER - INTUITION_TITLE_HEIGHT;
}

static void win_draw_body(struct window *w)
{
    i32 bx;
    i32 by;
    u32 bw;
    u32 bh;
    u32 line_y;
    const char *line = w->body;

    body_rect(w, &bx, &by, &bw, &bh);
    line_y = (u32)by + 6u;

    if (bw == 0u || bh <= INTUITION_TITLE_HEIGHT) {
        return;
    }
    fb_fill_rect((u32)bx, (u32)by, bw, bh, INTUITION_WINDOW_RGB);
    if (w->paint) {
        w->paint(w->id, bx, by, bw, bh, w->paint_ctx);
        win_draw_gadgets(w);
        return;
    }
    while (line && *line && line_y + 16u <= (u32)by + bh) {
        char text[48];
        u32 n = 0;

        while (*line && *line != '\n' && n + 1u < sizeof(text)) {
            text[n++] = *line++;
        }
        text[n] = '\0';
        if (*line == '\n') {
            line++;
        }
        fb_draw_text((u32)bx + 6u, line_y, text, INTUITION_BODY_TEXT_RGB,
                     INTUITION_WINDOW_RGB);
        line_y += 20u;
    }
    win_draw_gadgets(w);
}

static void win_draw(struct window *w)
{
    fb_frame_rect((u32)w->x, (u32)w->y, w->w, w->h, INTUITION_FRAME_RGB);
    win_draw_title(w);
    win_draw_body(w);
    win_draw_resize(w);
}

static void menu_draw_bar(void)
{
    struct window *w = win_find(g_active);
    u32 x = 4u;
    u32 i;

    if (!w || w->menu_count == 0u) {
        return;
    }
    fb_fill_rect(0, 0, g_screen_w, INTUITION_MENU_BAR_HEIGHT,
                 INTUITION_MENUBAR_RGB);
    for (i = 0; i < w->menu_count; i++) {
        u32 bw = menu_strip_width(&w->menus[i]);
        u32 rgb = (g_menu_win == w->id && g_menu_index == (int)i)
                      ? INTUITION_MENU_HIGHLIGHT_RGB
                      : INTUITION_MENUBAR_RGB;

        fb_fill_rect(x, 0, bw, INTUITION_MENU_BAR_HEIGHT, rgb);
        fb_draw_text(x + 8u, 1u, w->menus[i].title, INTUITION_MENU_TEXT_RGB,
                     rgb);
        x += bw;
    }
    fb_fill_rect(0, INTUITION_MENU_BAR_HEIGHT - 1u, g_screen_w, 1u,
                 INTUITION_FRAME_RGB);
}

static void menu_draw_popup(void)
{
    struct window *w = win_find(g_menu_win);
    const struct intuition_menu *m;
    u32 x;
    u32 y = INTUITION_MENU_BAR_HEIGHT;
    u32 h;
    u32 i;

    if (!w || g_menu_index < 0 || (u32)g_menu_index >= w->menu_count) {
        return;
    }
    m = &w->menus[g_menu_index];
    x = menu_strip_x(w, (u32)g_menu_index);
    h = 4u + m->item_count * INTUITION_MENU_ITEM_HEIGHT;
    fb_fill_rect(x, y, INTUITION_MENU_ITEM_WIDTH, h, INTUITION_MENU_BG_RGB);
    fb_frame_rect(x, y, INTUITION_MENU_ITEM_WIDTH, h, INTUITION_FRAME_RGB);
    for (i = 0; i < m->item_count; i++) {
        u32 iy = y + 2u + i * INTUITION_MENU_ITEM_HEIGHT;
        u32 rgb = (g_menu_hover == (int)i) ? INTUITION_MENU_HIGHLIGHT_RGB
                                           : INTUITION_MENU_BG_RGB;

        fb_fill_rect(x + 1u, iy, INTUITION_MENU_ITEM_WIDTH - 2u,
                     INTUITION_MENU_ITEM_HEIGHT, rgb);
        fb_draw_text(x + 10u, iy + 1u, m->items[i], INTUITION_MENU_TEXT_RGB,
                     rgb);
    }
}

static int menu_popup_row(const struct window *w, i32 px, i32 py)
{
    const struct intuition_menu *m;
    u32 x0;
    u32 y0;
    i32 rel;

    if (!w || g_menu_index < 0 || (u32)g_menu_index >= w->menu_count) {
        return -1;
    }
    m = &w->menus[g_menu_index];
    x0 = menu_strip_x(w, (u32)g_menu_index);
    y0 = INTUITION_MENU_BAR_HEIGHT;
    if (px < (i32)x0 || px >= (i32)(x0 + INTUITION_MENU_ITEM_WIDTH)) {
        return -1;
    }
    if (py < (i32)y0) {
        return -1;
    }
    rel = py - (i32)y0 - 2;
    if (rel < 0 || (u32)rel >= m->item_count * INTUITION_MENU_ITEM_HEIGHT) {
        return -1;
    }
    return (int)((u32)rel / INTUITION_MENU_ITEM_HEIGHT);
}

static void menu_close(void)
{
    if (!g_menu_win) {
        return;
    }
    g_menu_win = 0;
    g_menu_index = -1;
    g_menu_hover = -1;
    intuition_redraw();
}

static int menu_open_at(i32 px, i32 py)
{
    struct window *w = win_find(g_active);
    u32 x = 4u;
    u32 i;

    if (!w || w->menu_count == 0u) {
        return 0;
    }
    for (i = 0; i < w->menu_count; i++) {
        u32 bw = menu_strip_width(&w->menus[i]);

        if (px >= (i32)x && px < (i32)(x + bw) && w->menus[i].item_count > 0u) {
            g_menu_win = w->id;
            g_menu_index = (int)i;
            g_menu_hover = -1;
            g_menu_opens++;
            if (g_reporting) {
                kprintf("CRUGOS: wb menu '%s' open (window %d)\n",
                        w->menus[i].title, w->id);
            }
            intuition_redraw();
            return 1;
        }
        x += bw;
    }
    return 0;
}

static void menu_select(struct window *w, u32 row)
{
    const struct intuition_menu *m;

    if (!w || g_menu_index < 0 || (u32)g_menu_index >= w->menu_count) {
        menu_close();
        return;
    }
    m = &w->menus[g_menu_index];
    if (row >= m->item_count || !m->items[row]) {
        menu_close();
        return;
    }
    if (g_reporting) {
        kprintf("CRUGOS: wb menu '%s' selected ('%s')\n", m->title,
                m->items[row]);
    }
    event_push(INTUITION_EVENT_MENU, w->id, g_menu_index, row, w->x, w->y);
    g_actions++;
    menu_close();
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
    g_resize = -1;
    g_resize_px = 0;
    g_resize_py = 0;
    g_resize_w = 0u;
    g_resize_h = 0u;
    g_resizes = 0u;
    g_reporting = 0;
    g_drags = 0;
    g_screen_w = 0;
    g_screen_h = 0;
    g_menu_win = 0;
    g_menu_index = -1;
    g_menu_hover = -1;
    g_menu_opens = 0u;
    g_actions = 0u;
    g_gadget_clicks = 0u;
    g_close_count = 0u;
    g_gadget_win = 0;
    g_gadget = -1;
    g_event_head = 0u;
    g_event_count = 0u;
    g_event_dropped = 0u;
    g_icon_count = 0u;
    g_icon_sel = -1;
    g_icon_clicks = 0u;
    g_icon_moves = 0u;
    g_fronts = 0u;
    g_icon_drag = -1;
    g_icon_dx = 0;
    g_icon_dy = 0;
    g_icon_drag_moved = 0;
    g_depths = 0u;
    for (i = 0; i < INTUITION_MAX_ICONS; i++) {
        memset(&g_icons[i], 0, sizeof(g_icons[i]));
    }
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
            win->menu_count = 0u;
            memset(&win->menus[0], 0, sizeof(win->menus));
            win->gadget_count = 0u;
            memset(&win->gadgets[0], 0, sizeof(win->gadgets));
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
    if (g_menu_win == id) {
        g_menu_win = 0;
        g_menu_index = -1;
        g_menu_hover = -1;
    }
    if (g_gadget_win == id) {
        g_gadget_win = 0;
        g_gadget = -1;
    }
    if (g_active == id) {
        g_active = g_order_count ? g_order[g_order_count - 1u] : -1;
    }
    intuition_redraw();
    return 0;
}

int intuition_set_menus(int id, const struct intuition_menu *menus, u32 count)
{
    struct window *w = win_find(id);
    u32 i;
    u32 j;

    if (!w || !menus) {
        return -1;
    }
    if (count > INTUITION_MAX_MENUS) {
        count = INTUITION_MAX_MENUS;
    }
    memset(&w->menus[0], 0, sizeof(w->menus));
    /* A strip with no title or no items cannot be drawn, so the table ends
     * there: an empty entry is ignored rather than crashing the menu bar. */
    for (i = 0; i < count; i++) {
        if (!menus[i].title || menus[i].item_count == 0u) {
            count = i;
            break;
        }
        w->menus[i].title = menus[i].title;
        w->menus[i].item_count = menus[i].item_count > INTUITION_MAX_ITEMS
                                    ? INTUITION_MAX_ITEMS
                                    : menus[i].item_count;
        for (j = 0; j < w->menus[i].item_count; j++) {
            w->menus[i].items[j] = menus[i].items[j];
        }
    }
    w->menu_count = count;
    if (g_menu_win == id) {
        g_menu_win = 0;
        g_menu_index = -1;
        g_menu_hover = -1;
    }
    intuition_redraw();
    return 0;
}

int intuition_add_gadget(int id, i32 x, i32 y, u32 w, u32 h, const char *label)
{
    struct window *win = win_find(id);
    struct gadget *g;
    u32 body_w;
    u32 body_h;

    if (!win || win->gadget_count >= INTUITION_MAX_GADGETS || !w || !h) {
        return -1;
    }
    body_w = win->w - 2u * INTUITION_BORDER;
    body_h = win->h - 2u * INTUITION_BORDER - INTUITION_TITLE_HEIGHT;
    if (x < 0) {
        x = 0;
    }
    if (y < 0) {
        y = 0;
    }
    if ((u32)x + w > body_w) {
        w = body_w > (u32)x ? body_w - (u32)x : 0u;
    }
    if ((u32)y + h > body_h) {
        h = body_h > (u32)y ? body_h - (u32)y : 0u;
    }
    if (w < 16u || h < 16u) {
        return -1;
    }
    g = &win->gadgets[win->gadget_count];
    g->label = label;
    g->x = x;
    g->y = y;
    g->w = w;
    g->h = h;
    win->gadget_count++;
    intuition_redraw();
    return (int)(win->gadget_count - 1u);
}

int intuition_add_icon(i32 x, i32 y, const char *label, u32 kind)
{
    struct icon *ic;

    if (!g_screen_w || !g_screen_h || !label) {
        return -1;
    }
    if (g_icon_count >= INTUITION_MAX_ICONS) {
        return -1;
    }
    if (x < 0) {
        x = 0;
    }
    if (y < 0) {
        y = 0;
    }
    if ((u32)x + INTUITION_ICON_WIDTH > g_screen_w) {
        x = (i32)g_screen_w - (i32)INTUITION_ICON_WIDTH;
    }
    if ((u32)y + INTUITION_ICON_HEIGHT > g_screen_h) {
        y = (i32)g_screen_h - (i32)INTUITION_ICON_HEIGHT;
    }
    if (x < 0) {
        x = 0;
    }
    if (y < 0) {
        y = 0;
    }
    ic = &g_icons[g_icon_count];
    ic->label = label;
    ic->x = x;
    ic->y = y;
    ic->kind = kind;
    g_icon_count++;
    intuition_redraw();
    return (int)(g_icon_count - 1u);
}

int intuition_icon_rect(int index, i32 *x, i32 *y, u32 *w, u32 *h)
{
    i32 ix;
    i32 iy;
    u32 iw;
    u32 ih;

    if (index < 0 || (u32)index >= g_icon_count) {
        return -1;
    }
    icon_rect_of(index, &ix, &iy, &iw, &ih);
    if (x) {
        *x = ix;
    }
    if (y) {
        *y = iy;
    }
    if (w) {
        *w = iw;
    }
    if (h) {
        *h = ih;
    }
    return 0;
}

int intuition_icon_selected(void)
{
    return g_icon_sel;
}

u32 intuition_icon_count(void)
{
    return g_icon_count;
}

u64 intuition_icon_clicks(void)
{
    return g_icon_clicks;
}

u64 intuition_icon_moves(void)
{
    return g_icon_moves;
}

u64 intuition_front_count(void)
{
    return g_fronts;
}

/* Phase 6F: the desktop icon table takes keyboard focus.  Tab, Right and Down
   walk the table forwards, Left and Up walk backwards, Enter asks the caller to
   activate the selected object and Escape drops the selection. */
int intuition_icon_key(u8 key)
{
    int sel = g_icon_sel;
    int next = sel;

    if (g_icon_count == 0u) {
        return 0;
    }
    if (key == INPUT_KEY_RIGHT || key == INPUT_KEY_DOWN || key == '\t') {
        next = sel < 0 ? 0 : (int)(((u32)sel + 1u) % g_icon_count);
    } else if (key == INPUT_KEY_LEFT || key == INPUT_KEY_UP) {
        next = sel <= 0 ? (int)g_icon_count - 1 : sel - 1;
    } else if (key == '\n' || key == '\r') {
        if (sel < 0) {
            g_icon_sel = 0;
            if (g_reporting) {
                kprintf("CRUGOS: wb icon '%s' focused\n", g_icons[0].label);
            }
            intuition_redraw();
            return 0;
        }
        return 1;
    } else if (key == 0x1b) {
        if (sel >= 0) {
            g_icon_sel = -1;
            intuition_redraw();
        }
        return 0;
    } else {
        return 0;
    }
    if (next != sel) {
        g_icon_sel = next;
        if (g_reporting) {
            kprintf("CRUGOS: wb icon '%s' focused\n", g_icons[next].label);
        }
        intuition_redraw();
    }
    return 0;
}

u64 intuition_depth_count(void)
{
    return g_depths;
}

void intuition_redraw(void)
{
    u32 i;

    if (!g_screen_w || !g_screen_h) {
        return;
    }
    fb_fill_rect(0, 0, g_screen_w, g_screen_h, INTUITION_DESKTOP_RGB);
    for (i = 0; i < g_icon_count; i++) {
        draw_icon(i);
    }
    for (i = 0; i < g_order_count; i++) {
        struct window *w = win_find(g_order[i]);

        if (w) {
            win_draw(w);
        }
    }
    menu_draw_bar();
    menu_draw_popup();
}

/* A press that missed every window: select a desktop object, or clear the
   selection when the background itself was clicked. */
static void icon_press(i32 px, i32 py)
{
    int index = icon_hit(px, py);

    if (index < 0) {
        if (g_icon_sel >= 0) {
            g_icon_sel = -1;
            intuition_redraw();
        }
        return;
    }
    g_icon_sel = index;
    g_icon_clicks++;
    g_actions++;
    if (g_reporting) {
        kprintf("CRUGOS: wb icon '%s' selected\n", g_icons[index].label);
    }
    event_push(INTUITION_EVENT_ICON, 0, index, 0u, g_icons[index].x,
               g_icons[index].y);
    /* Remember where inside the tile the press landed, so motion can drag it. */
    g_icon_drag = index;
    g_icon_dx = px - g_icons[index].x;
    g_icon_dy = py - g_icons[index].y;
    g_icon_drag_moved = 0;
    intuition_redraw();
}

static void press(i32 px, i32 py)
{
    struct window *w;
    int id;
    int row;
    int gi;
    int prev;

    /* An open menu owns the press: select an item, or dismiss and reopen. */
    if (g_menu_win > 0) {
        struct window *mw = win_find(g_menu_win);

        row = menu_popup_row(mw, px, py);
        if (row >= 0 && mw) {
            menu_select(mw, (u32)row);
            return;
        }
        menu_close();
        if (py >= 0 && py < (i32)INTUITION_MENU_BAR_HEIGHT) {
            menu_open_at(px, py);
        }
        return;
    }
    if (py >= 0 && py < (i32)INTUITION_MENU_BAR_HEIGHT) {
        menu_open_at(px, py);
        return;
    }
    id = win_hit(px, py);
    if (id < 0) {
        icon_press(px, py);
        return;
    }
    w = win_find(id);
    if (!w) {
        return;
    }
    if (depth_hit(w, px, py)) {
        int top;

        if (!win_send_to_back(id)) {
            return;
        }
        if (g_active == id) {
            g_active = g_order_count ? g_order[g_order_count - 1u] : -1;
        }
        top = g_active;
        if (g_reporting) {
            kprintf("CRUGOS: wb window %d sent to back\n", id);
        }
        event_push(INTUITION_EVENT_DEPTH, id, -1, 0u, w->x, w->y);
        if (top > 0) {
            struct window *tw = win_find(top);

            event_push(INTUITION_EVENT_ACTIVATE, top, -1, 0u, tw ? tw->x : 0,
                       tw ? tw->y : 0);
        }
        g_depths++;
        g_actions++;
        intuition_redraw();
        return;
    }
    /* Phase 6F: the front gadget raises a covered window and does nothing at
       all when the window already owns the front of the stack. */
    if (front_hit(w, px, py)) {
        if (g_order_count > 0u && g_order[g_order_count - 1u] == id) {
            return;
        }
        prev = g_active;
        win_raise(id);
        g_active = id;
        if (prev != id) {
            event_push(INTUITION_EVENT_ACTIVATE, id, -1, 0u, w->x, w->y);
        }
        if (g_reporting) {
            kprintf("CRUGOS: wb window %d sent to front\n", id);
        }
        event_push(INTUITION_EVENT_FRONT, id, -1, 0u, w->x, w->y);
        g_fronts++;
        g_actions++;
        intuition_redraw();
        return;
    }
    prev = g_active;
    win_raise(id);
    g_active = id;
    if (prev != id) {
        event_push(INTUITION_EVENT_ACTIVATE, id, -1, 0u, w->x, w->y);
    }
    if (close_hit(w, px, py)) {
        if (g_reporting) {
            kprintf("CRUGOS: wb window %d closed (gadget)\n", id);
        }
        event_push(INTUITION_EVENT_CLOSE, id, -1, 0u, w->x, w->y);
        g_actions++;
        g_close_count++;
        intuition_close(id);
        return;
    }
    if (resize_hit(w, px, py)) {
        g_resize = id;
        g_resize_px = px;
        g_resize_py = py;
        g_resize_w = w->w;
        g_resize_h = w->h;
        if (g_reporting) {
            kprintf("CRUGOS: wb window %d resize start (%d, %d)\n", id,
                    (int)px, (int)py);
        }
        intuition_redraw();
        return;
    }
    gi = gadget_hit(w, px, py);
    if (gi >= 0) {
        g_gadget_win = id;
        g_gadget = gi;
        if (g_reporting) {
            kprintf("CRUGOS: wb window %d active\n", id);
        }
        intuition_redraw();
        return;
    }
    /* Phase 6H: a window with a body input handler owns presses inside its
       body, so a list can select a row and drag its own scrollbar. */
    {
        i32 bx;
        i32 by;
        u32 bw;
        u32 bh;

        body_rect(w, &bx, &by, &bw, &bh);
        if (w->body_input && bw > 0u && bh > INTUITION_TITLE_HEIGHT &&
            px >= bx && px < bx + (i32)bw && py >= by && py < by + (i32)bh) {
            g_body_win = id;
            if (g_reporting) {
                kprintf("CRUGOS: wb window %d active\n", id);
            }
            w->body_input(id, INTUITION_BODY_PRESS, px - bx, py - by,
                          w->body_input_ctx);
            intuition_redraw();
            return;
        }
    }
    if (py >= w->y + (i32)INTUITION_BORDER &&
        py < w->y + (i32)INTUITION_BORDER + (i32)INTUITION_TITLE_HEIGHT) {
        g_drag = id;
        g_drag_dx = px - w->x;
        g_drag_dy = py - w->y;
        if (g_reporting) {
            kprintf("CRUGOS: wb window %d drag start (%d, %d)\n", id, (int)px,
                    (int)py);
        }
    }
    if (g_reporting) {
        kprintf("CRUGOS: wb window %d active\n", id);
    }
    intuition_redraw();
}

static void release(i32 px, i32 py)
{
    /* Phase 6H: finish a body interaction before any window or icon drag
       branch can run; a body press never starts one of those. */
    if (g_body_win >= 0) {
        int id = g_body_win;
        struct window *w = win_find(id);

        g_body_win = -1;
        if (w && w->body_input) {
            i32 bx;
            i32 by;
            u32 bw;
            u32 bh;

            body_rect(w, &bx, &by, &bw, &bh);
            w->body_input(id, INTUITION_BODY_RELEASE, px - bx, py - by,
                          w->body_input_ctx);
        }
        intuition_redraw();
        return;
    }
    /* Phase 6F: ending an icon drag queues the new tile origin once. */
    if (g_icon_drag >= 0) {
        int index = g_icon_drag;
        struct icon *ic = &g_icons[index];

        g_icon_drag = -1;
        if (g_icon_drag_moved) {
            if (g_reporting) {
                kprintf("CRUGOS: wb icon '%s' dropped at (%d, %d)\n",
                        ic->label, (int)ic->x, (int)ic->y);
            }
            event_push(INTUITION_EVENT_ICON_MOVE, 0, index, 0u, ic->x, ic->y);
            g_icon_moves++;
            g_actions++;
        }
        g_icon_drag_moved = 0;
        intuition_redraw();
        return;
    }
    if (g_resize > 0) {
        struct window *w = win_find(g_resize);
        int id = g_resize;

        g_resize = -1;
        if (w) {
            if (g_reporting) {
                kprintf("CRUGOS: wb window %d resized to %ux%u\n", id,
                        (unsigned)w->w, (unsigned)w->h);
            }
            event_push_resize(id, w->x, w->y, w->w, w->h);
            g_resizes++;
            g_actions++;
        }
        intuition_redraw();
        return;
    }
    if (g_gadget_win > 0) {
        struct window *w = win_find(g_gadget_win);
        int gi = g_gadget;

        g_gadget_win = 0;
        g_gadget = -1;
        if (w && gadget_contains(w, gi, px, py)) {
            if (g_reporting) {
                kprintf("CRUGOS: wb gadget '%s' clicked in window %d\n",
                        w->gadgets[gi].label, w->id);
            }
            event_push(INTUITION_EVENT_GADGET, w->id, gi, 0u, w->x, w->y);
            g_actions++;
            g_gadget_clicks++;
        }
        intuition_redraw();
        return;
    }
    if (g_drag > 0) {
        struct window *w = win_find(g_drag);
        int id = g_drag;

        if (w && g_reporting) {
            kprintf("CRUGOS: wb window %d moved to (%d, %d)\n", id,
                    (int)w->x, (int)w->y);
        }
        if (w) {
            event_push(INTUITION_EVENT_DRAG, id, -1, 0u, w->x, w->y);
        }
        g_drag = -1;
        g_drags++;
    }
}

static void motion(i32 px, i32 py, i32 dx, i32 dy)
{
    if (g_menu_win > 0) {
        struct window *w = win_find(g_menu_win);
        int row = menu_popup_row(w, px, py);

        if (row != g_menu_hover) {
            g_menu_hover = row;
            if (row >= 0 && w && g_reporting) {
                const struct intuition_menu *m = &w->menus[g_menu_index];

                kprintf("CRUGOS: wb menu '%s' item %d highlighted ('%s')\n",
                        m->title, row, m->items[row]);
            }
            intuition_redraw();
        }
        return;
    }
    if (g_resize > 0) {
        struct window *w = win_find(g_resize);
        i32 nw;
        i32 nh;
        i32 max_w;
        i32 max_h;

        if (!w) {
            g_resize = -1;
            return;
        }
        nw = (i32)g_resize_w + (px - g_resize_px);
        nh = (i32)g_resize_h + (py - g_resize_py);
        max_w = (i32)g_screen_w - w->x;
        max_h = (i32)g_screen_h - w->y;
        if (nw < (i32)INTUITION_MIN_WIDTH) {
            nw = (i32)INTUITION_MIN_WIDTH;
        }
        if (nh < (i32)INTUITION_MIN_HEIGHT) {
            nh = (i32)INTUITION_MIN_HEIGHT;
        }
        if (nw > max_w || nh > max_h) {
            return;
        }
        if ((u32)nw != w->w || (u32)nh != w->h) {
            w->w = (u32)nw;
            w->h = (u32)nh;
            intuition_redraw();
        }
        return;
    }
    if (g_gadget_win > 0) {
        struct window *w = win_find(g_gadget_win);

        if (!w || !gadget_contains(w, g_gadget, px, py)) {
            g_gadget_win = 0;
            g_gadget = -1;
            intuition_redraw();
        }
        return;
    }
    /* Phase 6H: while a press sits inside a window body, every pointer move
       belongs to that window's handler. */
    if (g_body_win >= 0) {
        struct window *w = win_find(g_body_win);
        i32 bx;
        i32 by;
        u32 bw;
        u32 bh;

        if (!w || !w->body_input) {
            g_body_win = -1;
            return;
        }
        body_rect(w, &bx, &by, &bw, &bh);
        w->body_input(g_body_win, INTUITION_BODY_MOVE, px - bx, py - by,
                      w->body_input_ctx);
        return;
    }
    if (g_icon_drag >= 0 && (dx != 0 || dy != 0)) {
        struct icon *ic = &g_icons[g_icon_drag];
        i32 nx = px - g_icon_dx;
        i32 ny = py - g_icon_dy;

        /* Phase 6F: the first INTUITION_ICON_DRAG_MIN pixels are a dead zone so
         * a shaky click does not nudge the object. */
        if (!g_icon_drag_moved) {
            i32 ddx = nx - ic->x;
            i32 ddy = ny - ic->y;

            if (ddx > -(i32)INTUITION_ICON_DRAG_MIN &&
                ddx < (i32)INTUITION_ICON_DRAG_MIN &&
                ddy > -(i32)INTUITION_ICON_DRAG_MIN &&
                ddy < (i32)INTUITION_ICON_DRAG_MIN) {
                return;
            }
        }

        /* The whole tile stays on the desktop, exactly like intuition_add_icon. */
        if (nx < 0) {
            nx = 0;
        }
        if (ny < 0) {
            ny = 0;
        }
        if ((u32)nx + INTUITION_ICON_WIDTH > g_screen_w) {
            nx = (i32)g_screen_w - (i32)INTUITION_ICON_WIDTH;
        }
        if ((u32)ny + INTUITION_ICON_HEIGHT > g_screen_h) {
            ny = (i32)g_screen_h - (i32)INTUITION_ICON_HEIGHT;
        }
        if (nx != ic->x || ny != ic->y) {
            ic->x = nx;
            ic->y = ny;
            g_icon_drag_moved = 1;
            if (g_reporting) {
                kprintf("CRUGOS: wb icon '%s' drag to (%d, %d)\n", ic->label,
                        (int)nx, (int)ny);
            }
            intuition_redraw();
        }
        return;
    }
    if (g_drag > 0 && (dx != 0 || dy != 0)) {
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
            press(px, py);
        } else {
            release(px, py);
        }
        return;
    }
    motion(px, py, event->dx, event->dy);
}

int intuition_event_poll(struct intuition_event *out)
{
    if (!out || g_event_count == 0u) {
        return 0;
    }
    *out = g_events[g_event_head];
    g_event_head = (g_event_head + 1u) % INTUITION_EVENT_QUEUE;
    g_event_count--;
    return 1;
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

/* Install a body painter for one window.  The painter receives the body
   rectangle in screen coordinates and owns every pixel inside it; gadgets
   are drawn on top afterwards.  Passing a NULL painter restores the static
   body text. */
int intuition_set_paint(int id,
                        void (*fn)(int id, i32 x, i32 y, u32 w, u32 h,
                                   void *ctx),
                        void *ctx)
{
    struct window *w = win_find(id);

    if (!w) {
        return -1;
    }
    w->paint = fn;
    w->paint_ctx = ctx;
    return 0;
}

/* Install a body input handler for one window.  Intuition.device keeps
   delivering pointer reports to it until the button is released, so the
   callee (a list painter in the Workbench) can select a row and drag its
   scrollbar without the device knowing the body layout. */
int intuition_set_body_input(int id,
                             void (*fn)(int id, u32 kind, i32 x, i32 y,
                                        void *ctx),
                             void *ctx)
{
    struct window *w = win_find(id);

    if (!w) {
        return -1;
    }
    w->body_input = fn;
    w->body_input_ctx = ctx;
    return 0;
}

/* Repaint one window without touching the rest of the desktop: the shell
   calls this after every keystroke instead of a full intuition_redraw(),
   which would repaint every window and the menu bar on each keypress. */
void intuition_redraw_window(int id)
{
    struct window *w = win_find(id);

    if (!w) {
        return;
    }
    win_draw(w);
}

u64 intuition_drag_count(void)
{
    return g_drags;
}

u32 intuition_window_count(void)
{
    return g_order_count;
}

u32 intuition_event_count(void)
{
    return g_event_count;
}

u32 intuition_event_dropped(void)
{
    return g_event_dropped;
}

u64 intuition_action_count(void)
{
    return g_actions;
}

u32 intuition_menu_opens(void)
{
    return g_menu_opens;
}

u32 intuition_gadget_clicks(void)
{
    return g_gadget_clicks;
}

u64 intuition_close_count(void)
{
    return g_close_count;
}

int intuition_menu_open(void)
{
    return g_menu_win > 0 ? g_menu_index : -1;
}

int intuition_menu_hover(void)
{
    return g_menu_hover;
}

/* Phase 6D keyboard navigation.  Tab cycles the focus through the stacking
   order (raising the window it lands on), the arrow keys move the active
   window by INTUITION_KEY_STEP pixels, and while a popup is open the arrows
   highlight items, Enter selects and Escape dismisses.  The desktop does not
   consume any other key: input.device has no modifier delivery yet, so
   Shift-Tab and Alt shortcuts wait for a later phase. */
void intuition_key_event(u8 key)
{
    struct window *w;
    u32 i;
    u32 pos = 0u;
    int found = 0;

    if (g_screen_w == 0u || g_screen_h == 0u) {
        return;
    }
    if (g_menu_win > 0) {
        struct window *mw = win_find(g_menu_win);
        const struct intuition_menu *m;
        int row;

        if (!mw || g_menu_index < 0 || (u32)g_menu_index >= mw->menu_count) {
            menu_close();
            return;
        }
        m = &mw->menus[g_menu_index];
        if (key == INPUT_KEY_DOWN || key == INPUT_KEY_UP) {
            row = g_menu_hover;
            if (key == INPUT_KEY_DOWN) {
                if (row < 0) {
                    row = 0;
                } else if ((u32)(row + 1) < m->item_count) {
                    row++;
                }
            } else if (row > 0) {
                row--;
            }
            if (row != g_menu_hover) {
                g_menu_hover = row;
                if (row >= 0 && g_reporting) {
                    kprintf("CRUGOS: wb menu '%s' item %d highlighted ('%s')\n",
                            m->title, row, m->items[row]);
                }
                intuition_redraw();
            }
            return;
        }
        if (key == '\n' || key == '\r') {
            menu_select(mw, (u32)(g_menu_hover > 0 ? g_menu_hover : 0));
            return;
        }
        if (key == 0x1b) {
            menu_close();
            if (g_reporting) {
                kprintf("CRUGOS: wb menu closed (escape)\n");
            }
            return;
        }
        return;
    }
    if (key == '\t') {
        if (g_order_count == 0u) {
            return;
        }
        for (i = 0; i < g_order_count; i++) {
            if (g_order[i] == g_active) {
                pos = i;
                found = 1;
            }
        }
        pos = found ? (pos + 1u) % g_order_count : 0u;
        w = win_find(g_order[pos]);
        if (!w) {
            return;
        }
        win_raise(w->id);
        g_active = w->id;
        event_push(INTUITION_EVENT_ACTIVATE, w->id, -1, 0u, w->x, w->y);
        if (g_reporting) {
            kprintf("CRUGOS: wb window %d focused (Tab)\n", w->id);
        }
        intuition_redraw();
        return;
    }
    if (key == INPUT_KEY_UP || key == INPUT_KEY_DOWN ||
        key == INPUT_KEY_LEFT || key == INPUT_KEY_RIGHT) {
        w = win_find(g_active);
        if (!w) {
            return;
        }
        if (key == INPUT_KEY_UP) {
            w->y -= (i32)INTUITION_KEY_STEP;
        } else if (key == INPUT_KEY_DOWN) {
            w->y += (i32)INTUITION_KEY_STEP;
        } else if (key == INPUT_KEY_LEFT) {
            w->x -= (i32)INTUITION_KEY_STEP;
        } else {
            w->x += (i32)INTUITION_KEY_STEP;
        }
        win_clamp(w);
        if (g_reporting) {
            kprintf("CRUGOS: wb window %d key move to (%d, %d)\n", w->id,
                    (int)w->x, (int)w->y);
        }
        intuition_redraw();
        return;
    }
    if (key == '\n' || key == '\r') {
        w = win_find(g_active);
        if (!w || w->menu_count == 0u) {
            return;
        }
        if (menu_open_at((i32)menu_strip_x(w, 0u) +
                             (i32)(menu_strip_width(&w->menus[0]) / 2u),
                         (i32)(INTUITION_MENU_BAR_HEIGHT / 2u))) {
            g_menu_hover = 0;
            if (g_reporting) {
                kprintf("CRUGOS: wb menu '%s' item 0 highlighted ('%s')\n",
                        w->menus[0].title, w->menus[0].items[0]);
            }
            intuition_redraw();
        }
        return;
    }
}

u64 intuition_resize_count(void)
{
    return g_resizes;
}
