/* Colour arithmetic shared by the framebuffer primitives: RGB <-> device
   pixels, alpha blending and gradient steps.  Free of framebuffer state so the
   host tests can check every case. */
#ifndef CRUG_COLOR_H
#define CRUG_COLOR_H

#include <crug/types.h>

/* The same values as CRUG_PIXEL_RGBX8888 / CRUG_PIXEL_BGRX8888 in
   include/crug/bootinfo.h; kernel/arch/x86_64/fb.c asserts that at build time. */
#define CRUG_COLOR_RGBX 0u
#define CRUG_COLOR_BGRX 1u

#define CRUG_COLOR_ALPHA_MAX 255u

/* 0x00RRGGBB -> framebuffer word for the given pixel format. */
u32 color_pack(u32 rgb, u32 format);

/* Framebuffer word -> 0x00RRGGBB. */
u32 color_unpack(u32 device, u32 format);

/* Blend src over dst in RGB space: alpha 0 keeps dst, 255 returns src. */
u32 color_blend(u32 dst_rgb, u32 src_rgb, u32 alpha);

/* Blend over an existing framebuffer word and return a framebuffer word. */
u32 color_blend_device(u32 dst_device, u32 src_rgb, u32 alpha, u32 format);

/* Linear ramp: index 0 maps to from, index count-1 to to.  A count below 2
   has no distance to cover and returns from. */
u32 color_lerp(u32 from_rgb, u32 to_rgb, u32 index, u32 count);

#endif /* CRUG_COLOR_H */