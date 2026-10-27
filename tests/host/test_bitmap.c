#include <crug/bitmap.h>
#include <string.h>
#include "test.h"

void test_bitmap_functions(void)
{
    u8 bitmap[16];
    u64 i;

    memset(bitmap, 0, sizeof(bitmap));

    CHECK(bitmap_find_first_clear(bitmap, 128) == 0);

    bitmap_set(bitmap, 0);
    CHECK(bitmap_test(bitmap, 0) == 1);
    CHECK(bitmap_test(bitmap, 1) == 0);
    CHECK(bitmap_find_first_clear(bitmap, 128) == 1);

    bitmap_set(bitmap, 7);
    bitmap_set(bitmap, 8);
    bitmap_set(bitmap, 63);
    bitmap_set(bitmap, 64);
    bitmap_set(bitmap, 127);
    CHECK(bitmap_test(bitmap, 7) == 1);
    CHECK(bitmap_test(bitmap, 8) == 1);
    CHECK(bitmap_test(bitmap, 63) == 1);
    CHECK(bitmap_test(bitmap, 64) == 1);
    CHECK(bitmap_test(bitmap, 127) == 1);
    CHECK(bitmap_test(bitmap, 126) == 0);

    bitmap_clear(bitmap, 7);
    CHECK(bitmap_test(bitmap, 7) == 0);
    CHECK(bitmap_find_first_clear(bitmap, 128) == 1);

    bitmap_clear(bitmap, 0);
    CHECK(bitmap_find_first_clear(bitmap, 128) == 0);

    for (i = 0; i < 128; i++)
        bitmap_set(bitmap, i);
    CHECK(bitmap_find_first_clear(bitmap, 128) == 128);

    bitmap_clear(bitmap, 100);
    CHECK(bitmap_test(bitmap, 99) == 1);
    CHECK(bitmap_test(bitmap, 100) == 0);
    CHECK(bitmap_test(bitmap, 101) == 1);
    CHECK(bitmap_find_first_clear(bitmap, 128) == 100);
    CHECK(bitmap_find_first_clear(bitmap, 64) == 64);
    CHECK(bitmap_find_first_clear(bitmap, 0) == 0);
}
