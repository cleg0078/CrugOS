/* Paint geometry: corner insets, gradients and shadow ramps. */
#include <crug/types.h>
#include <crug/color.h>
#include <crug/paint.h>

u32 paint_round_inset(u32 radius, u32 dy)
{
    u64 rr;
    u64 yy;
    u32 dx;

    if (radius == 0u || dy >= radius) {
        return 0u;
    }
    rr = (u64)radius * (u64)radius;
    yy = (u64)(radius - 1u - dy) * (u64)(radius - 1u - dy);
    /* Find the first column of this row whose pixel still lies inside the
       corner circle centred at (radius, radius). */
    for (dx = 0u; dx < radius; dx++) {
        u64 xx = (u64)(radius - dx) * (u64)(radius - dx);
        if (xx + yy <= rr) {
            break;
        }
    }
    return dx;
}

u32 paint_gradient_rgb(u32 top_rgb, u32 bottom_rgb, u32 index, u32 count)
{
    return color_lerp(top_rgb, bottom_rgb, index, count);
}

u32 paint_shadow_alpha(u32 band, u32 depth, u32 strength, u32 floor)
{
    u32 step;

    if (depth == 0u) {
        return 0u;
    }
    if (band >= depth) {
        return floor;
    }
    if (depth == 1u) {
        return strength;
    }
    step = (strength > floor) ? (strength - floor) / (depth - 1u) : 0u;
    return strength - (band * step);
}