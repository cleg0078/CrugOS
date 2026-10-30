/* Pure paint geometry shared by the framebuffer primitives: rounded corner
   insets, gradient rows and shadow alphas.  Host-testable. */
#ifndef CRUG_PAINT_H
#define CRUG_PAINT_H

#include <crug/types.h>

/* Horizontal inset of a rounded corner on row dy (0 is the outermost row of
   the corner); rows at or below the radius are not inset at all. */
u32 paint_round_inset(u32 radius, u32 dy);

/* Colour of row index of a vertical gradient that spans count rows. */
u32 paint_gradient_rgb(u32 top_rgb, u32 bottom_rgb, u32 index, u32 count);

/* Alpha of shadow band band (0 is closest to the box) for a shadow of depth
   bands: it starts at strength and fades to floor. */
u32 paint_shadow_alpha(u32 band, u32 depth, u32 strength, u32 floor);

#endif /* CRUG_PAINT_H */