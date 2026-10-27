/* Small line editor for the serial passphrase prompt.
   Input arrives one character at a time; masking/echo is the caller's job. */
#ifndef CRUG_LINEEDIT_H
#define CRUG_LINEEDIT_H

#include <crug/types.h>

enum lineedit_status {
    LINEEDIT_MORE = 0,
    LINEEDIT_DONE = 1,
    LINEEDIT_CANCEL = 2
};

struct lineedit {
    char *buf;   /* NUL-terminated line, may be NULL when cap == 0 */
    u32 cap;     /* buffer capacity in bytes, including the NUL */
    u32 len;     /* characters accepted so far */
    int overflow;/* input exceeded the buffer at some point */
};

void lineedit_init(struct lineedit *le, char *buf, u32 cap);
enum lineedit_status lineedit_feed(struct lineedit *le, char c);

#endif /* CRUG_LINEEDIT_H */