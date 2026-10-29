/* Commodities Exchange registry (phase 6J). */
#include <crug/commodity.h>
#include <crug/input.h>
#include <crug/string.h>

static struct commodity g_commodities[COMMODITY_MAX];
static u32 g_count;

static const char *const g_key_names[12] = {
    "F1", "F2", "F3", "F4", "F5", "F6",
    "F7", "F8", "F9", "F10", "F11", "F12"
};

void commodity_reset(void)
{
    memset(g_commodities, 0, sizeof(g_commodities));
    g_count = 0u;
}

int commodity_register(const char *name, u8 hotkey)
{
    u32 i;
    usize len = 0;

    if (name == 0 || g_count >= COMMODITY_MAX) {
        return -1;
    }
    if (hotkey < (u8)INPUT_KEY_F1 || hotkey > (u8)INPUT_KEY_F12) {
        return -1;
    }
    while (name[len] != 0) {
        len++;
        if (len >= COMMODITY_NAME_MAX) {
            return -1;
        }
    }
    if (len == 0u) {
        return -1;
    }
    for (i = 0; i < g_count; i++) {
        if (strcmp(g_commodities[i].name, name) == 0) {
            return -1;
        }
        if (g_commodities[i].hotkey == hotkey) {
            return -1;
        }
    }
    memcpy(g_commodities[g_count].name, name, len + 1u);
    g_commodities[g_count].hotkey = hotkey;
    g_commodities[g_count].enabled = 1u;
    g_commodities[g_count].fires = 0u;
    g_commodities[g_count].blocked = 0u;
    g_count++;
    return (int)(g_count - 1u);
}

u32 commodity_count(void)
{
    return g_count;
}

const struct commodity *commodity_at(u32 index)
{
    if (index >= g_count) {
        return 0;
    }
    return &g_commodities[index];
}

int commodity_index_of_hotkey(u8 key)
{
    u32 i;

    for (i = 0; i < g_count; i++) {
        if (g_commodities[i].hotkey == key) {
            return (int)i;
        }
    }
    return -1;
}

int commodity_set_enabled(u32 index, int on)
{
    if (index >= g_count) {
        return -1;
    }
    g_commodities[index].enabled = on ? 1u : 0u;
    return (int)g_commodities[index].enabled;
}

int commodity_toggle(u32 index)
{
    if (index >= g_count) {
        return -1;
    }
    return commodity_set_enabled(index, g_commodities[index].enabled ? 0 : 1);
}

int commodity_hotkey(u8 key, int *fired)
{
    int index = commodity_index_of_hotkey(key);

    if (index < 0) {
        return -1;
    }
    if (g_commodities[index].enabled) {
        g_commodities[index].fires++;
        if (fired != 0) {
            *fired = 1;
        }
    } else {
        g_commodities[index].blocked++;
        if (fired != 0) {
            *fired = 0;
        }
    }
    return index;
}

u64 commodity_fires(void)
{
    u64 total = 0;
    u32 i;

    for (i = 0; i < g_count; i++) {
        total += g_commodities[i].fires;
    }
    return total;
}

u64 commodity_blocked(void)
{
    u64 total = 0;
    u32 i;

    for (i = 0; i < g_count; i++) {
        total += g_commodities[i].blocked;
    }
    return total;
}

u32 commodity_enabled_count(void)
{
    u32 total = 0;
    u32 i;

    for (i = 0; i < g_count; i++) {
        if (g_commodities[i].enabled) {
            total++;
        }
    }
    return total;
}

const char *commodity_key_name(u8 key)
{
    if (key < (u8)INPUT_KEY_F1 || key > (u8)INPUT_KEY_F12) {
        return 0;
    }
    return g_key_names[(u32)(key - (u8)INPUT_KEY_F1)];
}

int commodity_row_at(i32 y_rel)
{
    u32 row;

    if (y_rel < (i32)COMMODITY_ROW_TOP) {
        return -1;
    }
    row = (u32)(y_rel - (i32)COMMODITY_ROW_TOP) / COMMODITY_ROW_H;
    if (row >= g_count) {
        return -1;
    }
    return (int)row;
}