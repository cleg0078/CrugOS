/* The two built-in desktop themes. */
#include <crug/types.h>
#include <crug/string.h>
#include <crug/theme.h>

/* Classic reproduces the phase 6 look: navy desktop, white frame, solid amber
   active title, 20 px title, 18 px menu bar.  Every field is chosen so that
   painting through the theme yields the same pixels as the old constants. */
static const struct theme g_classic = {
    THEME_ID_CLASSIC,
    "classic",
    {
        0x00102030u, /* desktop */
        0x00081018u, /* window */
        0x00FFFFFFu, /* frame */
        0x00081018u, /* frame_highlight (unused: the classic frame is 2 px) */
        0x00FFB000u, /* title_active_top */
        0x00FFB000u, /* title_active_bottom */
        0x00000000u, /* title_active_text */
        0x00808080u, /* title_idle_top */
        0x00808080u, /* title_idle_bottom */
        0x00000000u, /* title_idle_text */
        0x00E0E0E0u, /* body_text */
        0x00FFB000u, /* accent */
        0x00C0C0C0u, /* menubar */
        0x00C0C0C0u, /* menu_bg */
        0x00000000u, /* menu_text */
        0x00FFB000u, /* menu_highlight */
        0x00A0A0A0u, /* gadget */
        0x00606060u, /* gadget_pressed */
        0x00000000u, /* gadget_text */
        0x00C0C0C0u, /* icon */
        0x00081018u, /* icon_inner */
        0x00E0E0E0u, /* icon_text */
        0x00FFB000u, /* icon_selected */
        0x00102030u, /* sidebar (classic has none) */
        0x00C0C0C0u, /* toolbar */
        0x00FFB000u, /* list_selection */
        0x00000000u, /* terminal_bg */
        0x00E0E0E0u, /* terminal_fg */
        0x00FFB000u, /* terminal_prompt */
        0x00000000u, /* dock_fill (classic has no dock) */
        0x00808080u, /* dock_border */
        0x00C0C0C0u, /* dock_hover */
        0x00FFFFFFu, /* cursor_border */
        0x00FFD040u, /* cursor_body */
    },
    {
        20u, /* title_height */
        2u,  /* border */
        0u,  /* radius */
        12u, /* gadget_size */
        18u, /* menubar_height */
        18u, /* menu_item_height */
        64u, /* icon_width */
        48u, /* icon_height */
        4u,  /* icon_label_gap */
        12u, /* depth_size */
        12u, /* front_size */
        0u,  /* shadow_depth */
        0u,  /* shadow_strength */
        0u,  /* shadow_floor */
        6u,  /* screen_pad */
        8u,  /* screen_glyph_w */
        0u,  /* dock_height */
    },
};

/* Glass is the approved mockup: a light window body with a one-pixel border,
   a soft title gradient, an amber accent and a floating dock. */
static const struct theme g_glass = {
    THEME_ID_GLASS,
    "glass",
    {
        0x001B2A3Au, /* desktop (fallback where the wallpaper is off) */
        0x00F4F4F5u, /* window */
        0x008A8A8Fu, /* frame */
        0x00FFFFFFu, /* frame_highlight */
        0x00FFFFFFu, /* title_active_top */
        0x00E4E6E8u, /* title_active_bottom */
        0x001E1E22u, /* title_active_text */
        0x00F7F7F7u, /* title_idle_top */
        0x00E9E9E9u, /* title_idle_bottom */
        0x008E8E93u, /* title_idle_text */
        0x0023232Au, /* body_text */
        0x00ECBF46u, /* accent */
        0x00D9D9D9u, /* menubar (the glass screen bar is drawn translucent) */
        0x00FFFFFFu, /* menu_bg */
        0x0023232Au, /* menu_text */
        0x00ECBF46u, /* menu_highlight */
        0x00EFEFF1u, /* gadget */
        0x00D3D3D8u, /* gadget_pressed */
        0x004A4A50u, /* gadget_text */
        0x00EAEAECu, /* icon */
        0x00FFFFFFu, /* icon_inner */
        0x002A2A30u, /* icon_text */
        0x00ECBF46u, /* icon_selected */
        0x00EAEAECu, /* sidebar */
        0x00F0F0F1u, /* toolbar */
        0x009EB8E0u, /* list_selection */
        0x00000000u, /* terminal_bg */
        0x00E8E8E8u, /* terminal_fg */
        0x00ECBF46u, /* terminal_prompt */
        0x00000000u, /* dock_fill (28% black over the wallpaper) */
        0x00FFFFFFu, /* dock_border */
        0x00FFFFFFu, /* dock_hover */
        0x00000000u, /* cursor_border */
        0x00FFFFFFu, /* cursor_body */
    },
    {
        24u, /* title_height */
        1u,  /* border */
        3u,  /* radius */
        14u, /* gadget_size */
        24u, /* menubar_height */
        20u, /* menu_item_height */
        48u, /* icon_width */
        48u, /* icon_height */
        4u,  /* icon_label_gap */
        14u, /* depth_size */
        14u, /* front_size */
        2u,  /* shadow_depth */
        96u, /* shadow_strength */
        24u, /* shadow_floor */
        8u,  /* screen_pad */
        8u,  /* screen_glyph_w */
        56u, /* dock_height */
    },
};

static const struct theme *const g_themes[THEME_COUNT] = { &g_classic, &g_glass };

/* Phase UI default: the glass theme.  wb.theme=classic keeps the phase 6
   look available, and the palette gates ask for it explicitly. */
static int g_theme_id = THEME_ID_GLASS;

const struct theme *theme_get(void)
{
    return g_themes[g_theme_id];
}

const struct theme *theme_by_id(int id)
{
    if (id < 0 || id >= THEME_COUNT) {
        return (const struct theme *)0;
    }
    return g_themes[id];
}

int theme_set(int id)
{
    if (!theme_by_id(id)) {
        return -1;
    }
    g_theme_id = id;
    return 0;
}

int theme_id(void)
{
    return g_theme_id;
}

const char *theme_name(void)
{
    return theme_get()->name;
}

int theme_parse(const char *text, u32 len)
{
    if (!text) {
        return -1;
    }
    if (len == 7u && text[0] == 'c' && text[1] == 'l' && text[2] == 'a' &&
        text[3] == 's' && text[4] == 's' && text[5] == 'i' && text[6] == 'c') {
        return theme_set(THEME_ID_CLASSIC);
    }
    if (len == 5u && text[0] == 'g' && text[1] == 'l' && text[2] == 'a' &&
        text[3] == 's' && text[4] == 's') {
        return theme_set(THEME_ID_GLASS);
    }
    return -1;
}