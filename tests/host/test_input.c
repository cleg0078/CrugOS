/* Host tests for the PS/2 set-1 scancode decoder. */
#include "test.h"
#include <crug/input.h>

static void expect_char(int expected)
{
    CHECK(input_try_getc() == expected);
}

static void expect_empty(void)
{
    CHECK(input_try_getc() == -1);
}

void test_input_functions(void)
{
    int i;

    input_init();
    expect_empty();

    /* Letters and shifts. */
    input_scancode(0x1E);
    expect_char('a');
    expect_empty();
    input_scancode(0x2A);
    input_scancode(0x1E);
    expect_char('A');
    input_scancode(0xAA);
    input_scancode(0x1E);
    expect_char('a');

    /* Caps Lock toggles letter case and combines with Shift as XOR. */
    input_init();
    input_scancode(0x3A);
    input_scancode(0x1E);
    expect_char('A');
    input_scancode(0x2A);
    input_scancode(0x1E);
    expect_char('a');
    input_scancode(0xAA);
    input_scancode(0x3A);
    input_scancode(0x1E);
    expect_char('a');

    /* Digit row and shifted symbols. */
    input_init();
    input_scancode(0x02);
    expect_char('1');
    input_scancode(0x0B);
    expect_char('0');
    input_scancode(0x2A);
    input_scancode(0x02);
    expect_char('!');
    input_scancode(0x0B);
    expect_char(')');
    input_scancode(0xAA);

    /* Punctuation and the named keys. */
    input_init();
    input_scancode(0x0C);
    expect_char('-');
    input_scancode(0x2A);
    input_scancode(0x0C);
    expect_char('_');
    input_scancode(0xAA);
    input_scancode(0x1A);
    expect_char('[');
    input_scancode(0x1C);
    expect_char('\n');
    input_scancode(0x0F);
    expect_char('\t');
    input_scancode(0x0E);
    expect_char(0x7F);
    input_scancode(0x01);
    expect_char(0x1B);
    input_scancode(0x39);
    expect_char(' ');

    /* Control chords map letters to control codes and release cleanly. */
    input_init();
    input_scancode(0x1D);
    input_scancode(0x2E);
    expect_char(0x03);
    input_scancode(0x9D);
    input_scancode(0x2E);
    expect_char('c');
    input_scancode(0x1D);
    input_scancode(0x1A);
    expect_char(0x1B);
    input_scancode(0x9D);

    /* E0-prefixed keys are ignored (or update right-hand modifiers). */
    input_init();
    input_scancode(0xE0);
    input_scancode(0x48);
    expect_empty();
    input_scancode(0xE0);
    input_scancode(0xC8);
    expect_empty();
    input_scancode(0x1E);
    expect_char('a');
    input_scancode(0xE0);
    input_scancode(0x1D);
    input_scancode(0x2E);
    expect_char(0x03);
    input_scancode(0xE0);
    input_scancode(0x9D);
    input_scancode(0x2E);
    expect_char('c');

    /* ACK/echo, release-only and unmapped codes produce nothing. */
    input_init();
    input_scancode(0xFA);
    input_scancode(0xEE);
    input_scancode(0x9E);
    input_scancode(0x00);
    input_scancode(0x7F);
    expect_empty();

    /* The queue drops bytes past its 64-byte capacity. */
    input_init();
    for (i = 0; i < 100; i++)
        input_scancode(0x1E);
    for (i = 0; i < 64; i++)
        expect_char('a');
    expect_empty();
}
