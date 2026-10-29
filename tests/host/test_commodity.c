/* Host tests for the Commodities Exchange registry (kernel/gfx/commodities.c). */
#include "test.h"
#include <crug/commodity.h>
#include <crug/input.h>
#include <string.h>

static void register_defaults(void)
{
    CHECK(commodity_register("Volume up", (u8)INPUT_KEY_F2) == 0);
    CHECK(commodity_register("Volume down", (u8)INPUT_KEY_F3) == 1);
    CHECK(commodity_register("Mute", (u8)INPUT_KEY_F4) == 2);
    CHECK(commodity_register("Clock", (u8)INPUT_KEY_F6) == 3);
    CHECK(commodity_count() == COMMODITY_MAX);
}

void test_commodity_functions(void)
{
    const struct commodity *c;
    int fired;
    int i;

    /* ---- an empty registry ---- */
    commodity_reset();
    CHECK(commodity_count() == 0);
    CHECK(commodity_fires() == 0);
    CHECK(commodity_blocked() == 0);
    CHECK(commodity_enabled_count() == 0);
    CHECK(commodity_at(0) == NULL);
    CHECK(commodity_index_of_hotkey((u8)INPUT_KEY_F1) == -1);
    CHECK(commodity_hotkey((u8)INPUT_KEY_F1, NULL) == -1);
    CHECK(commodity_row_at(0) == -1);
    CHECK(commodity_row_at((i32)COMMODITY_ROW_TOP) == -1);
    CHECK(commodity_toggle(0) == -1);
    CHECK(commodity_set_enabled(0, 1) == -1);

    /* ---- registration rejects bad names and hotkeys ---- */
    CHECK(commodity_register(NULL, (u8)INPUT_KEY_F1) == -1);
    CHECK(commodity_register("", (u8)INPUT_KEY_F1) == -1);
    CHECK(commodity_register("0123456789abcdef", (u8)INPUT_KEY_F1) == -1);
    CHECK(commodity_register("Zero", 0u) == -1);
    CHECK(commodity_register("Up", (u8)INPUT_KEY_UP) == -1);
    CHECK(commodity_register("Thirteen", (u8)(INPUT_KEY_F12 + 1u)) == -1);
    CHECK(commodity_count() == 0);

    /* A 15-byte name still fits; it is replaced by the defaults below. */
    CHECK(commodity_register("0123456789abcde", (u8)INPUT_KEY_F1) == 0);
    CHECK(commodity_count() == 1);
    CHECK(commodity_at(0) != NULL);
    CHECK(commodity_at(0)->hotkey == (u8)INPUT_KEY_F1);
    CHECK(commodity_at(0)->enabled == 1);
    CHECK(commodity_at(0)->fires == 0 && commodity_at(0)->blocked == 0);
    CHECK(commodity_register("Volume up", (u8)INPUT_KEY_F2) == 1);
    CHECK(commodity_register("Volume up", (u8)INPUT_KEY_F7) == -1);  /* name taken */
    CHECK(commodity_register("Other", (u8)INPUT_KEY_F1) == -1);      /* hotkey taken */
    CHECK(commodity_count() == 2);

    /* ---- the four Workbench commodities ---- */
    commodity_reset();
    register_defaults();
    c = commodity_at(0);
    CHECK(c != NULL && strcmp(c->name, "Volume up") == 0);
    CHECK(c != NULL && c->hotkey == (u8)INPUT_KEY_F2);
    CHECK(c != NULL && c->enabled == 1u);
    CHECK(commodity_at(2) != NULL && strcmp(commodity_at(2)->name, "Mute") == 0);
    CHECK(commodity_at(2) != NULL && commodity_at(2)->hotkey == (u8)INPUT_KEY_F4);
    CHECK(commodity_at(3) != NULL && commodity_at(3)->hotkey == (u8)INPUT_KEY_F6);
    CHECK(commodity_at(COMMODITY_MAX) == NULL);
    CHECK(commodity_enabled_count() == COMMODITY_MAX);
    CHECK(commodity_index_of_hotkey((u8)INPUT_KEY_F4) == 2);
    CHECK(commodity_index_of_hotkey((u8)INPUT_KEY_F6) == 3);
    CHECK(commodity_index_of_hotkey((u8)INPUT_KEY_F1) == -1);
    CHECK(commodity_index_of_hotkey((u8)INPUT_KEY_F12) == -1);
    /* The table is full: nothing else registers even with a free hotkey. */
    CHECK(commodity_register("Extra", (u8)INPUT_KEY_F1) == -1);

    /* ---- toggle and set_enabled ---- */
    CHECK(commodity_toggle(2) == 0);
    CHECK(commodity_at(2)->enabled == 0u);
    CHECK(commodity_enabled_count() == COMMODITY_MAX - 1u);
    CHECK(commodity_toggle(2) == 1);
    CHECK(commodity_enabled_count() == COMMODITY_MAX);
    CHECK(commodity_set_enabled(0, 0) == 0);
    CHECK(commodity_set_enabled(0, 7) == 1);
    CHECK(commodity_set_enabled(0, 0) == 0);
    CHECK(commodity_enabled_count() == COMMODITY_MAX - 1u);
    CHECK(commodity_toggle(COMMODITY_MAX) == -1);
    CHECK(commodity_set_enabled(COMMODITY_MAX, 1) == -1);
    CHECK(commodity_set_enabled(0, 1) == 1);

    /* ---- dispatch ---- */
    fired = -1;
    CHECK(commodity_hotkey((u8)INPUT_KEY_F4, &fired) == 2);
    CHECK(fired == 1);
    CHECK(commodity_at(2)->fires == 1u && commodity_at(2)->blocked == 0u);
    CHECK(commodity_fires() == 1 && commodity_blocked() == 0);
    /* A key that belongs to nobody leaves the counters and fired alone. */
    fired = -1;
    CHECK(commodity_hotkey((u8)INPUT_KEY_F5, &fired) == -1);
    CHECK(fired == -1);
    CHECK(commodity_hotkey((u8)INPUT_KEY_UP, &fired) == -1);
    CHECK(commodity_fires() == 1);
    /* A disabled commodity blocks the press instead of running it. */
    CHECK(commodity_set_enabled(2, 0) == 0);
    fired = -1;
    CHECK(commodity_hotkey((u8)INPUT_KEY_F4, &fired) == 2);
    CHECK(fired == 0);
    CHECK(commodity_at(2)->fires == 1u && commodity_at(2)->blocked == 1u);
    CHECK(commodity_fires() == 1 && commodity_blocked() == 1);
    /* fired may be omitted. */
    CHECK(commodity_hotkey((u8)INPUT_KEY_F6, NULL) == 3);
    CHECK(commodity_at(3)->fires == 1u);
    CHECK(commodity_fires() == 2);
    for (i = 0; i < 3; i++)
        CHECK(commodity_hotkey((u8)INPUT_KEY_F3, &fired) == 1);
    CHECK(commodity_at(1)->fires == 3u);
    CHECK(commodity_fires() == 5);

    /* ---- key names ---- */
    CHECK(commodity_key_name((u8)INPUT_KEY_F1) != NULL &&
          strcmp(commodity_key_name((u8)INPUT_KEY_F1), "F1") == 0);
    CHECK(commodity_key_name((u8)INPUT_KEY_F4) != NULL &&
          strcmp(commodity_key_name((u8)INPUT_KEY_F4), "F4") == 0);
    CHECK(commodity_key_name((u8)INPUT_KEY_F10) != NULL &&
          strcmp(commodity_key_name((u8)INPUT_KEY_F10), "F10") == 0);
    CHECK(commodity_key_name((u8)INPUT_KEY_F11) != NULL &&
          strcmp(commodity_key_name((u8)INPUT_KEY_F11), "F11") == 0);
    CHECK(commodity_key_name((u8)INPUT_KEY_F12) != NULL &&
          strcmp(commodity_key_name((u8)INPUT_KEY_F12), "F12") == 0);
    CHECK(commodity_key_name((u8)INPUT_KEY_UP) == NULL);
    CHECK(commodity_key_name(0u) == NULL);
    CHECK(commodity_key_name((u8)(INPUT_KEY_F12 + 1u)) == NULL);

    /* ---- row geometry for the manager window ---- */
    commodity_reset();
    CHECK(commodity_row_at(0) == -1);
    CHECK(commodity_row_at((i32)COMMODITY_ROW_TOP - 1) == -1);
    register_defaults();
    CHECK(commodity_row_at(-4) == -1);
    CHECK(commodity_row_at(0) == -1);
    CHECK(commodity_row_at((i32)COMMODITY_ROW_TOP - 1) == -1);
    CHECK(commodity_row_at((i32)COMMODITY_ROW_TOP) == 0);
    CHECK(commodity_row_at((i32)(COMMODITY_ROW_TOP + COMMODITY_ROW_H - 1u)) == 0);
    CHECK(commodity_row_at((i32)(COMMODITY_ROW_TOP + COMMODITY_ROW_H)) == 1);
    CHECK(commodity_row_at((i32)(COMMODITY_ROW_TOP + 2u * COMMODITY_ROW_H)) == 2);
    CHECK(commodity_row_at((i32)(COMMODITY_ROW_TOP + 3u * COMMODITY_ROW_H)) == 3);
    CHECK(commodity_row_at((i32)(COMMODITY_ROW_TOP + COMMODITY_MAX * COMMODITY_ROW_H)) == -1);

    /* ---- reset clears counters and the enabled flags ---- */
    CHECK(commodity_set_enabled(1, 0) == 0);
    commodity_reset();
    CHECK(commodity_count() == 0);
    CHECK(commodity_fires() == 0);
    CHECK(commodity_blocked() == 0);
    CHECK(commodity_enabled_count() == 0);
    CHECK(commodity_register("Again", (u8)INPUT_KEY_F1) == 0);
    CHECK(commodity_count() == 1);
    CHECK(commodity_at(0) != NULL && commodity_at(0)->enabled == 1u);
    CHECK(commodity_at(0) != NULL && commodity_at(0)->fires == 0u);
    CHECK(commodity_at(0) != NULL && commodity_at(0)->blocked == 0u);
}
