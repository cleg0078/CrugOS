/* Rounded-corner insets, gradients and shadow ramps. */
#include <crug/paint.h>
#include "test.h"

void test_paint_functions(void)
{
    u32 r;
    u32 dy;

    /* No radius and rows past the corner are not inset. */
    CHECK(paint_round_inset(0u, 0u) == 0u);
    CHECK(paint_round_inset(0u, 5u) == 0u);
    CHECK(paint_round_inset(4u, 4u) == 0u);
    CHECK(paint_round_inset(4u, 9u) == 0u);

    /* Known stair for a six-pixel corner. */
    CHECK(paint_round_inset(6u, 0u) == 3u);
    CHECK(paint_round_inset(6u, 1u) == 2u);
    CHECK(paint_round_inset(6u, 2u) == 1u);
    CHECK(paint_round_inset(6u, 3u) == 1u);
    CHECK(paint_round_inset(6u, 4u) == 1u);
    CHECK(paint_round_inset(6u, 5u) == 0u);

    /* An inset never grows as the row moves away from the corner, and it
       always leaves at least the middle of the row. */
    for (r = 1u; r <= 12u; r++) {
        u32 previous = r;
        for (dy = 0u; dy < r; dy++) {
            u32 inset = paint_round_inset(r, dy);
            CHECK(inset <= previous);
            CHECK(inset < r);
            previous = inset;
        }
    }

    /* Gradients: endpoints exact, ramp in between, degenerate spans flat. */
    CHECK(paint_gradient_rgb(0x000000u, 0xFFFFFFu, 0u, 2u) == 0x000000u);
    CHECK(paint_gradient_rgb(0x000000u, 0xFFFFFFu, 1u, 2u) == 0x00FFFFFFu);
    CHECK(paint_gradient_rgb(0x00102030u, 0x00405060u, 0u, 1u) == 0x00102030u);
    CHECK(paint_gradient_rgb(0x00102030u, 0x00405060u, 0u, 0u) == 0x00102030u);
    CHECK(paint_gradient_rgb(0x000000u, 0xFFFFFFu, 1u, 3u) == 0x007F7F7Fu);

    /* Shadow ramps start strong, end at the floor and never exceed strength. */
    CHECK(paint_shadow_alpha(0u, 2u, 96u, 24u) == 96u);
    CHECK(paint_shadow_alpha(1u, 2u, 96u, 24u) == 24u);
    CHECK(paint_shadow_alpha(0u, 1u, 96u, 24u) == 96u);
    CHECK(paint_shadow_alpha(0u, 0u, 96u, 24u) == 0u);
    CHECK(paint_shadow_alpha(5u, 2u, 96u, 24u) == 24u);
    {
        u32 band;
        u32 previous = 256u;
        for (band = 0u; band < 4u; band++) {
            u32 alpha = paint_shadow_alpha(band, 4u, 96u, 24u);
            CHECK(alpha <= 96u);
            CHECK(alpha <= previous);
            previous = alpha;
        }
    }
}