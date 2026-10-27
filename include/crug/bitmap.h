#ifndef CRUG_BITMAP_H
#define CRUG_BITMAP_H

#include <crug/types.h>

/* Bitmap helpers. Bit set means used/reserved, bit clear means free. */
void bitmap_set(u8 *bitmap, u64 index);
void bitmap_clear(u8 *bitmap, u64 index);
int bitmap_test(const u8 *bitmap, u64 index);
u64 bitmap_find_first_clear(const u8 *bitmap, u64 nbits);

#endif
