/* gfx.device: pointer-driven framebuffer cursor (phase 5D).
  
   The device owns one cursor on the GOP framebuffer. Pointer events from
   input.device (a USB HID boot mouse) move it by their relative deltas,
   clamped to the screen; the framebuffer console saves the pixels underneath
   the sprite and restores them around text writes, so the cursor never eats a
   glyph. A boot-time self-test waits for real motion so the hotplug gate can
   prove the whole path mouse -> input.device -> gfx.device -> framebuffer. */
#include <crug/types.h>
#include <crug/kernel.h>
#include <crug/fb.h>
#include <crug/gfx.h>
#include <crug/input.h>
#include <crug/timer.h>
#include <crug/cpu.h>

static struct gfx_cursor g_cursor;
static u32 g_width;
static u32 g_height;
static u32 g_wait_seconds;
static u32 g_want_moves;
static int g_ready;
static int g_reporting;  /* a gate wants one marker per move */

int gfx_init(void)
{
    u32 width = 0;
    u32 height = 0;

    g_ready = 0;
    g_cursor.x = 0;
    g_cursor.y = 0;
    g_cursor.visible = 0;
    g_cursor.moves = 0;
    g_cursor.buttons = 0;
    g_wait_seconds = 0;
    g_want_moves = 0;
    g_reporting = 0;
    g_width = 0;
    g_height = 0;
    if (!fb_ready() || fb_dimensions(&width, &height) != 0) {
        kprintf("CRUGOS: gfx framebuffer absent\n");
        return -1;
    }
    g_width = width;
    g_height = height;
    g_cursor.x = (i32)(width / 2u);
    g_cursor.y = (i32)(height / 2u);
    g_ready = 1;
    kprintf("CRUGOS: gfx ready (%ux%u, cursor at %d,%d)\n", width, height,
            (int)g_cursor.x, (int)g_cursor.y);
    return 0;
}

void gfx_show(int visible)
{
    if (!g_ready) {
        return;
    }
    if (visible) {
        fb_cursor_show_at((u32)g_cursor.x, (u32)g_cursor.y);
        g_cursor.visible = 1;
    } else {
        fb_cursor_hide();
        g_cursor.visible = 0;
    }
}

u64 gfx_move_count(void)
{
    return g_cursor.moves;
}

const struct gfx_cursor *gfx_cursor_state(void)
{
    return &g_cursor;
}

static int gfx_prefix(const char *tok, u32 len, const char *prefix)
{
    u32 i = 0;

    while (prefix[i]) {
        if (i >= len || tok[i] != prefix[i]) {
            return 0;
        }
        i++;
    }
    return 1;
}

static u32 gfx_parse_u32(const char *text, u32 len, u32 max)
{
    u64 value = 0;
    u32 i;

    if (len == 0) {
        return 0;
    }
    for (i = 0; i < len; i++) {
        char c = text[i];

        if (c < '0' || c > '9') {
            return 0;
        }
        value = value * 10u + (u32)(c - '0');
        if (value > max) {
            return max;
        }
    }
    return (u32)value;
}

static void gfx_cmdline_token(const char *tok, u32 len)
{
    if (gfx_prefix(tok, len, "gfx.wait=")) {
        g_wait_seconds = gfx_parse_u32(tok + 9, len - 9, GFX_WAIT_MAX);
    } else if (gfx_prefix(tok, len, "gfx.moves=")) {
        g_want_moves = gfx_parse_u32(tok + 10, len - 10, GFX_MOVES_MAX);
    }
}

/* The loader hands over the raw file contents, so a trailing newline can end
   the last token; treat every ASCII space as a separator. */
static int gfx_is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

void gfx_cmdline(const boot_info *bi)
{
    const char *cmd = bi ? bi->command_line : 0;
    u64 len = bi ? bi->command_line_len : 0;
    u64 i = 0;

    while (cmd && i < len) {
        u64 start;

        while (i < len && gfx_is_space(cmd[i])) {
            i++;
        }
        if (i >= len) {
            break;
        }
        start = i;
        while (i < len && !gfx_is_space(cmd[i])) {
            i++;
        }
        gfx_cmdline_token(cmd + start, (u32)(i - start));
    }
}

static void gfx_clamp(i32 *x, i32 *y)
{
    if (*x < 0) {
        *x = 0;
    } else if ((u32)*x >= g_width) {
        *x = (i32)(g_width - 1u);
    }
    if (*y < 0) {
        *y = 0;
    } else if ((u32)*y >= g_height) {
        *y = (i32)(g_height - 1u);
    }
}

void gfx_pointer_poll(void)
{
    struct input_pointer event;

    if (!g_ready) {
        return;
    }
    while (input_pointer_read(&event)) {
        if (event.dx != 0 || event.dy != 0) {
            i32 x = g_cursor.x + event.dx * GFX_CURSOR_STEP;
            i32 y = g_cursor.y + event.dy * GFX_CURSOR_STEP;

            gfx_clamp(&x, &y);
            g_cursor.x = x;
            g_cursor.y = y;
            g_cursor.moves++;
            gfx_show(1);
            if (g_reporting) {
                kprintf("CRUGOS: gfx cursor at (%d, %d)\n", (int)x, (int)y);
            }
        }
        if (event.changes != 0) {
            g_cursor.buttons = event.buttons;
            if (g_reporting) {
                kprintf("CRUGOS: gfx pointer buttons 0x%02x\n",
                        (unsigned)event.buttons);
            }
        }
    }
}

int gfx_selftest(void)
{
    u64 deadline;
    u32 want;

    if (!g_ready) {
        /* gfx_init() already reported the missing framebuffer. */
        return 1;
    }
    if (g_wait_seconds == 0u && g_want_moves == 0u) {
        return 1;
    }
    g_cursor.moves = 0;
    gfx_show(1);
    g_reporting = 1;
    kprintf("CRUGOS: gfx cursor shown at (%d, %d)\n", (int)g_cursor.x,
            (int)g_cursor.y);
    want = g_want_moves ? g_want_moves : 1u;
    deadline = timer_monotonic_ns() +
               (u64)(g_wait_seconds ? g_wait_seconds : 5u) * 1000000000ull;
    while (g_cursor.moves < want) {
        input_poll();
        gfx_pointer_poll();
        if (timer_monotonic_ns() >= deadline) {
            kprintf("CRUGOS: gfx pointer timeout (moves %u/%u)\n",
                    (unsigned)g_cursor.moves, (unsigned)want);
            return 0;
        }
        cpu_wait_for_irq();
    }
    kprintf("CRUGOS: gfx selftest ok (%u move(s), buttons 0x%02x)\n",
            (unsigned)g_cursor.moves, (unsigned)g_cursor.buttons);
    if (g_reporting) {
        /* A gate wants to capture the final frame; keep the cursor on screen
           for a moment instead of racing the rest of the boot to the exit. */
        deadline = timer_monotonic_ns() + 1500000000ull;
        while (timer_monotonic_ns() < deadline) {
            cpu_wait_for_irq();
        }
    }
    return 1;
}
