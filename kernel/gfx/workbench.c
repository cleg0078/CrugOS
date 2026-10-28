/* Workbench desktop bring-up (phase 6A).
   The desktop is a gate-only mode for now: wb.enable=1 takes the framebuffer
   away from the text console (which keeps writing to the serial port), opens
   two windows through intuition.device, and waits for a title-bar drag so the
   hotplug gate can prove the event path with a screendump comparison. */
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
#include <crug/workbench.h>

static int g_enabled;
static int g_ready;
static int g_reporting;
static u32 g_wait_seconds;
static int g_wait_given;
static u32 g_want_drags;
static u32 g_hold_seconds;

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

int wb_init(void)
{
    u32 width = 0;
    u32 height = 0;

    if (!g_enabled) {
        return 0;
    }
    if (!fb_ready() || fb_dimensions(&width, &height) != 0) {
        kprintf("CRUGOS: wb framebuffer absent\n");
        return -1;
    }
    g_reporting = g_want_drags > 0u;
    fb_set_text_enabled(0);
    intuition_init();
    intuition_set_reporting(g_reporting);
    intuition_open(80, 90, 360, 220, "Workbench",
                   "CrugOS Workbench\nphase 6a\nsoftware rendered");
    intuition_open(480, 300, 320, 200, "Shell", "4.OS\ncrugos>");
    gfx_set_pointer_hook(wb_report_cursor);
    gfx_show(1);
    input_set_poll(wb_console_poll);
    g_ready = 1;
    kprintf("CRUGOS: wb desktop ready (%ux%u, %u window(s))\n",
            (unsigned)width, (unsigned)height,
            (unsigned)intuition_window_count());
    return 0;
}

void wb_poll(void)
{
    if (!g_ready) {
        return;
    }
    gfx_pointer_poll();
}

int wb_selftest(void)
{
    u64 deadline;
    u32 want;

    if (!g_enabled) {
        return 1;
    }
    if (!g_ready) {
        return 1;
    }
    gfx_show(1);
    /* wb.wait=0 turns the drag gate off: the interactive demo boots the
       desktop and keeps it on screen for wb.hold= seconds instead. */
    if (g_wait_given && g_wait_seconds == 0u) {
        return 1;
    }
    want = g_want_drags ? g_want_drags : 1u;
    deadline = timer_monotonic_ns() +
               (u64)(g_wait_seconds ? g_wait_seconds : 5u) * 1000000000ull;
    while (intuition_drag_count() < (u64)want) {
        /* input_poll() runs the console hook, which pumps the USB mouse and
           then drains the pointer state into intuition.device. */
        input_poll();
        if (timer_monotonic_ns() >= deadline) {
            kprintf("CRUGOS: wb pointer timeout (drag %u/%u)\n",
                    (unsigned)intuition_drag_count(), (unsigned)want);
            return 0;
        }
        cpu_wait_for_irq();
    }
    kprintf("CRUGOS: wb selftest ok (%u drag(s))\n",
            (unsigned)intuition_drag_count());
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
        cpu_wait_for_irq();
    }
    kprintf("CRUGOS: wb hold finished\n");
}
