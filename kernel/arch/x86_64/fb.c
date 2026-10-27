/* Linear framebuffer text console (8x16 glyphs). */
#include <crug/types.h>
#include <crug/string.h>
#include <crug/kernel.h>

#define FB_GLYPH_W 8
#define FB_GLYPH_H 16
#define FB_COLOR_BG 0x00000000u
#define FB_COLOR_FG 0x00E0E0E0u

extern const u8 crug_font8x16[128][16];

static framebuffer_info g_fb;
static int g_ready;
static u32 g_cols;
static u32 g_rows;
static u32 g_x;
static u32 g_y;

static u32 fb_pixel_color(u32 rgb)
{
    u32 r = (rgb >> 16) & 0xFF;
    u32 g = (rgb >> 8) & 0xFF;
    u32 b = rgb & 0xFF;

    if (g_fb.format == CRUG_PIXEL_BGRX8888) {
        return (b << 16) | (g << 8) | r;
    }
    return (r << 16) | (g << 8) | b;
}

static void fb_store(u32 x, u32 y, u32 color)
{
    volatile u32 *row = (volatile u32 *)(uintptr)(g_fb.base + (u64)y * g_fb.stride * 4);
    row[x] = color;
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
    for (y = 0; y < g_fb.height; y++) {
        fb_fill_row(y, fb_pixel_color(FB_COLOR_BG));
    }
    g_x = 0;
    g_y = 0;
}

void fb_init(const framebuffer_info *fb)
{
    g_ready = 0;
    if (!fb || fb->base == 0 || fb->width == 0 || fb->height == 0 ||
        fb->stride < fb->width) {
        return;
    }
    g_fb = *fb;
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

    for (y = 0; y + FB_GLYPH_H < g_fb.height; y++) {
        memcpy((void *)(uintptr)(g_fb.base + (u64)y * line_bytes),
               (void *)(uintptr)(g_fb.base + (u64)(y + FB_GLYPH_H) * line_bytes),
               line_bytes);
    }
    for (; y < g_fb.height; y++) {
        fb_fill_row(y, fb_pixel_color(FB_COLOR_BG));
    }
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

void fb_putc(char ch)
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
            fb_putc(' ');
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
