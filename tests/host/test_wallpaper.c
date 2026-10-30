/* The procedural sunset wallpaper: name parsing and the artwork itself.
   wallpaper.c is linked into the host suite, so the two kernel-side calls it
   makes are stubbed here (the real fb_blit and pmm_alloc_pages live in the
   guest). */
#include <crug/types.h>
#include <crug/fb.h>
#include <crug/mm.h>
#include <crug/wallpaper.h>
#include "test.h"

static u32 g_blits;

void fb_blit(u32 x, u32 y, u32 w, u32 h, const u32 *src, u32 stride_px)
{
    (void)x; (void)y; (void)w; (void)h; (void)src; (void)stride_px;
    g_blits++;
}

static u32 brightness(u32 rgb)
{
    return ((rgb >> 16) & 0xFFu) + ((rgb >> 8) & 0xFFu) + (rgb & 0xFFu);
}

void test_wallpaper_functions(void)
{
    u32 w = 1024u;
    u32 h = 768u;
    u32 horizon = h * 58u / 100u;
    u32 sky;
    u32 sea;
    u32 again;
    int found_sun = 0;
    u32 x;
    u32 y;

    CHECK(wallpaper_parse("off", 3) == WALLPAPER_OFF);
    CHECK(wallpaper_parse("sunset", 6) == WALLPAPER_SUNSET);
    CHECK(wallpaper_parse("suns", 4) == -1);
    CHECK(wallpaper_parse("sunsetx", 7) == -1);
    CHECK(wallpaper_parse("", 0) == -1);
    CHECK(wallpaper_parse(0, 3) == -1);
    CHECK_STR(wallpaper_name(WALLPAPER_OFF), "off");
    CHECK_STR(wallpaper_name(WALLPAPER_SUNSET), "sunset");
    CHECK_STR(wallpaper_name(42), "off");

    /* The sky is brighter than the water, and the same input always gives
       the same pixel (no hidden state in the painting). */
    sky = wallpaper_sunset_rgb(w / 2u, h / 10u, w, h);
    sea = wallpaper_sunset_rgb(w / 2u, h - 10u, w, h);
    again = wallpaper_sunset_rgb(w / 2u, h / 10u, w, h);
    CHECK(sky == again);
    CHECK(brightness(sky) > brightness(sea));
    CHECK(sky != sea);

    /* Above the horizon it is sky, below it is water: the two ramps must
       meet at the horizon, not at y=0. */
    CHECK(wallpaper_sunset_rgb(w / 3u, horizon - 1u, w, h) !=
          wallpaper_sunset_rgb(w / 3u, horizon + 1u, w, h));

    /* The sun disk is the brightest spot of the sky. */
    for (y = 0; y < horizon; y++) {
        for (x = 0; x < w; x++) {
            if (wallpaper_sunset_rgb(x, y, w, h) == 0x00FFF0C0u) {
                found_sun++;
            }
        }
    }
    CHECK(found_sun > 0);

    /* Rendering goes through the physical allocator (the fake in test_heap.c)
       and blits one row at a time; without a rendered wallpaper the desktop
       keeps its flat colour. */
    g_blits = 0;
    CHECK(wallpaper_draw(w, h) == 0);
    CHECK(wallpaper_render(WALLPAPER_SUNSET, w, h) == 0);
    CHECK(g_blits == h);
    CHECK(wallpaper_draw(w, h) == 1);
    CHECK(wallpaper_render(WALLPAPER_OFF, w, h) == 0);
    CHECK(wallpaper_draw(w, h) == 0);
    CHECK(wallpaper_render(WALLPAPER_SUNSET, 0, h) == -1);
}