/* textgrid: a fixed console text grid with scrollback (phase 6C).

   The Workbench shell renders into one of these instead of the boot console:
   the grid owns a ring of text lines and a cursor, and tg_draw() blits the
   visible tail into a rectangle with the framebuffer primitives.  Nothing in
   here allocates, so the shell borrows no heap and the host tests can drive
   the whole thing without a framebuffer. */
#ifndef CRUG_TEXTGRID_H
#define CRUG_TEXTGRID_H

#include <crug/types.h>

/* 38 columns and 8 visible rows is what the Shell window fits: 320 px wide
   minus two 2 px borders and a 12 px text margin at 8 px per glyph, and
   196 px of body at the 20 px line pitch the desktop font uses. */
#define TG_COLS 38u
#define TG_ROWS_VISIBLE 8u
#define TG_LINES 96u
#define TG_CELL_W 8u
#define TG_CELL_H 16u
#define TG_LINE_PITCH 20u

struct textgrid {
    /* Each line is TG_COLS bytes and always NUL-terminated, so at most
       TG_COLS - 1 printable cells are used. */
    char lines[TG_LINES][TG_COLS];
    u32 first; /* ring slot of the oldest retained line */
    u32 count; /* retained lines, always at least 1 */
    u32 row;   /* cursor line, 0 .. count - 1 */
    u32 col;   /* cursor column, 0 .. TG_COLS - 1 */
};

void tg_init(struct textgrid *tg);
void tg_clear(struct textgrid *tg);
void tg_putc(struct textgrid *tg, u8 c);
void tg_puts(struct textgrid *tg, const char *s);
void tg_backspace(struct textgrid *tg);
u32 tg_line_count(const struct textgrid *tg);
u32 tg_cursor_row(const struct textgrid *tg);
u32 tg_cursor_col(const struct textgrid *tg);
int tg_line(const struct textgrid *tg, u32 index, char *out, usize cap);
/* Blit the visible tail into (x, y, w, h) and return the rows drawn. */
u32 tg_draw(const struct textgrid *tg, u32 x, u32 y, u32 w, u32 h, u32 fg,
            u32 bg);

#endif /* CRUG_TEXTGRID_H */