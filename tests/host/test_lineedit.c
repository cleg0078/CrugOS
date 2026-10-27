/* Host tests for the serial passphrase line editor. */
#include <string.h>
#include "test.h"
#include <crug/lineedit.h>

static void feed_str(struct lineedit *le, const char *s)
{
    while (*s)
        lineedit_feed(le, *s++);
}

void test_lineedit_functions(void)
{
    char buf[8];
    struct lineedit le;
    int i;

    lineedit_init(&le, buf, sizeof(buf));
    CHECK(le.len == 0);
    CHECK(le.overflow == 0);
    CHECK(buf[0] == 0);
    CHECK(lineedit_feed(&le, 'a') == LINEEDIT_MORE);
    CHECK(lineedit_feed(&le, 'b') == LINEEDIT_MORE);
    CHECK(lineedit_feed(&le, 'c') == LINEEDIT_MORE);
    CHECK(le.len == 3);
    CHECK_STR(buf, "abc");
    CHECK(lineedit_feed(&le, '\r') == LINEEDIT_DONE);
    CHECK_STR(buf, "abc");

    /* Backspace removes one character and keeps the string terminated. */
    lineedit_init(&le, buf, sizeof(buf));
    feed_str(&le, "abcd");
    CHECK(lineedit_feed(&le, 0x7F) == LINEEDIT_MORE);
    CHECK_STR(buf, "abc");
    CHECK(le.len == 3);
    CHECK(lineedit_feed(&le, 0x08) == LINEEDIT_MORE);
    CHECK_STR(buf, "ab");
    CHECK(lineedit_feed(&le, 0x08) == LINEEDIT_MORE);
    CHECK(lineedit_feed(&le, 0x08) == LINEEDIT_MORE);
    CHECK(lineedit_feed(&le, 0x08) == LINEEDIT_MORE);
    CHECK(le.len == 0);
    CHECK_STR(buf, "");
    feed_str(&le, "xy");
    CHECK(lineedit_feed(&le, '\n') == LINEEDIT_DONE);
    CHECK_STR(buf, "xy");

    /* Overflow is sticky and never writes past the buffer. */
    lineedit_init(&le, buf, 4);
    for (i = 0; i < 10; i++)
        CHECK(lineedit_feed(&le, 'z') == LINEEDIT_MORE);
    CHECK(le.overflow == 1);
    CHECK(le.len == 3);
    CHECK_STR(buf, "zzz");
    CHECK(buf[3] == 0);
    CHECK(lineedit_feed(&le, '\r') == LINEEDIT_DONE);
    CHECK_STR(buf, "zzz");

    /* Control characters other than CR/LF/backspace/Ctrl-C/Esc are ignored. */
    lineedit_init(&le, buf, sizeof(buf));
    CHECK(lineedit_feed(&le, 0x01) == LINEEDIT_MORE);
    CHECK(lineedit_feed(&le, 0x09) == LINEEDIT_MORE);
    CHECK(le.len == 0);
    feed_str(&le, "ok");
    CHECK(lineedit_feed(&le, 0x1B) == LINEEDIT_CANCEL);
    CHECK_STR(buf, "ok");
    lineedit_init(&le, buf, sizeof(buf));
    feed_str(&le, "x");
    CHECK(lineedit_feed(&le, 0x03) == LINEEDIT_CANCEL);
    CHECK_STR(buf, "x");

    /* Non-ASCII bytes are kept (UTF-8 passphrases typed over serial). */
    lineedit_init(&le, buf, sizeof(buf));
    CHECK(lineedit_feed(&le, (char)0xC3) == LINEEDIT_MORE);
    CHECK(lineedit_feed(&le, (char)0xA9) == LINEEDIT_MORE);
    CHECK(le.len == 2);
    CHECK((unsigned char)buf[0] == 0xC3);
    CHECK((unsigned char)buf[1] == 0xA9);
    CHECK(lineedit_feed(&le, '\r') == LINEEDIT_DONE);

    /* A zero-capacity buffer is handled without dereferencing it. */
    lineedit_init(&le, 0, 0);
    CHECK(lineedit_feed(&le, 'q') == LINEEDIT_MORE);
    CHECK(le.overflow == 1);
    CHECK(lineedit_feed(&le, '\r') == LINEEDIT_DONE);
    CHECK(le.len == 0);
}