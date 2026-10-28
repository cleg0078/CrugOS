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
   commands before the boot finishes. */
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

/* Phase 6E: desktop objects (the System: volume icon) and the drawer window
   that opens on a double click and lists the mounted root. */
static int g_desk_mode;
static int g_drawer_window;
static u32 g_drawers;
static int g_icon_last = -1;
static u64 g_icon_last_ns;
static char g_drawer_list[512];

static void wb_open_drawer(void);

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
    used = wb_prefix(tok, len, "wb.desk=");
    if (used >= 0) {
        if (wb_parse_u32(tok + used, len - (usize)used, &value) == 0) {
            g_desk_mode = value ? 1 : 0;
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
        }
    }
}

/* ---- phase 6E: desktop objects and the drawer ----------------------- */

/* Read the mounted root one entry per line into the painter buffer and count
   them; an absent volume leaves the buffer empty. */
static u32 wb_drawer_entries(void)
{
    struct ext4_fs *fs = storage_root();
    int n;
    int i = 0;
    u32 count = 0;

    g_drawer_list[0] = 0;
    if (!fs) {
        return 0u;
    }
    n = ext4_list(fs, "/", g_drawer_list, sizeof(g_drawer_list));
    if (n <= 0) {
        return 0u;
    }
    while (i < n) {
        if (g_drawer_list[i] != '\n') {
            count++;
        }
        while (i < n && g_drawer_list[i] != '\n') {
            i++;
        }
        i++;
    }
    return count;
}

/* Body painter of the drawer window: the root listing, one name per line. */
static void wb_drawer_paint(int id, i32 x, i32 y, u32 w, u32 h, void *ctx)
{
    u32 line_y = (u32)y + 6u;
    u32 max_chars = w > 12u ? (w - 12u) / 8u : 0u;
    int i = 0;
    u32 drawn = 0;

    (void)id;
    (void)ctx;
    if (max_chars > 0u && max_chars > 47u) {
        max_chars = 47u;
    }
    fb_fill_rect((u32)x, (u32)y, w, h, INTUITION_WINDOW_RGB);
    if (g_drawer_list[0] == 0) {
        fb_draw_text((u32)x + 6u, line_y, "no volume", INTUITION_BODY_TEXT_RGB,
                     INTUITION_WINDOW_RGB);
        return;
    }
    while (i < (int)sizeof(g_drawer_list) && g_drawer_list[i] &&
           line_y + 16u <= (u32)y + h) {
        char text[48];
        u32 n = 0;

        while (g_drawer_list[i] && g_drawer_list[i] != '\n' &&
               n + 1u < sizeof(text)) {
            text[n++] = g_drawer_list[i++];
        }
        text[n] = 0;
        if (g_drawer_list[i] == '\n') {
            i++;
        }
        if (max_chars != 0u && n > max_chars) {
            text[max_chars] = 0;
        }
        fb_draw_text((u32)x + 6u, line_y, text, INTUITION_BODY_TEXT_RGB,
                     INTUITION_WINDOW_RGB);
        line_y += 20u;
        drawn++;
        if (drawn >= 10u) {
            break;
        }
    }
}

static void wb_open_drawer(void)
{
    u32 entries;

    if (g_drawer_window > 0) {
        return;
    }
    entries = wb_drawer_entries();
    g_drawer_window = intuition_open(560, 60, 360, 260, "System:", NULL);
    if (g_drawer_window < 0) {
        return;
    }
    intuition_set_paint(g_drawer_window, wb_drawer_paint, 0);
    g_drawers++;
    if (g_reporting) {
        kprintf("CRUGOS: wb drawer 'System:' opened (%u entries)\n",
                (unsigned)entries);
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
static void wb_key_pump(void)
{
    int ch;

    if (!g_keys_mode || g_shell_mode || !g_ready) {
        return;
    }
    while ((ch = input_try_getc()) >= 0) {
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
                   g_shell_mode || g_desk_mode)
                      ? 1
                      : 0;
    fb_set_text_enabled(0);
    intuition_init();
    intuition_set_reporting(g_reporting);
    top = intuition_open(80, 90, 360, 220, "Workbench",
                         "CrugOS Workbench\nphase 6e\nsoftware rendered");
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
    return 0;
}

void wb_poll(void)
{
    if (!g_ready) {
        return;
    }
    gfx_pointer_poll();
    wb_key_pump();
    wb_shell_pump();
}

int wb_selftest(void)
{
    u64 deadline;
    u32 want;
    u32 want_actions;
    u32 want_commands;

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
    want_actions = (g_menu_mode || g_keys_mode || g_desk_mode)
                       ? (g_want_actions ? g_want_actions : 1u)
                       : 0u;
    want_commands = g_shell_mode ? (g_want_commands ? g_want_commands : 1u) : 0u;
    deadline = timer_monotonic_ns() +
               (u64)(g_wait_seconds ? g_wait_seconds : 5u) * 1000000000ull;
    while (intuition_drag_count() < (u64)want ||
           g_actions_seen < want_actions ||
           g_commands_seen < want_commands) {
        /* input_poll() runs the console hook, which pumps the USB mouse and
           then drains the pointer state into intuition.device; the shell pump
           then feeds whatever the keyboard queued into the Shell window. */
        input_poll();
        wb_drain_events();
        wb_key_pump();
        wb_shell_pump();
        if (timer_monotonic_ns() >= deadline) {
            if (intuition_drag_count() < (u64)want) {
                kprintf("CRUGOS: wb pointer timeout (drag %u/%u)\n",
                        (unsigned)intuition_drag_count(), (unsigned)want);
            } else if (g_actions_seen < want_actions) {
                kprintf("CRUGOS: wb action timeout (%u/%u)\n",
                        (unsigned)g_actions_seen, (unsigned)want_actions);
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
        wb_shell_pump();
        cpu_wait_for_irq();
    }
    kprintf("CRUGOS: wb hold finished\n");
}
