#include <crug/bitmap.h>

void bitmap_set(u8 *bitmap, u64 index)
{
    bitmap[index >> 3] |= (u8)(1u << (index & 7u));
}

void bitmap_clear(u8 *bitmap, u64 index)
{
    bitmap[index >> 3] &= (u8)~(1u << (index & 7u));
}

int bitmap_test(const u8 *bitmap, u64 index)
{
    return (bitmap[index >> 3] >> (index & 7u)) & 1u;
}

u64 bitmap_find_first_clear(const u8 *bitmap, u64 nbits)
{
    u64 i;

    for (i = 0; i < nbits; i++) {
        if (!bitmap_test(bitmap, i))
            return i;
    }
    return nbits;
}
