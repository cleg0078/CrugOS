/* Phase UI: procedural desktop icons.  Every icon is drawn from rectangles
   scaled from a 48 px tile, so the same code paints the 48 px glass grid and
   the 16 px list rows of the file browser. */
#ifndef CRUG_ICONS_H
#define CRUG_ICONS_H

#include <crug/types.h>

#define ICONS_SIZE 48u

/* One icon kind per desktop object; the numbers match intuition.h. */
#define ICONS_VOLUME 0u
#define ICONS_DRAWER 1u
#define ICONS_FILE 2u

void icons_draw(u32 kind, u32 x, u32 y, u32 size);

#endif