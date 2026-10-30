/* The theme table: ids, lookup, parsing and the classic palette contract. */
#include <crug/theme.h>
#include "test.h"

void test_theme_functions(void)
{
    const struct theme *classic;
    const struct theme *glass;

    classic = theme_by_id(THEME_ID_CLASSIC);
    glass = theme_by_id(THEME_ID_GLASS);
    CHECK(classic != 0);
    CHECK(glass != 0);
    CHECK(theme_by_id(-1) == 0);
    CHECK(theme_by_id(THEME_COUNT) == 0);

    /* The classic theme must reproduce the phase 6 constants exactly. */
    CHECK(classic->id == THEME_ID_CLASSIC);
    CHECK_STR(classic->name, "classic");
    CHECK(classic->palette.desktop == 0x00102030u);
    CHECK(classic->palette.window == 0x00081018u);
    CHECK(classic->palette.frame == 0x00FFFFFFu);
    CHECK(classic->palette.title_active_top == 0x00FFB000u);
    CHECK(classic->palette.title_active_bottom == 0x00FFB000u);
    CHECK(classic->palette.title_idle_top == 0x00808080u);
    CHECK(classic->palette.body_text == 0x00E0E0E0u);
    CHECK(classic->palette.gadget == 0x00A0A0A0u);
    CHECK(classic->metrics.title_height == 20u);
    CHECK(classic->metrics.border == 2u);
    CHECK(classic->metrics.menubar_height == 18u);
    CHECK(classic->metrics.radius == 0u);
    CHECK(classic->metrics.shadow_depth == 0u);

    /* The glass theme is a different look, not a recolour of the same one. */
    CHECK(glass->id == THEME_ID_GLASS);
    CHECK_STR(glass->name, "glass");
    CHECK(glass->palette.window != classic->palette.window);
    CHECK(glass->palette.window == 0x00F4F4F5u);
    CHECK(glass->palette.body_text == 0x0023232Au);
    CHECK(glass->palette.accent == 0x00ECBF46u);
    CHECK(glass->palette.title_active_top != glass->palette.title_active_bottom);
    CHECK(glass->metrics.title_height == 24u);
    CHECK(glass->metrics.border == 1u);
    CHECK(glass->metrics.radius == 3u);
    CHECK(glass->metrics.menubar_height == 24u);
    CHECK(glass->metrics.dock_height == 56u);
    CHECK(glass->metrics.shadow_depth == 2u);

    /* Every palette entry is a plain RGB value. */
    CHECK((classic->palette.desktop & 0xFF000000u) == 0u);
    CHECK((glass->palette.window & 0xFF000000u) == 0u);

    /* Selection and exact parsing. */
    CHECK(theme_set(THEME_ID_GLASS) == 0);
    CHECK(theme_id() == THEME_ID_GLASS);
    CHECK(theme_get() == glass);
    CHECK_STR(theme_name(), "glass");
    CHECK(theme_set(THEME_COUNT) == -1);
    CHECK(theme_set(-1) == -1);
    CHECK(theme_id() == THEME_ID_GLASS);

    CHECK(theme_parse("glass", 5u) == 0);
    CHECK(theme_id() == THEME_ID_GLASS);
    CHECK(theme_parse("classic", 7u) == 0);
    CHECK(theme_id() == THEME_ID_CLASSIC);
    CHECK(theme_parse("GLASS", 5u) == -1);
    CHECK(theme_parse("glas", 4u) == -1);
    CHECK(theme_parse("classicx", 8u) == -1);
    CHECK(theme_parse(0, 0u) == -1);
    CHECK(theme_id() == THEME_ID_CLASSIC);
}