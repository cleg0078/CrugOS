/* Workbench desktop bring-up (phase 6A/6B/6C).
   The desktop is a gate-only mode for now: wb.enable=1 takes the framebuffer
   away from the text console (which keeps writing to the serial port), opens
   two windows through intuition.device, gives them menus and a body gadget,
   and either waits for a title-bar drag (wb.drag=) or for a sequence of menu,
   gadget and close actions (wb.menu=1 wb.acts=) so the hotplug gate can prove
   the event path with serial markers and screendump comparisons.  Phase 6C
   adds the desktop objects: wb.desk=1 puts a System: volume icon on the
   background and a double click opens a drawer window listing the root.
   Phase 6C adds the live shell: wb.shell=1 paints the shell's text grid into the Shell
   window body and routes every key there, and wb.sh=<n> waits for n typed
   commands before the boot finishes.  Phase 6G makes the drawer scroll, refresh
   and reflow on resize: wb.dr=1 gives the open drawer the arrow keys and 'r',
   and wb.fill=<n> creates files first so there is something to scroll. */
#include <crug/types.h>
#include <crug/string.h>
#include <crug/kernel.h>
#include <crug/fb.h>
#include <crug/gfx.h>
#include <crug/input.h>
#include <crug/intuition.h>
#include <crug/timer.h>
#include <crug/cpu.h>
#include <crug/usb.h>
#include <crug/shell.h>
#include <crug/textgrid.h>
#include <crug/storage.h>
#include <crug/workbench.h>

static int g_enabled;
static int g_ready;
static int g_reporting;
static u32 g_wait_seconds;
static int g_wait_given;
static u32 g_want_drags;
static u32 g_hold_seconds;
static int g_menu_mode;
static u32 g_want_actions;
static u32 g_actions_seen;

/* Phase 6D: keyboard navigation (Tab focus, arrow moves, menu keys). */
static int g_keys_mode;

/* Phase 6F: keyboard focus for the desktop icon table and the front gadget. */
static int g_icon_keys_mode;

/* Phase 6E: desktop objects (the System: volume icon) and the drawer window
   that opens on a double click and lists the mounted root. */
static int g_desk_mode;
static int g_drawer_window;
static u32 g_drawers;
static int g_icon_last = -1;
static u64 g_icon_last_ns;
/* Phase 6G: the drawer's parsed listing and its view over it. */
static int g_drawer_mode;
static u32 g_fill_count;
static int g_filled;
static char g_drawer_names[WB_DRAWER_MAX][WB_DRAWER_NAME_MAX];
static u32 g_drawer_count;
static u32 g_drawer_top;
static u32 g_drawer_rows;
static u32 g_drawer_scrolls;
static u32 g_drawer_refreshes;
static u32 g_drawer_ops_seen;
/* Phase 6H: row selection and the draggable scrollbar.  The selection is an
   index into the listing, so it scrolls with the view; the body size is
   remembered by the painter because the pointer handler only gets
   body-relative coordinates. */
static int g_select_mode;
static int g_drawer_sel = -1;
static u32 g_drawer_selects;
static u32 g_drawer_scrollbar;
static u32 g_select_ops;
static int g_scroll_drag;
static u32 g_scroll_grab;
static u32 g_drawer_body_w;
static u32 g_drawer_body_h;

static void wb_open_drawer(void);
static void wb_icon_keyboard_activate(void);

/* Phase 6C: the Shell window hosts the live shell. */
static int g_shell_mode;
static u32 g_want_commands;
static u32 g_commands_seen;
static int g_shell_window;
static struct textgrid g_grid;
static struct shell g_shell;

/* Menus of the active window are drawn in the screen-top menu bar. */
static const struct intuition_menu wb_menus_top[2] = {
    { "Workbench", 3u, { "About", "Info", "Quit" } },
    { "Window", 2u, { "Close", "New" } },
};
static const struct intuition_menu wb_menus_shell[2] = {
    { "Shell", 2u, { "New", "Close" } },
    { "Workbench", 3u, { "About", "Info", "Quit" } },
};

static int wb_is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static int wb_prefix(const char *tok, usize len, const char *prefix)
{
    usize plen = strlen(prefix);

    if (len < plen) {
        return -1;
    }
    return memcmp(tok, prefix, plen) == 0 ? (int)plen : -1;
}

static int wb_parse_u32(const char *tok, usize len, u32 *out)
{
    u64 value = 0;
    usize i;

    if (!len || !out) {
        return -1;
    }
    for (i = 0; i < len; i++) {
        if (tok[i] < '0' || tok[i] > '9') {
            return -1;
        }
        value = value * 10u + (u64)(tok[i] - '0');
        if (value > 1000000u) {
            return -1;
        }
    }
    *out = (u32)value;
    return 0;
}

static void wb_cmdline_token(const char *tok, usize len)
{
    int used;
    u32 value;

    used = wb_prefix(tok, len, "wb.enable=");
    if (used >= 0) {
        if (wb_parse_u32(tok + used, len - (usize)used, &value) == 0) {
            g_enabled = value ? 1 : 0;
        }
        return;
    }
    used = wb_prefix(tok, len, "wb.wait=");
    if (used >= 0) {
        if (wb_parse_u32(tok + used, len - (usize)used, &value) == 0) {
            g_wait_seconds = value > WB_WAIT_MAX ? WB_WAIT_MAX : value;
            g_wait_given = 1;
        }
        return;
    }
    used = wb_prefix(tok, len, "wb.drag=");
    if (used >= 0) {
        if (wb_parse_u32(tok + used, len - (usize)used, &value) == 0) {
            g_want_drags = value > WB_DRAG_MAX ? WB_DRAG_MAX : value;
        }
        return;
    }
    used = wb_prefix(tok, len, "wb.menu=");
    if (used >= 0) {
        if (wb_parse_u32(tok + used, len - (usize)used, &value) == 0) {
            g_menu_mode = value ? 1 : 0;
        }
        return;
    }
    used = wb_prefix(tok, len, "wb.acts=");
    if (used >= 0) {
        if (wb_parse_u32(tok + used, len - (usize)used, &value) == 0) {
            g_want_actions = value > WB_ACTS_MAX ? WB_ACTS_MAX : value;
        }
        return;
    }
    used = wb_prefix(tok, len, "wb.keys=");
    if (used >= 0) {
        if (wb_parse_u32(tok + used, len - (usize)used, &value) == 0) {
            g_keys_mode = value ? 1 : 0;
        }
        return;
    }
    used = wb_prefix(tok, len, "wb.ikeys=");
    if (used >= 0) {
        if (wb_parse_u32(tok + used, len - (usize)used, &value) == 0) {
            g_icon_keys_mode = value ? 1 : 0;
        }
        return;
    }
    used = wb_prefix(tok, len, "wb.desk=");
    if (used >= 0) {
        if (wb_parse_u32(tok + used, len - (usize)used, &value) == 0) {
            g_desk_mode = value ? 1 : 0;
        }
        return;
    }
    used = wb_prefix(tok, len, "wb.dr=");
    if (used >= 0) {
        if (wb_parse_u32(tok + used, len - (usize)used, &value) == 0) {
            g_drawer_mode = value ? 1 : 0;
        }
        return;
    }
    used = wb_prefix(tok, len, "wb.fill=");
    if (used >= 0) {
        if (wb_parse_u32(tok + used, len - (usize)used, &value) == 0) {
            g_fill_count = value > WB_FILL_MAX ? WB_FILL_MAX : value;
        }
        return;
    }
    used = wb_prefix(tok, len, "wb.dsel=");
    if (used >= 0) {
        if (wb_parse_u32(tok + used, len - (usize)used, &value) == 0) {
            g_select_mode = value ? 1 : 0;
        }
        return;
    }
    used = wb_prefix(tok, len, "wb.shell=");
    if (used >= 0) {
        if (wb_parse_u32(tok + used, len - (usize)used, &value) == 0) {
            g_shell_mode = value ? 1 : 0;
        }
        return;
    }
    used = wb_prefix(tok, len, "wb.sh=");
    if (used >= 0) {
        if (wb_parse_u32(tok + used, len - (usize)used, &value) == 0) {
            g_want_commands = value > WB_SHELL_CMDS_MAX ? WB_SHELL_CMDS_MAX
                                                        : value;
        }
        return;
    }
    used = wb_prefix(tok, len, "wb.hold=");
    if (used >= 0) {
        if (wb_parse_u32(tok + used, len - (usize)used, &value) == 0) {
            g_hold_seconds = value > WB_HOLD_MAX ? WB_HOLD_MAX : value;
        }
    }
}

void wb_cmdline(const boot_info *bi)
{
    const char *p;
    const char *start;
    usize len;

    if (!bi || !bi->command_line) {
        return;
    }
    p = bi->command_line;
    len = (usize)bi->command_line_len;
    while (len > 0u) {
        while (len > 0u && wb_is_space(*p)) {
            p++;
            len--;
        }
        start = p;
        while (len > 0u && !wb_is_space(*p)) {
            p++;
            len--;
        }
        if (p > start) {
            wb_cmdline_token(start, (usize)(p - start));
        }
    }
}

static void wb_report_cursor(const struct input_pointer *event)
{
    if (!g_ready || !event) {
        return;
    }
    if (g_reporting && (event->dx != 0 || event->dy != 0)) {
        kprintf("CRUGOS: wb cursor at (%d, %d)\n", (int)fb_cursor_x(),
                (int)fb_cursor_y());
    }
    intuition_pointer_event(event);
}

/* The console read path polls this hook, so pointer motion still reaches the
   desktop when a program asks input.device for a character. */
static void wb_console_poll(void)
{
    usb_poll();
    gfx_pointer_poll();
}

/* Drain what intuition.device consumed: the gate counts the same actions the
   device already validated, and prints them in a fixed numeric form. */
static void wb_drain_events(void)
{
    struct intuition_event event;

    while (intuition_event_poll(&event)) {
        if (!g_reporting) {
            continue;
        }
        if (event.type == INTUITION_EVENT_MENU) {
            g_actions_seen++;
            kprintf("CRUGOS: wb action %u menu window %d item %u\n",
                    (unsigned)g_actions_seen, event.window,
                    (unsigned)event.item);
        } else if (event.type == INTUITION_EVENT_GADGET) {
            g_actions_seen++;
            kprintf("CRUGOS: wb action %u gadget window %d index %d\n",
                    (unsigned)g_actions_seen, event.window, event.index);
        } else if (event.type == INTUITION_EVENT_CLOSE) {
            g_actions_seen++;
            kprintf("CRUGOS: wb action %u close window %d\n",
                    (unsigned)g_actions_seen, event.window);
        } else if (event.type == INTUITION_EVENT_RESIZE) {
            g_actions_seen++;
            kprintf("CRUGOS: wb action %u resize window %d (%ux%u)\n",
                    (unsigned)g_actions_seen, event.window, (unsigned)event.w,
                    (unsigned)event.h);
        } else if (event.type == INTUITION_EVENT_ICON) {
            u64 now = timer_monotonic_ns();

            g_actions_seen++;
            kprintf("CRUGOS: wb action %u icon %d\n",
                    (unsigned)g_actions_seen, event.index);
            if (event.index >= 0 && event.index == g_icon_last &&
                g_icon_last_ns != 0u && now >= g_icon_last_ns &&
                now - g_icon_last_ns <= WB_DOUBLE_CLICK_NS) {
                wb_open_drawer();
                g_icon_last = -1;
                g_icon_last_ns = 0u;
            } else {
                g_icon_last = event.index;
                g_icon_last_ns = now;
            }
        } else if (event.type == INTUITION_EVENT_DEPTH) {
            g_actions_seen++;
            kprintf("CRUGOS: wb action %u depth window %d\n",
                    (unsigned)g_actions_seen, event.window);
        } else if (event.type == INTUITION_EVENT_ICON_MOVE) {
            g_actions_seen++;
            kprintf("CRUGOS: wb action %u icon-move %d\n",
                    (unsigned)g_actions_seen, event.index);
        } else if (event.type == INTUITION_EVENT_FRONT) {
            g_actions_seen++;
            kprintf("CRUGOS: wb action %u front window %d\n",
                    (unsigned)g_actions_seen, event.window);
        }
    }
}

/* ---- phase 6E/6G: desktop objects and the scrollable drawer --------- */

/* Fill the mounted root with empty files first so the drawer has rows to
   scroll through: wb.fill=<n> creates /f00../fNN once, right before it opens. */
static void wb_fill_root(void)
{
    struct ext4_fs *fs;
    u32 i;
    u32 made = 0u;

    if (g_fill_count == 0u || g_filled) {
        return;
    }
    g_filled = 1;
    fs = storage_root();
    if (!fs) {
        kprintf("CRUGOS: wb fill skipped (no volume)\n");
        return;
    }
    /* The boot mounts the root read-only, so the fill needs a short
       read-write remount; the unmount in storage_remount_ro() commits it. */
    if (storage_remount_rw() != 0) {
        kprintf("CRUGOS: wb fill skipped (read-only volume)\n");
        return;
    }
    for (i = 0u; i < g_fill_count; i++) {
        char path[8];
        u32 n = 0u;

        path[n++] = '/';
        path[n++] = 'f';
        path[n++] = (char)('0' + (char)((i / 10u) % 10u));
        path[n++] = (char)('0' + (char)(i % 10u));
        path[n] = 0;
        if (ext4_create(fs, path, 0644u) == 0) {
            made++;
        }
    }
    (void)storage_remount_ro();
    if (g_reporting) {
        kprintf("CRUGOS: wb fill %u file(s)\n", (unsigned)made);
    }
}

/* Parse the newline separated root listing into the drawer name table.  Names
   longer than the table are truncated, entries past the table are dropped. */
static u32 wb_drawer_load(void)
{
    struct ext4_fs *fs = storage_root();
    static char listing[1024];
    u32 count = 0u;
    int n;
    int i = 0;

    g_drawer_count = 0u;
    if (!fs) {
        return 0u;
    }
    listing[0] = 0;
    n = ext4_list(fs, "/", listing, sizeof(listing));
    while (i < n) {
        u32 len = 0u;

        if (listing[i] == '\n') {
            i++;
            continue;
        }
        if (count < WB_DRAWER_MAX) {
            while (i < n && listing[i] != '\n' &&
                   len + 1u < WB_DRAWER_NAME_MAX) {
                g_drawer_names[count][len++] = listing[i++];
            }
            g_drawer_names[count][len] = 0;
            count++;
        }
        while (i < n && listing[i] != '\n') {
            i++;
        }
        if (i < n) {
            i++;
        }
    }
    g_drawer_count = count;
    return count;
}

/* Visible rows of a drawer body h pixels tall: the first row starts 6 px below
   the body edge, glyphs are 16 px tall and rows are WB_DRAWER_ROW_H apart. */
static u32 wb_drawer_rows(u32 h)
{
    if (h <= 22u) {
        return 0u;
    }
    return (h - 22u) / WB_DRAWER_ROW_H + 1u;
}

static u32 wb_drawer_max_top(void)
{
    if (g_drawer_rows == 0u) {
        return 0u;
    }
    return g_drawer_count > g_drawer_rows ? g_drawer_count - g_drawer_rows
                                          : 0u;
}

static void wb_drawer_clamp_top(void)
{
    u32 max_top = wb_drawer_max_top();

    if (g_drawer_top > max_top) {
        g_drawer_top = max_top;
    }
}

/* Scrollbar geometry shared by the painter and the pointer handler: the track
   sits on the right edge of the body, the thumb is proportional to the rows
   the body can show, and span is the travel of the thumb from top to bottom. */
static void wb_track_geom(u32 w, u32 h, u32 *track_x, u32 *track_y,
                          u32 *track_h, u32 *thumb_h, u32 *span)
{
    u32 rows = wb_drawer_rows(h);
    u32 th = 0u;

    *track_x = w > 8u ? w - 8u : 0u;
    *track_y = 6u;
    *track_h = h > 12u ? h - 12u : 0u;
    if (g_drawer_count != 0u) {
        th = *track_h * rows / g_drawer_count;
    }
    if (th < 12u) {
        th = 12u;
    }
    if (th > *track_h) {
        th = *track_h;
    }
    *thumb_h = th;
    *span = *track_h > th ? *track_h - th : 0u;
}

/* Body painter of the drawer window: the visible slice of the listing, the
   selected row in inverse amber and, while the listing is longer than the
   body, a scrollbar on the right edge. */
static void wb_drawer_paint(int id, i32 x, i32 y, u32 w, u32 h, void *ctx)
{
    u32 max_chars = w > 20u ? (w - 20u) / 8u : 0u;
    u32 rows = wb_drawer_rows(h);
    u32 i;

    (void)id;
    (void)ctx;
    if (max_chars > 47u) {
        max_chars = 47u;
    }
    g_drawer_rows = rows;
    g_drawer_body_w = w;
    g_drawer_body_h = h;
    wb_drawer_clamp_top();
    fb_fill_rect((u32)x, (u32)y, w, h, INTUITION_WINDOW_RGB);
    if (g_drawer_count == 0u) {
        fb_draw_text((u32)x + 6u, (u32)y + 6u, "no volume",
                     INTUITION_BODY_TEXT_RGB, INTUITION_WINDOW_RGB);
        return;
    }
    for (i = 0u; i < rows; i++) {
        u32 index = g_drawer_top + i;
        char text[WB_DRAWER_NAME_MAX];
        u32 n = 0u;
        u32 fg = INTUITION_BODY_TEXT_RGB;
        u32 bg = INTUITION_WINDOW_RGB;

        if (index >= g_drawer_count) {
            break;
        }
        while (n + 1u < sizeof(text) && g_drawer_names[index][n]) {
            text[n] = g_drawer_names[index][n];
            n++;
        }
        text[n] = 0;
        if (max_chars != 0u && n > max_chars) {
            text[max_chars] = 0;
        }
        if (g_drawer_sel >= 0 && (u32)g_drawer_sel == index) {
            fg = INTUITION_WINDOW_RGB;
            bg = INTUITION_MENU_HIGHLIGHT_RGB;
            fb_fill_rect((u32)x, (u32)y + 4u + i * WB_DRAWER_ROW_H,
                         w > 12u ? w - 12u : w, WB_DRAWER_ROW_H, bg);
        }
        fb_draw_text((u32)x + 6u, (u32)y + 6u + i * WB_DRAWER_ROW_H, text,
                     fg, bg);
    }
    if (g_drawer_count > rows && w > 12u && h > 12u) {
        u32 track_x;
        u32 track_y;
        u32 track_h;
        u32 thumb_h;
        u32 span;
        u32 max_top = wb_drawer_max_top();
        u32 thumb_y;

        wb_track_geom(w, h, &track_x, &track_y, &track_h, &thumb_h, &span);
        thumb_y = (u32)y + track_y;
        if (max_top != 0u && span != 0u) {
            thumb_y += span * g_drawer_top / max_top;
        }
        fb_fill_rect((u32)x + track_x, (u32)y + track_y, WB_TRACK_W,
                     track_h, 0x00404040u);
        fb_fill_rect((u32)x + track_x, thumb_y, WB_TRACK_W, thumb_h,
                     INTUITION_MENU_BG_RGB);
    }
}

/* Move the view so the thumb follows a pointer at body-relative y during a
   scrollbar drag.  g_scroll_grab is the pointer offset inside the thumb at
   press time, so grabbing the thumb anywhere keeps its grip. */
static void wb_drawer_seek(u32 pointer_y)
{
    u32 track_x;
    u32 track_y;
    u32 track_h;
    u32 thumb_h;
    u32 span;
    u32 max_top = wb_drawer_max_top();
    u32 offset;
    u32 top;

    if (g_drawer_body_w == 0u || g_drawer_body_h == 0u) {
        return;
    }
    wb_track_geom(g_drawer_body_w, g_drawer_body_h, &track_x, &track_y,
                  &track_h, &thumb_h, &span);
    if (span == 0u || max_top == 0u) {
        return;
    }
    offset = pointer_y > track_y + g_scroll_grab
                 ? pointer_y - track_y - g_scroll_grab
                 : 0u;
    if (offset > span) {
        offset = span;
    }
    top = offset * max_top / span;
    if (top > max_top) {
        top = max_top;
    }
    if (top == g_drawer_top) {
        return;
    }
    g_drawer_top = top;
    intuition_redraw_window(g_drawer_window);
}

/* Phase 6H: pointer handler for the drawer body.  A press inside the scrollbar
   track jumps the view there and starts a thumb drag; a press on a row selects
   it.  The selection is an index into the listing, so it scrolls with the
   view instead of sticking to a screen row. */
static void wb_drawer_input(int id, u32 kind, i32 x, i32 y, void *ctx)
{
    u32 track_x;
    u32 track_y;
    u32 track_h;
    u32 thumb_h;
    u32 span;
    u32 max_top = wb_drawer_max_top();

    (void)ctx;
    if (id != g_drawer_window || g_drawer_rows == 0u ||
        g_drawer_body_w == 0u || g_drawer_body_h == 0u) {
        return;
    }
    wb_track_geom(g_drawer_body_w, g_drawer_body_h, &track_x, &track_y,
                  &track_h, &thumb_h, &span);
    if (kind == INTUITION_BODY_PRESS) {
        if (span != 0u && max_top != 0u && x >= (i32)track_x - 2 &&
            x < (i32)(track_x + WB_TRACK_W + 2) && y >= (i32)track_y &&
            y < (i32)(track_y + track_h)) {
            u32 thumb_y = track_y + span * g_drawer_top / max_top;

            if ((u32)y >= thumb_y && (u32)y < thumb_y + thumb_h) {
                g_scroll_grab = (u32)y - thumb_y;
            } else {
                g_scroll_grab = thumb_h / 2u;
            }
            g_scroll_drag = 1;
            g_drawer_scrollbar++;
            g_select_ops++;
            wb_drawer_seek((u32)y);
            if (g_reporting) {
                kprintf("CRUGOS: wb drawer scrollbar grabbed\n");
            }
            return;
        }
        if (x >= 0 && y >= 0 && (u32)y >= track_y) {
            u32 row = ((u32)y - track_y) / WB_DRAWER_ROW_H;
            u32 index = g_drawer_top + row;

            if (index < g_drawer_count) {
                g_drawer_sel = (int)index;
                g_drawer_selects++;
                g_select_ops++;
                intuition_redraw_window(g_drawer_window);
                if (g_reporting) {
                    kprintf("CRUGOS: wb drawer selected '%s' (index %u of %u)\n",
                            g_drawer_names[index], (unsigned)index,
                            (unsigned)g_drawer_count);
                }
            }
        }
        return;
    }
    if (kind == INTUITION_BODY_MOVE) {
        if (g_scroll_drag) {
            wb_drawer_seek((u32)(y > 0 ? y : 0));
        }
        return;
    }
    /* INTUITION_BODY_RELEASE */
    if (!g_scroll_drag) {
        return;
    }
    g_scroll_drag = 0;
    if (g_reporting) {
        kprintf("CRUGOS: wb drawer scrollbar to (top %u of %u)\n",
                (unsigned)g_drawer_top, (unsigned)g_drawer_count);
    }
}

/* Move the drawer view by one row; returns 1 only when the view changed, so a
   press against either end of the listing is not counted as an action. */
static int wb_drawer_scroll(int delta)
{
    u32 top = g_drawer_top;
    u32 max_top = wb_drawer_max_top();

    if (g_drawer_window <= 0 || g_drawer_rows == 0u) {
        return 0;
    }
    if (delta < 0) {
        if (top > 0u) {
            top--;
        }
    } else if (top < max_top) {
        top++;
    }
    if (top == g_drawer_top) {
        return 0;
    }
    g_drawer_top = top;
    g_drawer_scrolls++;
    g_drawer_ops_seen++;
    intuition_redraw_window(g_drawer_window);
    if (g_reporting) {
        kprintf("CRUGOS: wb drawer scrolled (top %u of %u)\n",
                (unsigned)g_drawer_top, (unsigned)g_drawer_count);
    }
    return 1;
}

/* Re-read the volume so files created elsewhere appear, keeping the view
   unless the listing got shorter than the body can show. */
static void wb_drawer_refresh(void)
{
    if (g_drawer_window <= 0) {
        return;
    }
    (void)wb_drawer_load();
    wb_drawer_clamp_top();
    g_drawer_refreshes++;
    g_drawer_ops_seen++;
    intuition_redraw_window(g_drawer_window);
    if (g_reporting) {
        kprintf("CRUGOS: wb drawer refreshed (%u entries)\n",
                (unsigned)g_drawer_count);
    }
}

static void wb_open_drawer(void)
{
    u32 entries;

    if (g_drawer_window > 0) {
        return;
    }
    if (g_fill_count != 0u) {
        wb_fill_root();
    }
    entries = wb_drawer_load();
    g_drawer_top = 0u;
    g_drawer_sel = -1;
    g_scroll_drag = 0;
    g_drawer_body_w = 0u;
    g_drawer_body_h = 0u;
    g_drawer_window = intuition_open(560, 60, 360, 260, "System:", NULL);
    if (g_drawer_window < 0) {
        return;
    }
    intuition_set_paint(g_drawer_window, wb_drawer_paint, 0);
    intuition_set_body_input(g_drawer_window, wb_drawer_input, 0);
    intuition_redraw_window(g_drawer_window);
    g_drawers++;
    if (g_reporting) {
        kprintf("CRUGOS: wb drawer 'System:' opened (%u entries)\n",
                (unsigned)entries);
        if (g_drawer_mode) {
            kprintf("CRUGOS: wb drawer ready (%u entries, %u row(s))\n",
                    (unsigned)entries, (unsigned)g_drawer_rows);
        }
    }
}

/* Phase 6G: the open drawer owns the arrow keys and 'r'; while it is closed
   the desktop object table keeps the keyboard. */
static void wb_drawer_pump(void)
{
    int ch;

    if (!g_drawer_mode || g_shell_mode || !g_ready) {
        return;
    }
    while ((ch = input_try_getc()) >= 0) {
        if (g_drawer_window > 0) {
            if (ch == (int)INPUT_KEY_UP) {
                wb_drawer_scroll(-1);
            } else if (ch == (int)INPUT_KEY_DOWN) {
                wb_drawer_scroll(1);
            } else if (ch == 'r' || ch == 'R') {
                wb_drawer_refresh();
            }
            continue;
        }
        if (ch >= (int)INPUT_KEY_UP || ch == '\t' || ch == '\n' ||
            ch == '\r' || ch == 0x1b) {
            if (intuition_icon_key((u8)ch)) {
                wb_icon_keyboard_activate();
            }
        }
    }
}

/* ---- phase 6C: the live shell -------------------------------------- */

/* Body painter: the shell's grid owns the Shell window body. */
static void wb_shell_paint(int id, i32 x, i32 y, u32 w, u32 h, void *ctx)
{
    (void)id;
    (void)ctx;
    tg_draw(&g_grid, (u32)x, (u32)y, w, h, INTUITION_BODY_TEXT_RGB,
            INTUITION_WINDOW_RGB);
}

/* Every command output line becomes one serial marker, so the gate can read
   the shell's answers without OCR. */
static void wb_shell_sink(const char *text, void *ctx)
{
    (void)ctx;
    if (g_reporting) {
        kprintf("CRUGOS: sh out %s\n", text);
    }
}

static void wb_shell_run(const char *line, void *ctx)
{
    (void)ctx;
    if (g_reporting) {
        kprintf("CRUGOS: sh run '%s'\n", line);
    }
}

/* Drain whatever input.device has queued into the shell and repaint the Shell
   window.  In shell mode every key goes to the shell, whatever window is
   active; the pointer path is unchanged.  Returns 1 when a line executed. */
static int wb_shell_pump(void)
{
    int ch;
    int executed = 0;

    if (!g_shell_mode || !g_ready) {
        return 0;
    }
    while ((ch = input_try_getc()) >= 0) {
        if (shell_feed(&g_shell, (u8)ch) == 1) {
            g_commands_seen++;
            executed = 1;
        }
        intuition_redraw_window(g_shell_window);
    }
    return executed;
}

/* Phase 6D: drain input.device and hand the decoded keys to intuition.  The
   shell window owns the keyboard when wb.shell=1, so keys mode leaves them
   for wb_shell_pump() instead. */
/* Phase 6F: Enter in the icon table asks for activation; the desktop runs the
   selected object (the System: volume opens its drawer). */
static void wb_icon_keyboard_activate(void)
{
    int index = intuition_icon_selected();

    if (index < 0 || !g_reporting) {
        return;
    }
    g_actions_seen++;
    kprintf("CRUGOS: wb action %u icon %d (keyboard)\n",
            (unsigned)g_actions_seen, index);
    kprintf("CRUGOS: wb icon activated (keyboard)\n");
    if (index == 0) {
        wb_open_drawer();
    }
}

static void wb_key_pump(void)
{
    int ch;

    if ((!g_keys_mode && !g_icon_keys_mode) || g_shell_mode || g_drawer_mode ||
        !g_ready) {
        return;
    }
    while ((ch = input_try_getc()) >= 0) {
        if (g_icon_keys_mode) {
            if (ch >= (int)INPUT_KEY_UP || ch == '\t' || ch == '\n' ||
                ch == '\r' || ch == 0x1b) {
                if (intuition_icon_key((u8)ch)) {
                    wb_icon_keyboard_activate();
                }
            }
            continue;
        }
        if (ch >= (int)INPUT_KEY_UP || ch == '\t' || ch == '\n' ||
            ch == '\r' || ch == 0x1b) {
            intuition_key_event((u8)ch);
        }
    }
}

int wb_init(void)
{
    int top;
    int shell_id;
    u32 width = 0;
    u32 height = 0;

    if (!g_enabled) {
        return 0;
    }
    if (!fb_ready() || fb_dimensions(&width, &height) != 0) {
        kprintf("CRUGOS: wb framebuffer absent\n");
        return -1;
    }
    g_reporting = (g_want_drags > 0u || g_menu_mode || g_keys_mode ||
                   g_shell_mode || g_desk_mode || g_icon_keys_mode ||
                   g_drawer_mode || g_select_mode)
                      ? 1
                      : 0;
    fb_set_text_enabled(0);
    intuition_init();
    intuition_set_reporting(g_reporting);
    top = intuition_open(80, 90, 360, 220, "Workbench",
                         "CrugOS Workbench\nphase 6h\nsoftware rendered");
    shell_id = intuition_open(480, 300, 320, 200, "Shell", "4.OS\ncrugos>");
    intuition_set_menus(top, wb_menus_top, 2u);
    intuition_add_gadget(top, 12, 60, 96, 22, "Run");
    if (g_desk_mode) {
        if (intuition_add_icon(16, 240, "System:", INTUITION_ICON_VOLUME) <
            0) {
            kprintf("CRUGOS: wb icon table full\n");
        }
    }
    intuition_set_menus(shell_id, wb_menus_shell, 2u);
    if (g_shell_mode) {
        g_shell_window = shell_id;
        tg_init(&g_grid);
        tg_puts(&g_grid, "CrugOS Workbench shell");
        tg_putc(&g_grid, '\n');
        shell_init(&g_shell, &g_grid, shell_kernel_io());
        shell_set_sink(&g_shell, wb_shell_sink, 0);
        shell_set_run_hook(&g_shell, wb_shell_run, 0);
        intuition_set_paint(shell_id, wb_shell_paint, 0);
    }
    gfx_set_pointer_hook(wb_report_cursor);
    gfx_show(1);
    input_set_poll(wb_console_poll);
    g_ready = 1;
    kprintf("CRUGOS: wb desktop ready (%ux%u, %u window(s))\n",
            (unsigned)width, (unsigned)height,
            (unsigned)intuition_window_count());
    if (g_shell_mode) {
        kprintf("CRUGOS: wb shell ready (window %d, %ux%u text)\n",
                g_shell_window, (unsigned)(TG_COLS - 1u),
                (unsigned)TG_ROWS_VISIBLE);
    }
    if (g_keys_mode && !g_shell_mode) {
        kprintf("CRUGOS: wb keys ready (window %d active)\n",
                intuition_active());
    }
    if (g_desk_mode) {
        kprintf("CRUGOS: wb icon ready ('System:', volume)\n");
    }
    if (g_icon_keys_mode) {
        kprintf("CRUGOS: wb icons ready (%u object(s), keyboard focus)\n",
                (unsigned)intuition_icon_count());
    }
    if (g_drawer_mode) {
        kprintf("CRUGOS: wb drawer mode (arrow keys scroll, r refreshes)\n");
    }
    if (g_select_mode) {
        kprintf("CRUGOS: wb select mode (click a row, drag the scrollbar)\n");
    }
    return 0;
}

void wb_poll(void)
{
    if (!g_ready) {
        return;
    }
    gfx_pointer_poll();
    wb_key_pump();
    wb_drawer_pump();
    wb_shell_pump();
}

int wb_selftest(void)
{
    u64 deadline;
    u32 want;
    u32 want_actions;
    u32 want_commands;
    u32 want_drawer;
    u32 want_select;

    if (!g_enabled) {
        return 1;
    }
    if (!g_ready) {
        return 1;
    }
    gfx_show(1);
    /* wb.wait=0 turns both gates off: the interactive demo boots the
       desktop and keeps it on screen for wb.hold= seconds instead. */
    if (g_wait_given && g_wait_seconds == 0u) {
        return 1;
    }
    want = g_want_drags;
    want_actions = (g_menu_mode || g_keys_mode || g_icon_keys_mode ||
                    (g_desk_mode && !g_drawer_mode))
                       ? (g_want_actions ? g_want_actions : 1u)
                       : 0u;
    want_commands = g_shell_mode ? (g_want_commands ? g_want_commands : 1u) : 0u;
    /* Drawer mode counts scrolls and refreshes instead of desktop actions, so
       the gate waits for exactly the view moves its driver scripts.  Select
       mode counts row selections plus scrollbar grabs instead. */
    want_drawer = (g_drawer_mode && !g_select_mode)
                      ? (g_want_actions ? g_want_actions : 1u)
                      : 0u;
    want_select = g_select_mode ? (g_want_actions ? g_want_actions : 1u) : 0u;
    deadline = timer_monotonic_ns() +
               (u64)(g_wait_seconds ? g_wait_seconds : 5u) * 1000000000ull;
    while (intuition_drag_count() < (u64)want ||
           g_actions_seen < want_actions ||
           g_drawer_ops_seen < want_drawer ||
           g_select_ops < want_select ||
           g_commands_seen < want_commands) {
        /* input_poll() runs the console hook, which pumps the USB mouse and
           then drains the pointer state into intuition.device; the shell pump
           then feeds whatever the keyboard queued into the Shell window. */
        input_poll();
        wb_drain_events();
        wb_key_pump();
        wb_drawer_pump();
        wb_shell_pump();
        if (timer_monotonic_ns() >= deadline) {
            if (intuition_drag_count() < (u64)want) {
                kprintf("CRUGOS: wb pointer timeout (drag %u/%u)\n",
                        (unsigned)intuition_drag_count(), (unsigned)want);
            } else if (g_actions_seen < want_actions) {
                kprintf("CRUGOS: wb action timeout (%u/%u)\n",
                        (unsigned)g_actions_seen, (unsigned)want_actions);
            } else if (g_drawer_ops_seen < want_drawer) {
                kprintf("CRUGOS: wb drawer timeout (%u/%u)\n",
                        (unsigned)g_drawer_ops_seen, (unsigned)want_drawer);
            } else if (g_select_ops < want_select) {
                kprintf("CRUGOS: wb select timeout (%u/%u)\n",
                        (unsigned)g_select_ops, (unsigned)want_select);
            } else {
                kprintf("CRUGOS: sh command timeout (%u/%u)\n",
                        (unsigned)g_commands_seen, (unsigned)want_commands);
            }
            return 0;
        }
        cpu_wait_for_irq();
    }
    wb_drain_events();
    wb_key_pump();
    wb_shell_pump();
    kprintf("CRUGOS: wb selftest ok (%u drag(s), %u action(s))\n",
            (unsigned)intuition_drag_count(), (unsigned)g_actions_seen);
    if (g_shell_mode) {
        kprintf("CRUGOS: sh selftest ok (%u command(s))\n",
                (unsigned)g_commands_seen);
    }
    if (g_desk_mode) {
        kprintf("CRUGOS: wb desk selftest ok (%u drawer(s))\n",
                (unsigned)g_drawers);
    }
    if (g_icon_keys_mode) {
        kprintf("CRUGOS: wb icons selftest ok (%u move(s), %u front(s))\n",
                (unsigned)intuition_icon_moves(),
                (unsigned)intuition_front_count());
    }
    if (g_drawer_mode) {
        kprintf("CRUGOS: wb drawer selftest ok (%u scroll(s), %u refresh(es))\n",
                (unsigned)g_drawer_scrolls, (unsigned)g_drawer_refreshes);
    }
    if (g_select_mode) {
        kprintf("CRUGOS: wb select selftest ok (%u select(s), %u scrollbar drag(s))\n",
                (unsigned)g_drawer_selects, (unsigned)g_drawer_scrollbar);
    }
    /* Keep the finished desktop on screen for a moment so the gate's last
       screendump does not race the rest of the boot to the exit. */
    deadline = timer_monotonic_ns() + 1500000000ull;
    while (timer_monotonic_ns() < deadline) {
        cpu_wait_for_irq();
    }
    return 1;
}

int wb_ready(void)
{
    return g_ready;
}

/* Interactive demo: keep the booted desktop on screen for wb.hold= seconds
   while still pumping input.device, so the mouse and keyboard stay live. */
void wb_hold(void)
{
    u64 deadline;

    if (!g_ready || g_hold_seconds == 0u) {
        return;
    }
    kprintf("CRUGOS: wb holding desktop for %u s\n",
            (unsigned)g_hold_seconds);
    deadline = timer_monotonic_ns() +
               (u64)g_hold_seconds * 1000000000ull;
    while (timer_monotonic_ns() < deadline) {
        input_poll();
        wb_drain_events();
        wb_key_pump();
        wb_drawer_pump();
        wb_shell_pump();
        cpu_wait_for_irq();
    }
    kprintf("CRUGOS: wb hold finished\n");
}
