/* CrugOS copy-on-write frame reference counts (phase U6).
 *
 * Open addressing with tombstones: a probe chain may not stop at a released
 * slot, so release turns the entry into COW_TOMB instead of clearing it and
 * the next insert reuses the first tombstone it saw.
 */
#include <crug/cow.h>
#include <crug/string.h>

#define COW_EMPTY 0ull
#define COW_TOMB 1ull

typedef struct cow_slot {
    u64 pa;
    u32 refs;
    u32 pad;
} cow_slot;

static cow_slot g_cow_slots[COW_TABLE_SLOTS];

/* Frame numbers differ only in their high bits, so mix them down before
   masking: a plain low-bit mask would put every 16 MiB onto one slot. */
static u32 cow_hash(u64 pa)
{
    u64 x = (pa >> 12) * 2654435761ull;

    x ^= x >> 16;
    return (u32)x & (COW_TABLE_SLOTS - 1u);
}

static cow_slot *cow_lookup(u64 pa)
{
    u32 i = cow_hash(pa);
    u32 n;

    for (n = 0; n < COW_TABLE_SLOTS; n++) {
        cow_slot *slot = &g_cow_slots[i];

        if (slot->pa == pa && slot->refs != 0)
            return slot;
        if (slot->pa == COW_EMPTY)
            return 0;
        i = (i + 1u) & (COW_TABLE_SLOTS - 1u);
    }
    return 0;
}

static cow_slot *cow_reserve(u64 pa)
{
    u32 i = cow_hash(pa);
    cow_slot *tomb = 0;
    u32 n;

    for (n = 0; n < COW_TABLE_SLOTS; n++) {
        cow_slot *slot = &g_cow_slots[i];

        if (slot->pa == pa)
            return slot;
        if (slot->pa == COW_EMPTY)
            return tomb ? tomb : slot;
        if (slot->pa == COW_TOMB && !tomb)
            tomb = slot;
        i = (i + 1u) & (COW_TABLE_SLOTS - 1u);
    }
    return tomb;
}

void cow_reset(void)
{
    memset(g_cow_slots, 0, sizeof(g_cow_slots));
}

int cow_share(u64 pa)
{
    cow_slot *slot;

    pa &= ~0xfffull;
    if (pa == 0)
        return -1;
    slot = cow_lookup(pa);
    if (slot) {
        if (slot->refs != 0xffffffffu)
            slot->refs++;
        return 0;
    }
    slot = cow_reserve(pa);
    if (!slot)
        return -1;
    slot->pa = pa;
    slot->refs = 2u;
    return 0;
}

u32 cow_refs(u64 pa)
{
    cow_slot *slot;

    pa &= ~0xfffull;
    slot = cow_lookup(pa);
    return slot ? slot->refs : 0;
}

int cow_release(u64 pa)
{
    cow_slot *slot;

    pa &= ~0xfffull;
    slot = cow_lookup(pa);
    if (!slot)
        return 1;
    if (slot->refs > 1u) {
        slot->refs--;
        return 0;
    }
    slot->pa = COW_TOMB;
    slot->refs = 0;
    return 1;
}

u32 cow_tracked(void)
{
    u32 i;
    u32 n = 0;

    for (i = 0; i < COW_TABLE_SLOTS; i++) {
        if (g_cow_slots[i].pa > COW_TOMB && g_cow_slots[i].refs != 0)
            n++;
    }
    return n;
}
