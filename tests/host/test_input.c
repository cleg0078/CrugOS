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

    /* E0-prefixed arrows are decoded (phase 6D) and their break codes are
       ignored; E0 modifier presses still apply to the next key. */
    input_init();
    input_scancode(0xE0);
    input_scancode(0x48);
    expect_char(INPUT_KEY_UP);
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

    /* Phase 6D: the remaining E0 arrows become INPUT_KEY_* bytes, and a
       held ctrl (E0 1D) does not swallow the following E0 arrow. */
    input_init();
    input_scancode(0xE0);
    input_scancode(0x50);
    expect_char(INPUT_KEY_DOWN);
    input_scancode(0xE0);
    input_scancode(0x4B);
    expect_char(INPUT_KEY_LEFT);
    input_scancode(0xE0);
    input_scancode(0x4D);
    expect_char(INPUT_KEY_RIGHT);
    input_scancode(0xE0);
    input_scancode(0x9D);   /* right ctrl release */
    input_scancode(0xE0);
    input_scancode(0x1D);   /* right ctrl press */
    input_scancode(0xE0);
    input_scancode(0x48);   /* ctrl+up is still an arrow */
    expect_char(INPUT_KEY_UP);
    expect_empty();

    /* ACK/echo, release-only and unmapped codes produce nothing. */
    input_init();
    input_scancode(0xFA);
    input_scancode(0xEE);
    input_scancode(0x9E);
    input_scancode(0x00);
    input_scancode(0x7F);
    expect_empty();

    /* A USB HID device feeds the same character queue through input_char. */
    input_init();
    input_char('q');
    expect_char('q');
    input_char('\n');
    expect_char('\n');
    expect_empty();

    /* Pointer events accumulate until read and then reset. */
    input_init();
    CHECK(input_pointer_count() == 0);
    CHECK(input_pointer_read(NULL) == 0);
    {
        struct input_pointer p;

        CHECK(input_pointer_read(&p) == 0);
        input_pointer_event(0, 3, 4, 0);
        input_pointer_event(0, -1, 2, -1);
        CHECK(input_pointer_count() == 2);
        CHECK(input_pointer_read(&p) == 1);
        CHECK(p.dx == 2 && p.dy == 6 && p.wheel == -1);
        CHECK(p.buttons == 0 && p.changes == 0);
        CHECK(input_pointer_read(&p) == 0);

        /* Button changes are accumulated as a bitmask of every transition. */
        input_pointer_event(1, 0, 0, 0);
        input_pointer_event(1, 0, 0, 0);
        input_pointer_event(3, 0, 0, 0);
        input_pointer_event(0, 0, 0, 0);
        CHECK(input_pointer_read(&p) == 1);
        CHECK(p.buttons == 0);
        CHECK(p.changes == 0x03);
        CHECK(p.dx == 0 && p.dy == 0 && p.wheel == 0);

        /* Reset drops the accumulators and the event counter. */
        input_pointer_event(2, -7, 9, 0);
        input_pointer_reset();
        CHECK(input_pointer_count() == 0);
        CHECK(input_pointer_read(&p) == 0);
        input_pointer_event(0, 1, 1, 0);
        CHECK(input_pointer_count() == 1);
        CHECK(input_pointer_read(&p) == 1);
        CHECK(p.dx == 1 && p.dy == 1);
    }

    /* The queue drops bytes past its 64-byte capacity. */
    input_init();
    for (i = 0; i < 100; i++)
        input_scancode(0x1E);
    for (i = 0; i < 64; i++)
        expect_char('a');
    expect_empty();
}
