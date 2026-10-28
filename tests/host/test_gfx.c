/* Host tests for the phase 5D pointer-driven framebuffer cursor (kernel/gfx/gfx.c). */
#include <crug/types.h>
#include <crug/kernel.h>
#include <crug/fb.h>
#include <crug/gfx.h>
#include <crug/input.h>
#include <crug/bootinfo.h>
#include <stdio.h>
#include <string.h>

#include "test.h"

/* ---------------------------------------------------------------------- */
/* Framebuffer fake: the cursor sprite itself lives in the arch code, so   */
/* the host build only records where gfx.device asked for it.             */
/* ---------------------------------------------------------------------- */

static int g_fb_ready;
static u32 g_fb_width;
static u32 g_fb_height;
static u32 g_show_x;
static u32 g_show_y;
static int g_show_calls;
static int g_hide_calls;

int fb_ready(void)
{
    return g_fb_ready;
}

int fb_dimensions(u32 *width, u32 *height)
{
    if (!width || !height || !g_fb_ready) {
        return -1;
    }
    *width = g_fb_width;
    *height = g_fb_height;
    return 0;
}

void fb_cursor_show_at(u32 x, u32 y)
{
    g_show_x = x;
    g_show_y = y;
    g_show_calls++;
}

void fb_cursor_hide(void)
{
    g_hide_calls++;
}

int fb_cursor_visible(void)
{
    return 0;
}

u32 fb_cursor_x(void)
{
    return g_show_x;
}

u32 fb_cursor_y(void)
{
    return g_show_y;
}

/* Shared with tests/host/test_intuition.c, which renders the desktop through
 * the same fake framebuffer. */
void test_fb_fake_reset(int ready, u32 width, u32 height)
{
    g_fb_ready = ready;
    g_fb_width = width;
    g_fb_height = height;
    g_show_x = 0;
    g_show_y = 0;
}

void test_fb_cursor_set(u32 x, u32 y)
{
    g_show_x = x;
    g_show_y = y;
}

/* Replaces the HLT idle path (include/crug/cpu.h) so the self-test linger and
 * timeout loops step the shared fake clock instead of sleeping. */
void host_cpu_wait_for_irq(void)
{
    test_clock_advance_ns(100000000ull);
}

static void gfx_setup(int ready, u32 width, u32 height)
{
    g_fb_ready = ready;
    g_fb_width = width;
    g_fb_height = height;
    g_show_x = 0;
    g_show_y = 0;
    g_show_calls = 0;
    g_hide_calls = 0;
    input_pointer_reset();
}

/* The self-test pumps input_poll() once per iteration; the hook below injects a
 * single pointer report per pump, which is exactly how a real mouse arrives. */
static struct {
    i32 dx;
    i32 dy;
} g_script[8];
static int g_script_len;
static int g_script_next;

static void gfx_poll_hook(void)
{
    if (g_script_next < g_script_len) {
        input_pointer_event(0, g_script[g_script_next].dx,
                            g_script[g_script_next].dy, 0);
        g_script_next++;
    }
}

static void gfx_script_begin(i32 dx0, i32 dy0, i32 dx1, i32 dy1, i32 dx2,
                             i32 dy2)
{
    input_pointer_reset();
    g_script[0].dx = dx0;
    g_script[0].dy = dy0;
    g_script[1].dx = dx1;
    g_script[1].dy = dy1;
    g_script[2].dx = dx2;
    g_script[2].dy = dy2;
    g_script_len = 3;
    g_script_next = 0;
}

static void gfx_boot_line(struct boot_info *bi, const char *cmd)
{
    memset(bi, 0, sizeof(*bi));
    bi->command_line = cmd;
    bi->command_line_len = cmd ? (u64)strlen(cmd) : 0u;
}

void test_gfx_functions(void)
{
    struct boot_info bi;
    const struct gfx_cursor *state;

    /* No framebuffer: init fails, nothing is drawn, the self-test is a no-op. */
    gfx_setup(0, 0, 0);
    CHECK(gfx_init() == -1);
    state = gfx_cursor_state();
    CHECK(state != NULL);
    CHECK(state->visible == 0);
    CHECK(gfx_move_count() == 0);
    gfx_pointer_poll();
    CHECK(gfx_move_count() == 0);
    CHECK(gfx_selftest() == 1);
    gfx_show(1);
    CHECK(g_show_calls == 0);

    /* A 1024x768 framebuffer puts an invisible cursor in the centre. */
    gfx_setup(1, 1024, 768);
    CHECK(gfx_init() == 0);
    state = gfx_cursor_state();
    CHECK(state->x == 512);
    CHECK(state->y == 384);
    CHECK(state->visible == 0);
    CHECK(state->buttons == 0);
    CHECK(gfx_move_count() == 0);

    /* Showing and hiding reaches the framebuffer sprite. */
    gfx_show(1);
    CHECK(g_show_calls == 1);
    CHECK(g_show_x == 512);
    CHECK(g_show_y == 384);
    CHECK(gfx_cursor_state()->visible == 1);
    gfx_show(0);
    CHECK(g_hide_calls == 1);
    CHECK(gfx_cursor_state()->visible == 0);

    /* Relative motion moves the cursor by the raw delta (GFX_CURSOR_STEP 1). */
    CHECK(GFX_CURSOR_STEP == 1);
    input_pointer_event(0, 10, -5, 0);
    gfx_pointer_poll();
    state = gfx_cursor_state();
    CHECK(state->x == 522);
    CHECK(state->y == 379);
    CHECK(state->visible == 1);
    CHECK(gfx_move_count() == 1);
    CHECK(g_show_x == 522);
    CHECK(g_show_y == 379);

    /* A wheel-only report never counts as cursor motion. */
    input_pointer_event(0, 0, 0, 3);
    gfx_pointer_poll();
    CHECK(gfx_move_count() == 1);
    CHECK(gfx_cursor_state()->x == 522);

    /* Motion is clamped to the last pixel, not past the screen. */
    input_pointer_event(0, 5000, 0, 0);
    gfx_pointer_poll();
    CHECK(gfx_cursor_state()->x == 1023);
    CHECK(gfx_cursor_state()->y == 379);
    input_pointer_event(0, -10000, -10000, 0);
    gfx_pointer_poll();
    CHECK(gfx_cursor_state()->x == 0);
    CHECK(gfx_cursor_state()->y == 0);
    input_pointer_event(0, 0, 20000, 0);
    gfx_pointer_poll();
    CHECK(gfx_cursor_state()->y == 767);
    CHECK(gfx_move_count() == 4);

    /* Button transitions are tracked through input.device changes. */
    input_pointer_event(0x01u, 0, 0, 0);
    gfx_pointer_poll();
    CHECK(gfx_cursor_state()->buttons == 0x01u);
    CHECK(gfx_move_count() == 4);
    input_pointer_event(0x00u, 0, 0, 0);
    gfx_pointer_poll();
    CHECK(gfx_cursor_state()->buttons == 0u);

    /* No gate asked for motion, so the self-test stays silent. */
    gfx_setup(1, 1024, 768);
    CHECK(gfx_init() == 0);
    CHECK(gfx_selftest() == 1);
    CHECK(g_show_calls == 0);

    /* gfx.moves=3 waits for three pointer reports, including a trailing newline. */
    gfx_setup(1, 1024, 768);
    CHECK(gfx_init() == 0);
    gfx_boot_line(&bi, "gfx.wait=2 gfx.moves=3\n");
    gfx_cmdline(&bi);
    gfx_script_begin(3, 4, -1, 2, 0, 7);
    input_set_poll(gfx_poll_hook);
    CHECK(gfx_selftest() == 1);
    input_set_poll(NULL);
    CHECK(g_script_next == 3);
    state = gfx_cursor_state();
    CHECK(gfx_move_count() == 3);
    CHECK(state->x == 512 + 3 - 1);
    CHECK(state->y == 384 + 4 + 2 + 7);

    /* Fewer reports than requested time out once the fake clock passes the wait. */
    gfx_setup(1, 1024, 768);
    CHECK(gfx_init() == 0);
    gfx_boot_line(&bi, "gfx.wait=1 gfx.moves=5");
    gfx_cmdline(&bi);
    input_pointer_event(0, 1, 1, 0);
    CHECK(gfx_selftest() == 0);
    CHECK(gfx_move_count() == 1);

    /* Unparsable tokens leave the defaults in place. */
    gfx_setup(1, 1024, 768);
    CHECK(gfx_init() == 0);
    gfx_boot_line(&bi, "gfx.wait=abc gfx.moves=-4 ignored");
    gfx_cmdline(&bi);
    CHECK(gfx_selftest() == 1);
    gfx_cmdline(NULL);

    /* The parser caps gfx.wait at GFX_WAIT_MAX and gfx.moves at GFX_MOVES_MAX,
     * so an absurd request still ends in a bounded timeout, not a spin. */
    gfx_setup(1, 1024, 768);
    CHECK(gfx_init() == 0);
    gfx_boot_line(&bi, "gfx.wait=9999 gfx.moves=9999");
    gfx_cmdline(&bi);
    input_pointer_event(0, 1, 0, 0);
    CHECK(gfx_selftest() == 0);
    CHECK(gfx_move_count() == 1);
}
