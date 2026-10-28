/* Fixed-size console text grid with scrollback, used by the Workbench shell
   window.  No heap, no libc: the grid is a plain struct the caller owns. */
#include <crug/types.h>
#include <crug/string.h>
#include <crug/fb.h>
#include <crug/textgrid.h>

static u32 tg_slot(const struct textgrid *tg, u32 index)
{
    return (tg->first + index) % TG_LINES;
}

static char *tg_line_mut(struct textgrid *tg, u32 index)
{
    return tg->lines[tg_slot(tg, index)];
}

static const char *tg_line_ro(const struct textgrid *tg, u32 index)
{
    return tg->lines[tg_slot(tg, index)];
}

static void tg_zero_line(struct textgrid *tg, u32 index)
{
    memset(tg_line_mut(tg, index), 0, TG_COLS);
}

/* Move to a fresh empty line, evicting the oldest one when the ring is
   full.  The cursor always ends up at column 0 of the new line. */
static void tg_new_line(struct textgrid *tg)
{
    if (tg->count < TG_LINES) {
        tg->count++;
        tg->row = tg->count - 1u;
    } else {
        tg->first = (tg->first + 1u) % TG_LINES;
        tg->row = TG_LINES - 1u;
    }
    tg_zero_line(tg, tg->row);
    tg->col = 0;
}

void tg_init(struct textgrid *tg)
{
    if (!tg)
        return;
    memset(tg, 0, sizeof(*tg));
    tg->first = 0;
    tg->count = 1;
    tg->row = 0;
    tg->col = 0;
    tg_zero_line(tg, 0);
}

void tg_clear(struct textgrid *tg)
{
    if (!tg)
        return;
    tg->first = 0;
    tg->count = 1;
    tg->row = 0;
    tg->col = 0;
    memset(tg->lines[0], 0, TG_COLS);
}

void tg_putc(struct textgrid *tg, u8 c)
{
    char *line;

    if (!tg)
        return;
    if (c == '\n') {
        tg_new_line(tg);
        return;
    }
    if (c == '\r') {
        tg->col = 0;
        return;
    }
    if (c == 0x08 || c == 0x7F) {
        tg_backspace(tg);
        return;
    }
    if (c < 0x20 || c > 0x7E)
        c = '.';
    if (tg->col + 1u >= TG_COLS)
        tg_new_line(tg);
    line = tg_line_mut(tg, tg->row);
    /* Overwriting inside the line keeps the tail (tg_putc after '\r'),
       appending terminates the new end. */
    {
        u32 old_len = (u32)strlen(line);

        line[tg->col] = (char)c;
        tg->col++;
        if (tg->col > old_len)
            line[tg->col] = 0;
    }
}

void tg_puts(struct textgrid *tg, const char *s)
{
    if (!tg || !s)
        return;
    while (*s)
        tg_putc(tg, (u8)*s++);
}

void tg_backspace(struct textgrid *tg)
{
    char *line;

    if (!tg || tg->col == 0)
        return;
    tg->col--;
    line = tg_line_mut(tg, tg->row);
    line[tg->col] = 0;
}

u32 tg_line_count(const struct textgrid *tg)
{
    return tg ? tg->count : 0;
}

u32 tg_cursor_row(const struct textgrid *tg)
{
    return tg ? tg->row : 0;
}

u32 tg_cursor_col(const struct textgrid *tg)
{
    return tg ? tg->col : 0;
}

int tg_line(const struct textgrid *tg, u32 index, char *out, usize cap)
{
    const char *line;
    usize len;

    if (!tg || !out || cap == 0)
        return -1;
    if (index >= tg->count)
        return -1;
    line = tg_line_ro(tg, index);
    len = strlen(line);
    if (len > cap - 1u)
        len = cap - 1u;
    memcpy(out, line, len);
    out[len] = 0;
    return (int)len;
}

u32 tg_draw(const struct textgrid *tg, u32 x, u32 y, u32 w, u32 h, u32 fg,
            u32 bg)
{
    char buf[TG_COLS];
    u32 rows;
    u32 first_vis;
    u32 max_chars;
    u32 i;
    u32 cursor_line;

    if (!tg || w <= 12u || h < 6u + TG_CELL_H)
        return 0;
    rows = (h - 6u) / TG_LINE_PITCH;
    if (rows > TG_ROWS_VISIBLE)
        rows = TG_ROWS_VISIBLE;
    if (rows > tg->count)
        rows = tg->count;
    if (rows == 0)
        return 0;
    first_vis = tg->count - rows;
    max_chars = (w - 12u) / TG_CELL_W;
    if (max_chars > TG_COLS - 1u)
        max_chars = TG_COLS - 1u;
    for (i = 0; i < rows; i++) {
        if (tg_line(tg, first_vis + i, buf, sizeof(buf)) < 0)
            continue;
        if (strlen(buf) > max_chars)
            buf[max_chars] = 0;
        fb_draw_text(x + 6u, y + 6u + i * TG_LINE_PITCH, buf, fg, bg);
    }
    /* Inverse-video block cursor, drawn only when it is on screen. */
    cursor_line = tg->row;
    if (cursor_line >= first_vis && cursor_line - first_vis < rows &&
        tg->col < max_chars) {
        char cell[2];
        u32 cx = x + 6u + tg->col * TG_CELL_W;
        u32 cy = y + 6u + (cursor_line - first_vis) * TG_LINE_PITCH;

        cell[0] = (char)tg_line_ro(tg, cursor_line)[tg->col];
        if (cell[0] == 0)
            cell[0] = ' ';
        cell[1] = 0;
        fb_draw_text(cx, cy, cell, bg, fg);
    }
    return rows;
}