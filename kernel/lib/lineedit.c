/* Serial line editor. Deliberately console-agnostic so the host tests can
   exercise the exact editing rules used by the boot passphrase prompt. */
#include <crug/lineedit.h>

static void lineedit_terminate(struct lineedit *le)
{
    if (le->buf == 0 || le->cap == 0)
        return;
    if (le->len >= le->cap)
        le->len = le->cap - 1;
    le->buf[le->len] = 0;
}

void lineedit_init(struct lineedit *le, char *buf, u32 cap)
{
    le->buf = buf;
    le->cap = cap;
    le->len = 0;
    le->overflow = 0;
    if (buf != 0 && cap != 0)
        buf[0] = 0;
}

enum lineedit_status lineedit_feed(struct lineedit *le, char c)
{
    u8 ch = (u8)c;

    if (ch == '\r' || ch == '\n') {
        lineedit_terminate(le);
        return LINEEDIT_DONE;
    }
    if (ch == 0x7F || ch == 0x08) {
        if (le->len > 0)
            le->len--;
        lineedit_terminate(le);
        return LINEEDIT_MORE;
    }
    if (ch == 0x03 || ch == 0x1B) {
        lineedit_terminate(le);
        return LINEEDIT_CANCEL;
    }
    if (ch < 0x20)
        return LINEEDIT_MORE;
    if (le->cap == 0 || le->len + 1 >= le->cap) {
        le->overflow = 1;
        return LINEEDIT_MORE;
    }
    le->buf[le->len++] = (char)ch;
    lineedit_terminate(le);
    return LINEEDIT_MORE;
}