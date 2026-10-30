/* The desktop theme: every colour and metric the window manager and the
   Workbench chrome paint with.  Two themes ship: 'classic' reproduces the
   phase 6 look pixel for pixel, 'glass' is the light AmigaOS 4 style desktop. */
#ifndef CRUG_THEME_H
#define CRUG_THEME_H

#include <crug/types.h>

#define THEME_ID_CLASSIC 0
#define THEME_ID_GLASS 1
#define THEME_COUNT 2

struct theme_palette {
    u32 desktop;
    u32 window;
    u32 frame;
    u32 frame_highlight;
    u32 title_active_top;
    u32 title_active_bottom;
    u32 title_active_text;
    u32 title_idle_top;
    u32 title_idle_bottom;
    u32 title_idle_text;
    u32 body_text;
    u32 accent;
    u32 menubar;
    u32 menu_bg;
    u32 menu_text;
    u32 menu_highlight;
    u32 gadget;
    u32 gadget_pressed;
    u32 gadget_text;
    u32 icon;
    u32 icon_inner;
    u32 icon_text;
    u32 icon_selected;
    u32 sidebar;
    u32 toolbar;
    u32 list_selection;
    u32 terminal_bg;
    u32 terminal_fg;
    u32 terminal_prompt;
    u32 dock_fill;
    u32 dock_border;
    u32 dock_hover;
    u32 cursor_border;
    u32 cursor_body;
};

struct theme_metrics {
    u32 title_height;
    u32 border;
    u32 radius;
    u32 gadget_size;
    u32 menubar_height;
    u32 menu_item_height;
    u32 icon_width;
    u32 icon_height;
    u32 icon_label_gap;
    u32 depth_size;
    u32 front_size;
    u32 shadow_depth;
    u32 shadow_strength;
    u32 shadow_floor;
    u32 screen_pad;
    u32 screen_glyph_w;
    u32 dock_height;
};

struct theme {
    int id;
    const char *name;
    struct theme_palette palette;
    struct theme_metrics metrics;
};

/* The theme everything paints with; never NULL. */
const struct theme *theme_get(void);
const struct theme *theme_by_id(int id);

/* Select a theme by id.  Returns 0 on success and -1 for an unknown id. */
int theme_set(int id);
int theme_id(void);
const char *theme_name(void);

/* Parse "classic" or "glass" (exact, case sensitive).  Returns 0 when a
   theme was selected and -1 for any other text. */
int theme_parse(const char *text, u32 len);

#endif /* CRUG_THEME_H */