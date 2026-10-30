/* Colour packing, blending and ramp arithmetic. */
#include <crug/color.h>
#include "test.h"

void test_color_functions(void)
{
    /* Packing swaps red and blue only for RGBX8888. */
    CHECK(color_pack(0x00112233u, CRUG_COLOR_BGRX) == 0x00112233u);
    CHECK(color_pack(0x00112233u, CRUG_COLOR_RGBX) == 0x00332211u);
    CHECK(color_unpack(0x00332211u, CRUG_COLOR_RGBX) == 0x00112233u);
    CHECK(color_unpack(0x00112233u, CRUG_COLOR_BGRX) == 0x00112233u);
    CHECK(color_unpack(0xFFFFFFFFu, CRUG_COLOR_BGRX) == 0x00FFFFFFu);
    CHECK(color_unpack(0xFFFFFFFFu, CRUG_COLOR_RGBX) == 0x00FFFFFFu);

    /* Round trips in both formats, including single channels. */
    CHECK(color_unpack(color_pack(0x00010203u, CRUG_COLOR_BGRX), CRUG_COLOR_BGRX) ==
          0x00010203u);
    CHECK(color_unpack(color_pack(0x00010203u, CRUG_COLOR_RGBX), CRUG_COLOR_RGBX) ==
          0x00010203u);
    CHECK(color_unpack(color_pack(0x00FF0000u, CRUG_COLOR_RGBX), CRUG_COLOR_RGBX) ==
          0x00FF0000u);
    CHECK(color_unpack(color_pack(0x000000FFu, CRUG_COLOR_RGBX), CRUG_COLOR_RGBX) ==
          0x000000FFu);
    CHECK(color_pack(0x00FF0000u, CRUG_COLOR_RGBX) == 0x000000FFu);
    CHECK(color_pack(0x000000FFu, CRUG_COLOR_RGBX) == 0x00FF0000u);

    /* Alpha extremes are exact. */
    CHECK(color_blend(0x00102030u, 0x00FFFFFFu, 0u) == 0x00102030u);
    CHECK(color_blend(0x00102030u, 0x00FFFFFFu, 255u) == 0x00FFFFFFu);
    CHECK(color_blend(0x00102030u, 0x00FFFFFFu, 128u) == 0x00889098u);
    CHECK(color_blend(0x00000000u, 0x00000000u, 128u) == 0x00000000u);
    CHECK(color_blend(0x00FFFFFFu, 0x00FFFFFFu, 64u) == 0x00FFFFFFu);

    /* Blending over a device word matches blending over the RGB value. */
    CHECK(color_blend_device(color_pack(0x00102030u, CRUG_COLOR_BGRX), 0x00FFFFFFu,
                             128u, CRUG_COLOR_BGRX) ==
          color_pack(0x00889098u, CRUG_COLOR_BGRX));
    CHECK(color_blend_device(color_pack(0x00102030u, CRUG_COLOR_RGBX), 0x00FFFFFFu,
                             128u, CRUG_COLOR_RGBX) ==
          color_pack(0x00889098u, CRUG_COLOR_RGBX));

    /* Ramps include both endpoints and interpolate in between. */
    CHECK(color_lerp(0x000000u, 0xFFFFFFu, 0u, 2u) == 0x000000u);
    CHECK(color_lerp(0x000000u, 0xFFFFFFu, 1u, 2u) == 0x00FFFFFFu);
    CHECK(color_lerp(0x00102030u, 0x00405060u, 0u, 0u) == 0x00102030u);
    CHECK(color_lerp(0x00102030u, 0x00405060u, 7u, 1u) == 0x00102030u);
    CHECK(color_lerp(0x000000u, 0xFFFFFFu, 1u, 3u) == 0x007F7F7Fu);
    CHECK(color_lerp(0x00FFFFFFu, 0x000000u, 1u, 3u) == 0x00808080u);
    CHECK(color_lerp(0x00102030u, 0x00405060u, 9u, 4u) == 0x00405060u);
}