/* Host tests for the phase 6C text grid (kernel/lib/textgrid.c): line
   editing, wrapping at the fixed column count, ring eviction of the oldest
   scrollback lines, cursor tracking and the row arithmetic of tg_draw. */
#include <crug/types.h>
#include <crug/kernel.h>
#include <crug/textgrid.h>
#include <string.h>
#include "test.h"

static struct textgrid g_grid;

static void reset(void)
{
    tg_init(&g_grid);
}

static void check_line(u32 index, const char *expected)
{
    char buf[TG_COLS];

    CHECK(tg_line(&g_grid, index, buf, sizeof(buf)) >= 0);
    CHECK_STR(buf, expected);
}

static void test_textgrid_init(void)
{
    char buf[TG_COLS];

    reset();
    CHECK(tg_line_count(&g_grid) == 1u);
    CHECK(tg_cursor_row(&g_grid) == 0u);
    CHECK(tg_cursor_col(&g_grid) == 0u);
    CHECK(tg_line(&g_grid, 0, buf, sizeof(buf)) == 0);
    CHECK_STR(buf, "");
    CHECK(tg_line(&g_grid, 1, buf, sizeof(buf)) == -1);
    CHECK(tg_line(NULL, 0, buf, sizeof(buf)) == -1);
    CHECK(tg_line(&g_grid, 0, buf, 0) == -1);
    CHECK(tg_line_count(NULL) == 0u);
    CHECK(tg_cursor_row(NULL) == 0u);
    CHECK(tg_cursor_col(NULL) == 0u);
}

static void test_textgrid_editing(void)
{
    reset();
    tg_puts(&g_grid, "hello");
    CHECK(tg_cursor_col(&g_grid) == 5u);
    CHECK(tg_line_count(&g_grid) == 1u);
    check_line(0, "hello");

    tg_backspace(&g_grid);
    CHECK(tg_cursor_col(&g_grid) == 4u);
    check_line(0, "hell");
    tg_backspace(&g_grid);
    tg_backspace(&g_grid);
    tg_backspace(&g_grid);
    tg_backspace(&g_grid);
    CHECK(tg_cursor_col(&g_grid) == 0u);
    tg_backspace(&g_grid); /* at column 0: no-op, no underflow */
    CHECK(tg_cursor_col(&g_grid) == 0u);

    tg_putc(&g_grid, 'a');
    tg_putc(&g_grid, (u8)'\n');
    CHECK(tg_line_count(&g_grid) == 2u);
    CHECK(tg_cursor_row(&g_grid) == 1u);
    CHECK(tg_cursor_col(&g_grid) == 0u);
    check_line(0, "a");
    check_line(1, "");

    /* \r returns to column 0 but keeps the line. */
    tg_puts(&g_grid, "xy");
    tg_putc(&g_grid, (u8)'\r');
    CHECK(tg_cursor_col(&g_grid) == 0u);
    check_line(1, "xy");

    /* Control characters are stored as dots, not dropped. */
    tg_putc(&g_grid, 0x01);
    check_line(1, ".y");
}

static void test_textgrid_wrap(void)
{
    char expected[TG_COLS];
    u32 i;

    reset();
    for (i = 0; i < TG_COLS; i++)
        expected[i] = 'x';
    expected[TG_COLS - 1u] = 0;
    for (i = 0; i < TG_COLS + 5u; i++)
        tg_putc(&g_grid, 'x');
    /* TG_COLS - 1 characters fit on a line, the rest wraps. */
    CHECK(tg_line_count(&g_grid) == 2u);
    check_line(0, expected);
    CHECK(tg_cursor_row(&g_grid) == 1u);
    CHECK(tg_cursor_col(&g_grid) == 6u);
    check_line(1, "xxxxxx");

    /* A newline right after a full line does not leave an empty line. */
    reset();
    for (i = 0; i < TG_COLS - 1u; i++)
        tg_putc(&g_grid, 'y');
    CHECK(tg_cursor_col(&g_grid) == TG_COLS - 1u);
    tg_putc(&g_grid, 'z');
    CHECK(tg_line_count(&g_grid) == 2u);
    CHECK(tg_cursor_col(&g_grid) == 1u);
}

static void test_textgrid_ring(void)
{
    char buf[TG_COLS];
    u32 i;
    char text[8];

    reset();
    /* Fill the ring and then push it past its size: the oldest lines go. */
    for (i = 0; i < TG_LINES + 10u; i++) {
        ksnprintf(text, sizeof(text), "l%u", i);
        tg_puts(&g_grid, text);
        tg_putc(&g_grid, (u8)'\n');
    }
    CHECK(tg_line_count(&g_grid) == TG_LINES);
    CHECK(tg_cursor_row(&g_grid) == TG_LINES - 1u);
    /* 107 logical lines were written (line 0 holds "l0" and the final
       newline opened an empty line 107), so the ring now starts at "l11". */
    tg_line(&g_grid, 0, buf, sizeof(buf));
    CHECK_STR(buf, "l11");
    tg_line(&g_grid, TG_LINES - 2u, buf, sizeof(buf));
    CHECK_STR(buf, "l105");
    CHECK(tg_line(&g_grid, TG_LINES - 1u, buf, sizeof(buf)) == 0);
}

static void test_textgrid_clear(void)
{
    reset();
    tg_puts(&g_grid, "one\ntwo\nthree");
    CHECK(tg_line_count(&g_grid) == 3u);
    tg_clear(&g_grid);
    CHECK(tg_line_count(&g_grid) == 1u);
    CHECK(tg_cursor_row(&g_grid) == 0u);
    CHECK(tg_cursor_col(&g_grid) == 0u);
    check_line(0, "");
}

static void test_textgrid_truncation(void)
{
    char small[4];

    reset();
    tg_puts(&g_grid, "abcdef");
    CHECK(tg_line(&g_grid, 0, small, sizeof(small)) == 3);
    CHECK_STR(small, "abc");
}

static void test_textgrid_draw(void)
{
    u32 rows;

    reset();
    /* Too narrow, too short: nothing is drawn. */
    CHECK(tg_draw(&g_grid, 0, 0, 12, 200, 0, 0) == 0u);
    CHECK(tg_draw(&g_grid, 0, 0, 200, 6 + TG_CELL_H - 1u, 0, 0) == 0u);
    CHECK(tg_draw(NULL, 0, 0, 200, 200, 0, 0) == 0u);

    /* One line of content draws one row. */
    tg_puts(&g_grid, "hello");
    CHECK(tg_draw(&g_grid, 10, 10, 316, 176, 0, 0) == 1u);

    /* The visible row count follows the height and stops at
       TG_ROWS_VISIBLE. */
    tg_clear(&g_grid);
    tg_puts(&g_grid, "1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11\n12");
    rows = tg_draw(&g_grid, 10, 10, 316, 176, 0, 0);
    CHECK(rows == TG_ROWS_VISIBLE);
    rows = tg_draw(&g_grid, 10, 10, 316, 6 + 3u * TG_LINE_PITCH, 0, 0);
    CHECK(rows == 3u);
    /* A cursor far past the visible window does not add a row. */
    tg_puts(&g_grid, "tail");
    rows = tg_draw(&g_grid, 10, 10, 316, 6 + TG_LINE_PITCH, 0, 0);
    CHECK(rows == 1u);
}

void test_textgrid_functions(void)
{
    test_textgrid_init();
    test_textgrid_editing();
    test_textgrid_wrap();
    test_textgrid_ring();
    test_textgrid_clear();
    test_textgrid_truncation();
    test_textgrid_draw();
}
