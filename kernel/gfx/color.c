/* Colour arithmetic for the framebuffer: packing, blending, ramps. */
#include <crug/types.h>
#include <crug/color.h>

u32 color_pack(u32 rgb, u32 format)
{
    u32 r = (rgb >> 16) & 0xFFu;
    u32 g = (rgb >> 8) & 0xFFu;
    u32 b = rgb & 0xFFu;

    /* BGRX8888 stores blue in the lowest byte, so the natural
       (r << 16) | (g << 8) | b packing already matches it.  RGBX8888 stores
       red there instead and needs the swap. */
    if (format == CRUG_COLOR_RGBX) {
        return (b << 16) | (g << 8) | r;
    }
    return (r << 16) | (g << 8) | b;
}

u32 color_unpack(u32 device, u32 format)
{
    if (format == CRUG_COLOR_RGBX) {
        u32 r = device & 0xFFu;
        u32 b = (device >> 16) & 0xFFu;
        return (r << 16) | (device & 0xFF00u) | b;
    }
    return device & 0x00FFFFFFu;
}

u32 color_blend(u32 dst_rgb, u32 src_rgb, u32 alpha)
{
    u32 out = 0u;
    u32 shift;

    if (alpha == 0u) {
        return dst_rgb & 0x00FFFFFFu;
    }
    if (alpha >= CRUG_COLOR_ALPHA_MAX) {
        return src_rgb & 0x00FFFFFFu;
    }
    for (shift = 0u; shift < 24u; shift += 8u) {
        u32 d = (dst_rgb >> shift) & 0xFFu;
        u32 s = (src_rgb >> shift) & 0xFFu;
        u32 v = (d * (CRUG_COLOR_ALPHA_MAX - alpha) + s * alpha + 127u) /
                CRUG_COLOR_ALPHA_MAX;
        out |= (v & 0xFFu) << shift;
    }
    return out;
}

u32 color_blend_device(u32 dst_device, u32 src_rgb, u32 alpha, u32 format)
{
    return color_pack(color_blend(color_unpack(dst_device, format), src_rgb, alpha),
                      format);
}

u32 color_lerp(u32 from_rgb, u32 to_rgb, u32 index, u32 count)
{
    u32 out = 0u;
    u32 shift;
    u32 den;

    if (count < 2u || index == 0u) {
        return from_rgb & 0x00FFFFFFu;
    }
    den = count - 1u;
    if (index >= den) {
        return to_rgb & 0x00FFFFFFu;
    }
    for (shift = 0u; shift < 24u; shift += 8u) {
        i32 a = (i32)((from_rgb >> shift) & 0xFFu);
        i32 b = (i32)((to_rgb >> shift) & 0xFFu);
        i32 v = a + ((b - a) * (i32)index) / (i32)den;
        out |= ((u32)(v & 0xFF)) << shift;
    }
    return out;
}