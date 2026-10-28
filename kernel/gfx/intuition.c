/* intuition.device: a small software window manager (phase 6A/6B).
   Windows are plain rectangles with a title bar and body text; a window can
   also own screen-top menus and labelled body gadgets.  The device keeps up
   to four windows, renders bottom to top with the menu bar and the open menu
   on top, raises and activates the window under a left click, drags a window
   by its title bar, opens and selects menus, runs gadgets, closes a window
   from its close gadget and queues what it consumed as typed events.  All
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

static int g_menu_win;   /* window owning the open menu, 0 when closed */
static int g_menu_index; /* index in that window's menu table, -1 closed */
static int g_menu_hover; /* highlighted item index, -1 none */
static u32 g_menu_opens;
static u64 g_actions;
static u32 g_gadget_clicks;
static u64 g_close_count;
static int g_gadget_win; /* window whose gadget is pressed, 0 when none */
static int g_gadget;

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
    g_event_count++;
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
    win_draw_gadgets(w);
}

static void win_draw(struct window *w)
{
    fb_frame_rect((u32)w->x, (u32)w->y, w->w, w->h, INTUITION_FRAME_RGB);
    win_draw_title(w);
    win_draw_body(w);
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
    menu_draw_bar();
    menu_draw_popup();
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
        return;
    }
    w = win_find(id);
    if (!w) {
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
    if (g_gadget_win > 0) {
        struct window *w = win_find(g_gadget_win);

        if (!w || !gadget_contains(w, g_gadget, px, py)) {
            g_gadget_win = 0;
            g_gadget = -1;
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
