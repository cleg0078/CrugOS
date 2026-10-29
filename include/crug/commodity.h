/* Commodities Exchange (phase 6J): global hotkey utilities.
   A commodity is a named action bound to one function key.  The registry is
   pure data so the host tests can drive it without a framebuffer; the
   Workbench owns the manager window that lists it. */
#ifndef CRUG_COMMODITY_H
#define CRUG_COMMODITY_H

#include <crug/types.h>

#define COMMODITY_MAX 4u
#define COMMODITY_NAME_MAX 16u

/* Manager window body geometry (body-relative pixels). */
#define COMMODITY_HEADER_H 24u
#define COMMODITY_ROW_TOP 28u
#define COMMODITY_ROW_H 26u
#define COMMODITY_ROW_X 6u

struct commodity {
    char name[COMMODITY_NAME_MAX];
    u8 hotkey;  /* INPUT_KEY_F1..INPUT_KEY_F12 */
    u8 enabled; /* 1 while the hotkey is live */
    u32 fires;  /* hotkey presses that ran the action */
    u32 blocked;/* hotkey presses refused because it was off */
};

void commodity_reset(void);
/* Register a commodity; returns its index or -1 when the table is full, the
   name or hotkey is already taken, or either argument is invalid. */
int commodity_register(const char *name, u8 hotkey);
u32 commodity_count(void);
const struct commodity *commodity_at(u32 index);
int commodity_index_of_hotkey(u8 key);
/* Flip a row; returns the new enabled state or -1 for an unknown index. */
int commodity_toggle(u32 index);
int commodity_set_enabled(u32 index, int on);
/* Dispatch one decoded key: returns the commodity index that owns it (or -1
   when the key is not a hotkey) and reports through fired whether the action
   ran (1) or was blocked because the commodity is off (0). */
int commodity_hotkey(u8 key, int *fired);
u64 commodity_fires(void);
u64 commodity_blocked(void);
u32 commodity_enabled_count(void);
/* "F1".."F12" for a function key, NULL for anything else. */
const char *commodity_key_name(u8 key);
/* Row index for a manager body-relative y, or -1 outside the rows. */
int commodity_row_at(i32 y_rel);

#endif /* CRUG_COMMODITY_H */