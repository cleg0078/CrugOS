/* The procedural icon set (phase UI). */
#include <crug/types.h>
#include <crug/kernel.h>
#include <crug/fb.h>
#include <crug/icons.h>

static void px(u32 x, u32 y, u32 w, u32 h, u32 rgb)
{
    if (w != 0u && h != 0u) {
        fb_fill_rect(x, y, w, h, rgb);
    }
}

/* Scale one 48 px coordinate to the requested tile size. */
static u32 sc(u32 value, u32 size)
{
    return value * size / ICONS_SIZE;
}

static void icon_volume(u32 x, u32 y, u32 s)
{
    u32 body = sc(6u, s);
    u32 width = sc(36u, s);

    /* Feet, then the box, then the lid with its slot and its LED. */
    px(x + sc(10u, s), y + sc(38u, s), sc(8u, s), sc(4u, s), 0x00303038u);
    px(x + sc(30u, s), y + sc(38u, s), sc(8u, s), sc(4u, s), 0x00303038u);
    px(x + body, y + sc(14u, s), width, sc(28u, s), 0x00D8D8DEu);
    px(x + body, y + sc(14u, s), width, sc(2u, s), 0x00FFFFFFu);
    px(x + body, y + sc(38u, s), width, sc(2u, s), 0x0090909Au);
    px(x + sc(10u, s), y + sc(8u, s), sc(28u, s), sc(8u, s), 0x00B8B8C2u);
    px(x + sc(13u, s), y + sc(20u, s), sc(22u, s), sc(6u, s), 0x00FFFFFFu);
    px(x + sc(13u, s), y + sc(30u, s), sc(14u, s), sc(3u, s), 0x0080808Au);
    px(x + sc(34u, s), y + sc(30u, s), sc(4u, s), sc(4u, s), 0x004CC86Cu);
}

static void icon_drawer(u32 x, u32 y, u32 s)
{
    /* Tab, folder body, a lighter top edge and a darker bottom edge. */
    px(x + sc(8u, s), y + sc(10u, s), sc(16u, s), sc(8u, s), 0x00C89828u);
    px(x + sc(6u, s), y + sc(16u, s), sc(36u, s), sc(24u, s), 0x00ECBF46u);
    px(x + sc(6u, s), y + sc(16u, s), sc(36u, s), sc(2u, s), 0x00FFE49Au);
    px(x + sc(6u, s), y + sc(37u, s), sc(36u, s), sc(3u, s), 0x00B08818u);
    px(x + sc(10u, s), y + sc(22u, s), sc(28u, s), sc(11u, s), 0x00F8D878u);
}

static void icon_file(u32 x, u32 y, u32 s)
{
    /* A white sheet with a folded corner and three grey text lines. */
    px(x + sc(12u, s), y + sc(6u, s), sc(22u, s), sc(36u, s), 0x00FFFFFFu);
    px(x + sc(12u, s), y + sc(6u, s), sc(22u, s), sc(1u, s), 0x00E0E0E4u);
    px(x + sc(28u, s), y + sc(6u, s), sc(6u, s), sc(8u, s), 0x00C8C8D0u);
    px(x + sc(16u, s), y + sc(20u, s), sc(14u, s), sc(2u, s), 0x00787880u);
    px(x + sc(16u, s), y + sc(25u, s), sc(14u, s), sc(2u, s), 0x00787880u);
    px(x + sc(16u, s), y + sc(30u, s), sc(9u, s), sc(2u, s), 0x00787880u);
}

void icons_draw(u32 kind, u32 x, u32 y, u32 size)
{
    if (size < 8u) {
        size = 8u;
    }
    switch (kind) {
    case ICONS_DRAWER:
        icon_drawer(x, y, size);
        break;
    case ICONS_FILE:
        icon_file(x, y, size);
        break;
    default:
        icon_volume(x, y, size);
        break;
    }
}
