/* Linear framebuffer text console (8x16 glyphs). */
#include <crug/types.h>
#include <crug/string.h>
#include <crug/kernel.h>
#include <crug/fb.h>
#include <crug/gpu.h>
#include <crug/color.h>
#include <crug/paint.h>

#define FB_GLYPH_W 8
#define FB_GLYPH_H 16
#define FB_COLOR_BG 0x00000000u
#define FB_COLOR_FG 0x00E0E0E0u

extern const u8 crug_font8x16[128][16];

static framebuffer_info g_fb;
static int g_ready;
static int g_text_enabled;
static u32 g_cols;
static u32 g_rows;
static u32 g_x;
static u32 g_y;

/* Phase 8A: present callback and damage rectangle.  The GPU driver installs a
   callback that transfers only the damaged pixels; the plain linear
   framebuffer leaves it NULL and every flush is a no-op. */
static fb_present_fn g_present;
static u32 g_present_depth;
static struct gpu_damage g_dmg;

static void fb_present_flush(void)
{
    struct gpu_rect r;

    if (!g_present || g_present_depth) {
        return;
    }
    if (!gpu_damage_rect(&g_dmg, &r)) {
        return;
    }
    gpu_damage_reset(&g_dmg);
    g_present(r.x, r.y, r.w, r.h);
}

void fb_present(void)
{
    fb_present_flush();
}

void fb_present_begin(void)
{
    g_present_depth++;
}

void fb_present_end(void)
{
    if (g_present_depth) {
        g_present_depth--;
    }
    fb_present_flush();
}

static u32 fb_pixel_color(u32 rgb)
{
    u32 r = (rgb >> 16) & 0xFF;
    u32 g = (rgb >> 8) & 0xFF;
    u32 b = rgb & 0xFF;

    /* BGRX8888 stores blue in the lowest byte, so the natural
       (r << 16) | (g << 8) | b packing already matches it.  RGBX8888
       stores red there instead and needs the swap. */
    if (g_fb.format == CRUG_PIXEL_RGBX8888) {
        return (b << 16) | (g << 8) | r;
    }
    return (r << 16) | (g << 8) | b;
}

static void fb_store(u32 x, u32 y, u32 color)
{
    volatile u32 *row = (volatile u32 *)(uintptr)(g_fb.base + (u64)y * g_fb.stride * 4);
    row[x] = color;
    gpu_damage_add(&g_dmg, x, y, 1u, 1u);
}

static u32 fb_read(u32 x, u32 y)
{
    volatile u32 *row = (volatile u32 *)(uintptr)(g_fb.base + (u64)y * g_fb.stride * 4);
    return row[x];
}

/* Pointer sprite: '#' is the one-pixel outline, 'O' the body, '.' transparent. */
#define FB_CURSOR_BORDER 1u
#define FB_CURSOR_BODY 2u
#define FB_CURSOR_BORDER_RGB 0x00FFFFFFu
#define FB_CURSOR_BODY_RGB 0x00FFD040u

static const u8 g_cursor_shape[FB_CURSOR_HEIGHT][FB_CURSOR_WIDTH] = {
    { 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 1, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 1, 2, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 1, 2, 2, 2, 1, 0, 0, 0, 0, 0, 0, 0 },
    { 1, 2, 2, 2, 2, 1, 0, 0, 0, 0, 0, 0 },
    { 1, 2, 2, 2, 2, 2, 1, 0, 0, 0, 0, 0 },
    { 1, 2, 2, 2, 2, 2, 2, 1, 0, 0, 0, 0 },
    { 1, 2, 2, 2, 2, 2, 2, 2, 1, 0, 0, 0 },
    { 1, 2, 2, 2, 2, 2, 2, 2, 2, 1, 0, 0 },
    { 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 0 },
    { 1, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1 },
    { 1, 2, 2, 1, 2, 2, 2, 1, 0, 0, 0, 0 },
    { 1, 2, 1, 0, 1, 2, 2, 2, 1, 0, 0, 0 },
    { 1, 1, 0, 0, 1, 2, 2, 2, 1, 0, 0, 0 },
    { 1, 0, 0, 0, 0, 1, 2, 2, 2, 1, 0, 0 },
    { 0, 0, 0, 0, 0, 1, 2, 2, 2, 1, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 1, 2, 2, 1, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0 },
};

static u32 g_cursor_x;
static u32 g_cursor_y;
static u32 g_cursor_saved[FB_CURSOR_HEIGHT][FB_CURSOR_WIDTH];
static int g_cursor_wanted;  /* the consumer asked for a cursor */
static int g_cursor_drawn;   /* the sprite is currently in the framebuffer */
static const struct fb_cursor_ops *g_cursor_ops;  /* phase 8B: device cursor */
static int g_cursor_hw_shown;                     /* the device cursor is visible */

static void fb_cursor_composite(void)
{
    u32 row, col;

    if (!g_ready || !g_cursor_wanted || g_cursor_drawn || g_cursor_ops) {
        return;
    }
    for (row = 0; row < FB_CURSOR_HEIGHT; row++) {
        for (col = 0; col < FB_CURSOR_WIDTH; col++) {
            u32 x = g_cursor_x + col;
            u32 y = g_cursor_y + row;
            u8 shape;

            if (x >= g_fb.width || y >= g_fb.height) {
                continue;
            }
            g_cursor_saved[row][col] = fb_read(x, y);
            shape = g_cursor_shape[row][col];
            if (shape == FB_CURSOR_BORDER) {
                fb_store(x, y, fb_pixel_color(FB_CURSOR_BORDER_RGB));
            } else if (shape == FB_CURSOR_BODY) {
                fb_store(x, y, fb_pixel_color(FB_CURSOR_BODY_RGB));
            }
        }
    }
    g_cursor_drawn = 1;
}

/* Put the saved pixels back before anything else writes over them. */
static void fb_cursor_restore(void)
{
    u32 row, col;

    if (!g_cursor_drawn) {
        return;
    }
    for (row = 0; row < FB_CURSOR_HEIGHT; row++) {
        for (col = 0; col < FB_CURSOR_WIDTH; col++) {
            u32 x = g_cursor_x + col;
            u32 y = g_cursor_y + row;

            if (x >= g_fb.width || y >= g_fb.height) {
                continue;
            }
            fb_store(x, y, g_cursor_saved[row][col]);
        }
    }
    g_cursor_drawn = 0;
}

static void fb_fill_row(u32 y, u32 color)
{
    u32 x;
    for (x = 0; x < g_fb.width; x++) {
        fb_store(x, y, color);
    }
}

static void fb_clear(void)
{
    u32 y;

    fb_cursor_restore();
    for (y = 0; y < g_fb.height; y++) {
        fb_fill_row(y, fb_pixel_color(FB_COLOR_BG));
    }
    g_x = 0;
    g_y = 0;
    fb_cursor_composite();
    fb_present_flush();
}

void fb_init(const framebuffer_info *fb)
{
    g_ready = 0;
    g_present = 0;
    g_present_depth = 0;
    gpu_damage_reset(&g_dmg);
    g_cursor_wanted = 0;
    g_cursor_drawn = 0;
    g_cursor_hw_shown = 0;
    g_cursor_x = 0;
    g_cursor_y = 0;
    if (!fb || fb->base == 0 || fb->width == 0 || fb->height == 0 ||
        fb->stride < fb->width) {
        return;
    }
    g_fb = *fb;
    g_text_enabled = 1;
    g_cols = g_fb.width / FB_GLYPH_W;
    g_rows = g_fb.height / FB_GLYPH_H;
    if (g_cols == 0 || g_rows == 0) {
        return;
    }
    g_ready = 1;
    fb_clear();
}

int fb_ready(void)
{
    return g_ready;
}

static void fb_scroll(void)
{
    u32 y;
    usize line_bytes = (usize)g_fb.stride * 4;

    fb_cursor_restore();
    for (y = 0; y + FB_GLYPH_H < g_fb.height; y++) {
        memcpy((void *)(uintptr)(g_fb.base + (u64)y * line_bytes),
               (void *)(uintptr)(g_fb.base + (u64)(y + FB_GLYPH_H) * line_bytes),
               line_bytes);
    }
    for (; y < g_fb.height; y++) {
        fb_fill_row(y, fb_pixel_color(FB_COLOR_BG));
    }
    fb_cursor_composite();
    fb_present_flush();
}

static void fb_draw_glyph(u32 cell_x, u32 cell_y, unsigned char c)
{
    const u8 *glyph = crug_font8x16[c < 128 ? c : (unsigned char)'?'];
    u32 row, col;
    u32 x0 = cell_x * FB_GLYPH_W;
    u32 y0 = cell_y * FB_GLYPH_H;

    for (row = 0; row < FB_GLYPH_H; row++) {
        u8 bits = glyph[row];
        for (col = 0; col < FB_GLYPH_W; col++) {
            u32 color = (bits & (0x80u >> col)) ? fb_pixel_color(FB_COLOR_FG)
                                                : fb_pixel_color(FB_COLOR_BG);
            fb_store(x0 + col, y0 + row, color);
        }
    }
}

static void fb_putc_raw(char ch)
{
    unsigned char c = (unsigned char)ch;

    if (!g_ready) {
        return;
    }

    if (c == '\n') {
        g_x = 0;
        g_y++;
    } else if (c == '\r') {
        g_x = 0;
        return;
    } else if (c == '\t') {
        u32 i;
        for (i = 0; i < 4; i++) {
            fb_putc_raw(' ');
        }
        return;
    } else {
        fb_draw_glyph(g_x, g_y, c);
        g_x++;
        if (g_x >= g_cols) {
            g_x = 0;
            g_y++;
        }
    }

    while (g_y >= g_rows) {
        fb_scroll();
        g_y--;
    }
}

void fb_putc(char ch)
{
    if (!g_ready || !g_text_enabled) {
        return;
    }
    /* Take the sprite out of the framebuffer before the glyph (or a scroll)
       overwrites the pixels it saved, then put it back on top. */
    fb_cursor_restore();
    fb_putc_raw(ch);
    fb_cursor_composite();
    /* Flushing once per line keeps boot output readable without one device
       round trip per glyph; the desktop primitives flush on their own. */
    if (ch == 0x0A) {
        fb_present_flush();
    }
}

int fb_dimensions(u32 *width, u32 *height)
{
    if (!g_ready || !width || !height) {
        return -1;
    }
    *width = g_fb.width;
    *height = g_fb.height;
    return 0;
}

void fb_cursor_show_at(u32 x, u32 y)
{
    int ok;

    if (!g_ready) {
        return;
    }
    fb_cursor_restore();
    if (g_cursor_ops) {
        ok = g_cursor_hw_shown ? g_cursor_ops->move(x, y) : g_cursor_ops->show(x, y);
        if (ok == 0) {
            g_cursor_x = x;
            g_cursor_y = y;
            g_cursor_wanted = 1;
            g_cursor_hw_shown = 1;
            return;
        }
        /* The device refused the cursor: fall back to the software sprite. */
        g_cursor_ops = 0;
        g_cursor_hw_shown = 0;
    }
    g_cursor_x = x;
    g_cursor_y = y;
    g_cursor_wanted = 1;
    fb_cursor_composite();
    fb_present_flush();
}

void fb_cursor_hide(void)
{
    fb_cursor_restore();
    if (g_cursor_ops && g_cursor_hw_shown) {
        g_cursor_ops->hide();
        g_cursor_hw_shown = 0;
    }
    g_cursor_wanted = 0;
    fb_present_flush();
}

int fb_cursor_visible(void)
{
    if (g_cursor_ops && g_cursor_wanted) {
        return 1;
    }
    return g_cursor_drawn;
}

u32 fb_cursor_x(void)
{
    return g_cursor_x;
}

u32 fb_cursor_y(void)
{
    return g_cursor_y;
}

void fb_cursor_begin(void)
{
    fb_cursor_restore();
}

void fb_cursor_end(void)
{
    fb_cursor_composite();
}

void fb_set_text_enabled(int enabled)
{
    g_text_enabled = enabled ? 1 : 0;
}

int fb_text_enabled(void)
{
    return g_text_enabled;
}

/* Forward declaration: the clipped fill helper below feeds fb_fill_raw. */
static void fb_fill_raw(u32 x, u32 y, u32 w, u32 h,
                        const framebuffer_info *fb, u32 color);

/* --- phase UI: clip stack, blending and the desktop primitives --- */

static u32 g_clip[FB_CLIP_MAX][4]; /* x, y, w, h, already intersected */
static u32 g_clip_depth;

static void fb_clip_bounds(u32 *x, u32 *y, u32 *w, u32 *h)
{
    u32 cx = 0u, cy = 0u, cw = g_fb.width, ch = g_fb.height;
    u32 x1, y1, x2, y2;

    if (g_clip_depth) {
        cx = g_clip[g_clip_depth - 1u][0];
        cy = g_clip[g_clip_depth - 1u][1];
        cw = g_clip[g_clip_depth - 1u][2];
        ch = g_clip[g_clip_depth - 1u][3];
    }
    x1 = *x > cx ? *x : cx;
    y1 = *y > cy ? *y : cy;
    x2 = (*x + *w) < (cx + cw) ? (*x + *w) : (cx + cw);
    y2 = (*y + *h) < (cy + ch) ? (*y + *h) : (cy + ch);
    if (x2 <= x1 || y2 <= y1) {
        *x = x1;
        *y = y1;
        *w = 0u;
        *h = 0u;
        return;
    }
    *x = x1;
    *y = y1;
    *w = x2 - x1;
    *h = y2 - y1;
}

int fb_clip_push(u32 x, u32 y, u32 w, u32 h)
{
    if (!g_ready || g_clip_depth >= FB_CLIP_MAX) {
        return -1;
    }
    fb_clip_bounds(&x, &y, &w, &h);
    g_clip[g_clip_depth][0] = x;
    g_clip[g_clip_depth][1] = y;
    g_clip[g_clip_depth][2] = w;
    g_clip[g_clip_depth][3] = h;
    g_clip_depth++;
    return 0;
}

void fb_clip_pop(void)
{
    if (g_clip_depth) {
        g_clip_depth--;
    }
}

void fb_clip_reset(void)
{
    g_clip_depth = 0u;
}

int fb_clip_depth(void)
{
    return (int)g_clip_depth;
}

static int fb_inside(u32 x, u32 y)
{
    const u32 *c;

    if (x >= g_fb.width || y >= g_fb.height) {
        return 0;
    }
    if (!g_clip_depth) {
        return 1;
    }
    c = g_clip[g_clip_depth - 1u];
    return x >= c[0] && y >= c[1] && x < c[0] + c[2] && y < c[1] + c[3];
}

/* One clipped, unblended fill; callers hold the cursor save bracket. */
static void fb_rect_rgb(u32 x, u32 y, u32 w, u32 h, u32 rgb)
{
    u32 cx = x, cy = y, cw = w, ch = h;

    if (!w || !h) {
        return;
    }
    fb_clip_bounds(&cx, &cy, &cw, &ch);
    if (!cw || !ch) {
        return;
    }
    fb_fill_raw(cx, cy, cw, ch, &g_fb, fb_pixel_color(rgb));
}

/* One clipped, unblended blend; callers hold the cursor save bracket. */
static void fb_blend_raw(u32 x, u32 y, u32 w, u32 h, u32 rgb, u32 alpha)
{
    u32 cx = x, cy = y, cw = w, ch = h;
    u32 row, col;

    if (!w || !h || !alpha) {
        return;
    }
    fb_clip_bounds(&cx, &cy, &cw, &ch);
    if (!cw || !ch) {
        return;
    }
    for (row = cy; row < cy + ch; row++) {
        for (col = cx; col < cx + cw; col++) {
            fb_store(col, row,
                     color_blend_device(fb_read(col, row), rgb, alpha,
                                        g_fb.format));
        }
    }
}
static void fb_fill_raw(u32 x, u32 y, u32 w, u32 h, const framebuffer_info *fb,
                        u32 color)
{
    u32 row, col;

    if (!fb || x >= fb->width || y >= fb->height) {
        return;
    }
    for (row = y; row < fb->height && row - y < h; row++) {
        for (col = x; col < fb->width && col - x < w; col++) {
            fb_store(col, row, color);
        }
    }
}

void fb_fill_rect(u32 x, u32 y, u32 w, u32 h, u32 rgb)
{
    if (!g_ready || !w || !h) {
        return;
    }
    fb_cursor_restore();
    fb_rect_rgb(x, y, w, h, rgb);
    fb_cursor_composite();
    fb_present_flush();
}

void fb_frame_rect(u32 x, u32 y, u32 w, u32 h, u32 rgb)
{
    if (!g_ready || w < 2u || h < 2u) {
        return;
    }
    fb_cursor_restore();
    fb_rect_rgb(x, y, w, 1u, rgb);
    fb_rect_rgb(x, y + h - 1u, w, 1u, rgb);
    fb_rect_rgb(x, y, 1u, h, rgb);
    fb_rect_rgb(x + w - 1u, y, 1u, h, rgb);
    fb_cursor_composite();
    fb_present_flush();
}

static void fb_glyph_px(u32 x0, u32 y0, unsigned char c, u32 fg, u32 bg)
{
    const u8 *glyph = crug_font8x16[c < 128 ? c : (unsigned char)'?'];
    u32 row, col;

    for (row = 0; row < FB_GLYPH_H; row++) {
        u8 bits = glyph[row];
        for (col = 0; col < FB_GLYPH_W; col++) {
            u32 color = (bits & (0x80u >> col)) ? fg : bg;
            if (fb_inside(x0 + col, y0 + row)) {
                fb_store(x0 + col, y0 + row, color);
            }
        }
    }
}

void fb_draw_text(u32 x, u32 y, const char *text, u32 fg, u32 bg)
{
    u32 fg_color, bg_color;

    if (!g_ready || !text || y >= g_fb.height) {
        return;
    }
    fg_color = fb_pixel_color(fg);
    bg_color = fb_pixel_color(bg);
    fb_cursor_restore();
    while (*text) {
        if (x + FB_GLYPH_W > g_fb.width) {
            break;
        }
        fb_glyph_px(x, y, (unsigned char)*text, fg_color, bg_color);
        x += FB_GLYPH_W;
        text++;
    }
    fb_cursor_composite();
    fb_present_flush();
}
/* --- phase UI: composite primitives built on the clip stack --------------- */

/* The colour formats in include/crug/color.h mirror the boot pixel formats;
   a drift here would silently swap red and blue. */
_Static_assert(CRUG_COLOR_RGBX == (u32)CRUG_PIXEL_RGBX8888,
              "color.h RGBX constant drifted from the boot format");
_Static_assert(CRUG_COLOR_BGRX == (u32)CRUG_PIXEL_BGRX8888,
              "color.h BGRX constant drifted from the boot format");

void fb_blend_rect(u32 x, u32 y, u32 w, u32 h, u32 rgb, u32 alpha)
{
    if (!g_ready || !w || !h || !alpha) {
        return;
    }
    fb_cursor_restore();
    fb_blend_raw(x, y, w, h, rgb, alpha);
    fb_cursor_composite();
    fb_present_flush();
}

void fb_round_rect(u32 x, u32 y, u32 w, u32 h, u32 radius, u32 fill, u32 border)
{
    u32 row;

    if (!g_ready || !w || !h) {
        return;
    }
    if (radius > w / 2u) {
        radius = w / 2u;
    }
    if (radius > h / 2u) {
        radius = h / 2u;
    }
    fb_cursor_restore();
    for (row = 0u; row < h; row++) {
        u32 dy;
        u32 inset;
        u32 sx;
        u32 sw;

        /* paint_round_inset() counts dy from the outermost corner row: the
           first rows are cut back by the circle, the straight middle rows
           are not inset at all, so the border lands on the rectangle edge. */
        if (row < radius) {
            dy = row;
        } else if (row + radius >= h) {
            dy = h - 1u - row;
        } else {
            dy = radius;
        }
        inset = paint_round_inset(radius, dy);
        if (inset * 2u >= w) {
            continue;
        }
        sx = x + inset;
        sw = w - 2u * inset;
        if (fill) {
            fb_rect_rgb(sx, y + row, sw, 1u, fill);
        }
        if (border) {
            fb_rect_rgb(sx, y + row, 1u, 1u, border);
            fb_rect_rgb(sx + sw - 1u, y + row, 1u, 1u, border);
        }
    }
    if (border) {
        u32 top_inset = paint_round_inset(radius, 0u);
        u32 top_w = w > 2u * top_inset ? w - 2u * top_inset : 0u;

        if (top_w) {
            fb_rect_rgb(x + top_inset, y, top_w, 1u, border);
            fb_rect_rgb(x + top_inset, y + h - 1u, top_w, 1u, border);
        }
    }
    fb_cursor_composite();
    fb_present_flush();
}

void fb_vgradient_rect(u32 x, u32 y, u32 w, u32 h, u32 top_rgb, u32 bottom_rgb)
{
    u32 row;

    if (!g_ready || !w || !h) {
        return;
    }
    fb_cursor_restore();
    for (row = 0u; row < h; row++) {
        fb_rect_rgb(x, y + row, w, 1u,
                    paint_gradient_rgb(top_rgb, bottom_rgb, row, h));
    }
    fb_cursor_composite();
    fb_present_flush();
}

void fb_shadow_box(u32 x, u32 y, u32 w, u32 h, u32 depth)
{
    u32 i;

    if (!g_ready || !w || !h || !depth) {
        return;
    }
    if (depth > 8u) {
        depth = 8u;
    }
    fb_cursor_restore();
    for (i = 0u; i < depth; i++) {
        u32 alpha = paint_shadow_alpha(i, depth, 96u, 24u);
        u32 height = h > i ? h - i : 1u;
        u32 width = w > i ? w - i : 1u;

        /* One column to the right and one row below the box, shifted a
           little further out per band so the edge looks soft. */
        fb_blend_raw(x + w + i, y + 2u + i, 1u, height, 0x00000000u, alpha);
        fb_blend_raw(x + 2u + i, y + h + i, width, 1u, 0x00000000u, alpha);
    }
    fb_cursor_composite();
    fb_present_flush();
}

void fb_blit_keyed(u32 x, u32 y, u32 w, u32 h, const u32 *src, u32 stride_px,
                   u32 key_rgb)
{
    u32 row, col;

    if (!g_ready || !src || !w || !h) {
        return;
    }
    key_rgb &= 0x00FFFFFFu;
    fb_cursor_restore();
    for (row = 0u; row < h; row++) {
        for (col = 0u; col < w; col++) {
            u32 px = src[row * stride_px + col] & 0x00FFFFFFu;
            if (px == key_rgb || !fb_inside(x + col, y + row)) {
                continue;
            }
            fb_store(x + col, y + row, fb_pixel_color(px));
        }
    }
    fb_cursor_composite();
    fb_present_flush();
}

void fb_blit(u32 x, u32 y, u32 w, u32 h, const u32 *src, u32 stride_px)
{
    u32 row, col;

    if (!g_ready || !src || !w || !h) {
        return;
    }
    fb_cursor_restore();
    for (row = 0u; row < h; row++) {
        for (col = 0u; col < w; col++) {
            u32 px = src[row * stride_px + col] & 0x00FFFFFFu;
            if (!fb_inside(x + col, y + row)) {
                continue;
            }
            fb_store(x + col, y + row, fb_pixel_color(px));
        }
    }
    fb_cursor_composite();
    fb_present_flush();
}

void fb_draw_text_clipped(u32 x, u32 y, const char *text, u32 fg, u32 bg,
                          u32 max_w)
{
    u32 fg_color, bg_color;
    u32 cells, len, draw, i;

    if (!g_ready || !text || !max_w || y >= g_fb.height) {
        return;
    }
    cells = max_w / FB_GLYPH_W;
    if (!cells) {
        return;
    }
    len = (u32)strlen(text);
    draw = (len > cells) ? (cells > 3u ? cells - 3u : cells) : len;
    fg_color = fb_pixel_color(fg);
    bg_color = fb_pixel_color(bg);
    fb_cursor_restore();
    for (i = 0u; i < draw && i < len; i++) {
        if (x + FB_GLYPH_W > g_fb.width) {
            break;
        }
        fb_glyph_px(x, y, (unsigned char)text[i], fg_color, bg_color);
        x += FB_GLYPH_W;
    }
    if (len > cells && cells > 3u) {
        for (i = 0u; i < 3u; i++) {
            if (x + FB_GLYPH_W > g_fb.width) {
                break;
            }
            fb_glyph_px(x, y, (unsigned char)'.', fg_color, bg_color);
            x += FB_GLYPH_W;
        }
    }
    fb_cursor_composite();
    fb_present_flush();
}

/* Draw only the set glyph pixels, leaving the background alone: the glass
   title bar and the desktop wallpaper both need text over a gradient. */
void fb_draw_text_transparent(u32 x, u32 y, const char *text, u32 fg)
{
    u32 fg_color;

    if (!g_ready || !text || y >= g_fb.height) {
        return;
    }
    fg_color = fb_pixel_color(fg);
    fb_cursor_restore();
    while (*text && x + FB_GLYPH_W <= g_fb.width) {
        unsigned char ch = (unsigned char)*text++;
        u32 row, col;

        if (ch >= 128u) {
            ch = '?';
        }
        for (row = 0u; row < FB_GLYPH_H; row++) {
            u8 bits = crug_font8x16[ch][row];

            for (col = 0u; col < FB_GLYPH_W; col++) {
                if (!(bits & (0x80u >> col))) {
                    continue;
                }
                if (!fb_inside(x + col, y + row)) {
                    continue;
                }
                fb_store(x + col, y + row, fg_color);
            }
        }
        x += FB_GLYPH_W;
    }
    fb_cursor_composite();
    fb_present_flush();
}

u32 fb_text_width(const char *text)
{
    return text ? (u32)strlen(text) * FB_GLYPH_W : 0u;
}

int fb_sample(u32 x, u32 y, u32 *rgb)
{
    if (!g_ready || !rgb || x >= g_fb.width || y >= g_fb.height) {
        return -1;
    }
    *rgb = color_unpack(fb_read(x, y), g_fb.format);
    return 0;
}

/* Adopt a caller-owned linear buffer as the display and its present callback. */
int fb_attach_linear(u8 *base, u32 width, u32 height, u32 stride, u32 format,
                     fb_present_fn present)
{
    if (!base || !width || !height || stride < width) {
        return -1;
    }
    if (width / FB_GLYPH_W == 0u || height / FB_GLYPH_H == 0u) {
        return -1;
    }
    g_fb.base = (u64)(uintptr)base;
    g_fb.size = (u64)stride * (u64)height * 4u;
    g_fb.width = width;
    g_fb.height = height;
    g_fb.stride = stride;
    g_fb.format = format;
    g_cols = width / FB_GLYPH_W;
    g_rows = height / FB_GLYPH_H;
    g_x = 0;
    g_y = 0;
    g_text_enabled = 1;
    g_cursor_wanted = 0;
    g_cursor_drawn = 0;
    g_cursor_hw_shown = 0;
    g_present = present;
    g_present_depth = 0;
    gpu_damage_reset(&g_dmg);
    g_ready = 1;
    fb_clear();
    fb_present_flush();
    return 0;
}
/* Phase 8B: the hardware cursor plane.  The virtio-gpu driver installs these
   callbacks so the device moves the pointer itself; the software sprite path
   stays intact for the GOP framebuffer and for the fallback when the device
   refuses a cursor command. */
int fb_set_cursor_ops(const struct fb_cursor_ops *ops)
{
    fb_cursor_restore();
    if (g_cursor_hw_shown && g_cursor_ops) {
        g_cursor_ops->hide();
        g_cursor_hw_shown = 0;
    }
    g_cursor_ops = ops;
    return 0;
}

int fb_cursor_hardware(void)
{
    return g_cursor_ops != 0;
}

u32 fb_cursor_width(void)
{
    return FB_CURSOR_WIDTH;
}

u32 fb_cursor_height(void)
{
    return FB_CURSOR_HEIGHT;
}

u32 fb_cursor_sprite(u32 x, u32 y)
{
    if (x >= FB_CURSOR_WIDTH || y >= FB_CURSOR_HEIGHT) {
        return 0u;
    }
    return (u32)g_cursor_shape[y][x];
}

u32 fb_cursor_border_rgb(void)
{
    return FB_CURSOR_BORDER_RGB;
}

u32 fb_cursor_body_rgb(void)
{
    return FB_CURSOR_BODY_RGB;
}
