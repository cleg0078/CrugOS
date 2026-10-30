/* The procedural sunset wallpaper (phase UI). */
#include <crug/types.h>
#include <crug/string.h>
#include <crug/kernel.h>
#include <crug/fb.h>
#include <crug/mm.h>
#include <crug/color.h>
#include <crug/paint.h>
#include <crug/wallpaper.h>

static u32 *g_pixels;
static u32 g_width;
static u32 g_height;
static int g_kind = WALLPAPER_OFF;

static int wallpaper_match(const char *text, u32 len, const char *name)
{
    u32 i;

    for (i = 0; i < len; i++) {
        if (text[i] != name[i]) {
            return 0;
        }
    }
    return name[len] == '\0';
}

int wallpaper_parse(const char *text, u32 len)
{
    if (!text || len == 0u) {
        return -1;
    }
    if (wallpaper_match(text, len, "off")) {
        return WALLPAPER_OFF;
    }
    if (wallpaper_match(text, len, "sunset")) {
        return WALLPAPER_SUNSET;
    }
    return -1;
}

const char *wallpaper_name(int kind)
{
    return kind == WALLPAPER_SUNSET ? "sunset" : "off";
}

/* A cheap hash-free ordering helper: blend two colours along a ramp. */
static u32 ramp(u32 from, u32 to, u32 index, u32 count)
{
    return color_lerp(from, to, index, count);
}

static u32 sky_rgb(u32 y, u32 horizon, u32 h)
{
    u32 span = horizon;

    (void)h;
    if (span == 0u) {
        return 0x00F0A060u;
    }
    if (y * 100u < span * 45u) {
        return ramp(0x0010182Eu, 0x00785078u, y, span * 45u / 100u);
    }
    if (y * 100u < span * 78u) {
        return ramp(0x00785078u, 0x00E89060u, y - span * 45u / 100u,
                    span * 33u / 100u);
    }
    return ramp(0x00E89060u, 0x00FFC878u, y - span * 78u / 100u,
                span * 22u / 100u);
}

static u32 sea_rgb(u32 y, u32 horizon, u32 h)
{
    u32 span = h > horizon ? h - horizon : 1u;

    return ramp(0x00463C60u, 0x000C1020u, y - horizon, span);
}

u32 wallpaper_sunset_rgb(u32 x, u32 y, u32 w, u32 h)
{
    i32 horizon = (i32)(h * 58u / 100u);
    i32 sun_x = (i32)(w * 72u / 100u);
    i32 sun_y = horizon - (i32)(h * 7u / 100u);
    i32 sun_r = (i32)(h * 9u / 100u);
    i32 planet_x = (i32)(w * 15u / 100u);
    i32 planet_y = (i32)(h * 21u / 100u);
    i32 planet_r = (i32)(h * 10u / 100u);
    u32 rgb;
    i32 dx;
    i32 dy;
    i64 d2;

    if (sun_r < 4) {
        sun_r = 4;
    }
    if (planet_r < 4) {
        planet_r = 4;
    }
    if ((i32)y < horizon) {
        rgb = sky_rgb(y, (u32)horizon, h);
    } else {
        rgb = sea_rgb(y, (u32)horizon, h);
    }
    /* The sun: a warm disk with a banded rim and a soft halo. */
    dx = (i32)x - sun_x;
    dy = (i32)y - sun_y;
    d2 = (i64)dx * dx + (i64)dy * dy;
    if (d2 <= (i64)sun_r * sun_r) {
        rgb = d2 > (i64)(sun_r - 4) * (sun_r - 4) ? 0x00FFD890u : 0x00FFF0C0u;
    } else if (d2 <= (i64)(sun_r * 2) * (sun_r * 2)) {
        u32 alpha = 70u - (u32)(d2 * 70 / ((i64)(sun_r * 2) * (sun_r * 2)));

        rgb = color_blend(rgb, 0x00FFC070u, alpha);
    }
    /* The ringed planet in the upper left. */
    dx = (i32)x - planet_x;
    dy = (i32)y - planet_y;
    d2 = (i64)dx * dx + (i64)dy * dy;
    if (d2 <= (i64)planet_r * planet_r) {
        u32 shade = (u32)((i64)planet_r - dy) * 40u / (u32)(planet_r * 2);

        rgb = color_blend(0x008890B8u, 0x00E8E8F0u, shade);
    } else {
        i64 rx = planet_r * 2;
        i64 ry = planet_r / 3;
        i64 e = ((i64)dx * dx * 100) / (rx * rx) +
                ((i64)dy * dy * 100) / (ry * ry + 1);

        if (e >= 62 && e <= 100) {
            rgb = color_blend(rgb, 0x00D8D0E0u, 120u);
        }
    }
    /* The sun's reflection on the water. */
    /* The reflection: a soft column under the sun that fades both downwards
       and towards its edges, with a ripple every few rows. */
    if ((i32)y > horizon && y < (u32)(horizon + (i32)(h * 18u / 100u))) {
        i32 sdx = (i32)x - sun_x;
        u32 half = (u32)sun_r;
        u32 depth = y - (u32)horizon;
        u32 side;

        if (sdx < 0) {
            side = (u32)(-sdx);
        } else {
            side = (u32)sdx;
        }
        if (side < half) {
            u32 alpha = (40u - depth / 12u) * (half - side) / half;

            if ((y / 3u) % 2u == 0u) {
                alpha = alpha * 3u / 4u;
            }
            rgb = color_blend(rgb, 0x00FFC070u, alpha);
        }
    }
    return rgb;
}

int wallpaper_render(int kind, u32 w, u32 h)
{
    u64 pages;
    u64 pa;
    u32 x;
    u32 y;

    g_kind = kind;
    if (kind != WALLPAPER_SUNSET || w == 0u || h == 0u) {
        return kind == WALLPAPER_OFF ? 0 : -1;
    }
    if (g_pixels && g_width == w && g_height == h) {
        return 0;
    }
    pages = ((u64)w * h * 4u + 4095u) / 4096u;
    pa = pmm_alloc_pages(pages);
    if (pa == 0u) {
        kprintf("CRUGOS: wallpaper buffer unavailable\n");
        return -1;
    }
    g_pixels = (u32 *)(uintptr)pa;
    g_width = w;
    g_height = h;
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            g_pixels[y * w + x] = wallpaper_sunset_rgb(x, y, w, h);
        }
    }
    for (y = 0; y < h; y++) {
        fb_blit(0u, y, w, 1u, g_pixels + y * w, w);
    }
    kprintf("CRUGOS: wallpaper %s %ux%u\n", wallpaper_name(kind), (unsigned)w,
            (unsigned)h);
    return 0;
}

int wallpaper_draw(u32 w, u32 h)
{
    if (g_kind != WALLPAPER_SUNSET || !g_pixels) {
        return 0;
    }
    if (g_width != w || g_height != h) {
        return 0;
    }
    fb_blit(0u, 0u, w, h, g_pixels, w);
    return 1;
}
