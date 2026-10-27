/* Minimal kernel formatter: %c %s %d %i %u %x %X %p, %%, lengths l/ll/z. */
#include <crug/types.h>
#include <crug/string.h>
#include <crug/kernel.h>

struct out_buf {
    char *buf;
    usize cap;
    usize len;
};

static void out_ch(struct out_buf *o, char c)
{
    if (o->cap && o->len + 1 < o->cap) {
        o->buf[o->len] = c;
    }
    o->len++;
}

static void out_pad(struct out_buf *o, u32 *written, u32 count, char c)
{
    while (*written < count) {
        out_ch(o, c);
        (*written)++;
    }
}

enum length_mod { LEN_INT, LEN_LONG, LEN_LONGLONG, LEN_SIZE };

static void out_unsigned(struct out_buf *o, u64 value, u32 base, int upper,
                         u32 width, int zero_pad, int left_align, char sign)
{
    char tmp[32];
    u32 i = 0;
    u32 written = 0;
    u32 digits;

    if (value == 0) {
        tmp[i++] = '0';
    }
    while (value) {
        u32 d = (u32)(value % base);
        tmp[i++] = (char)(d < 10 ? ('0' + d) : ((upper ? 'A' : 'a') + (d - 10)));
        value /= base;
    }
    digits = i + (sign ? 1u : 0u);

    if (!left_align && !zero_pad) {
        out_pad(o, &written, width > digits ? width - digits : 0, ' ');
    }
    if (sign) {
        out_ch(o, sign);
        written++;
    }
    if (!left_align && zero_pad) {
        out_pad(o, &written, width > digits ? width - digits : 0, '0');
    }
    while (i) {
        out_ch(o, tmp[--i]);
        written++;
    }
    if (left_align) {
        out_pad(o, &written, width, ' ');
    }
}

int kvsnprintf(char *buf, usize size, const char *fmt, va_list ap)
{
    struct out_buf o;
    const char *p;

    o.buf = buf;
    o.cap = size;
    o.len = 0;

    for (p = fmt; *p; p++) {
        int zero_pad = 0;
        int left_align = 0;
        int force_sign = 0;
        int space_sign = 0;
        u32 width = 0;
        enum length_mod len = LEN_INT;
        char conv;

        if (*p != '%') {
            out_ch(&o, *p);
            continue;
        }
        p++;

        for (;;) {
            if (*p == '0') {
                zero_pad = 1;
            } else if (*p == '-') {
                left_align = 1;
            } else if (*p == '+') {
                force_sign = 1;
            } else if (*p == ' ') {
                space_sign = 1;
            } else {
                break;
            }
            p++;
        }
        while (*p >= '0' && *p <= '9') {
            width = width * 10 + (u32)(*p - '0');
            p++;
        }
        if (*p == 'l') {
            p++;
            if (*p == 'l') {
                p++;
                len = LEN_LONGLONG;
            } else {
                len = LEN_LONG;
            }
        } else if (*p == 'z') {
            p++;
            len = LEN_SIZE;
        }

        conv = *p;
        if (!conv) {
            break;
        }

        switch (conv) {
        case '%':
            out_ch(&o, '%');
            break;
        case 'c': {
            char c = (char)va_arg(ap, int);
            if (!left_align) {
                u32 written = 0;
                out_pad(&o, &written, width > 1 ? width - 1 : 0, ' ');
            }
            out_ch(&o, c);
            if (left_align) {
                u32 written = 1;
                out_pad(&o, &written, width, ' ');
            }
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            usize n, i;
            if (!s) {
                s = "(null)";
            }
            n = strlen(s);
            if (!left_align) {
                u32 written = 0;
                out_pad(&o, &written, width > n ? (u32)(width - n) : 0, ' ');
            }
            for (i = 0; i < n; i++) {
                out_ch(&o, s[i]);
            }
            if (left_align) {
                u32 written = (u32)n;
                out_pad(&o, &written, width, ' ');
            }
            break;
        }
        case 'd':
        case 'i': {
            i64 sv;
            u64 uv;
            char sign = 0;
            if (len == LEN_LONGLONG) {
                sv = (i64)va_arg(ap, long long);
            } else if (len == LEN_LONG) {
                sv = (i64)va_arg(ap, long);
            } else if (len == LEN_SIZE) {
                sv = (i64)va_arg(ap, isize);
            } else {
                sv = (i64)va_arg(ap, int);
            }
            if (sv < 0) {
                sign = '-';
                uv = (u64)(-(sv + 1)) + 1;
            } else {
                uv = (u64)sv;
                if (force_sign) {
                    sign = '+';
                } else if (space_sign) {
                    sign = ' ';
                }
            }
            out_unsigned(&o, uv, 10, 0, width, zero_pad, left_align, sign);
            break;
        }
        case 'u':
        case 'x':
        case 'X': {
            u64 uv;
            if (len == LEN_LONGLONG) {
                uv = va_arg(ap, unsigned long long);
            } else if (len == LEN_LONG) {
                uv = va_arg(ap, unsigned long);
            } else if (len == LEN_SIZE) {
                uv = va_arg(ap, usize);
            } else {
                uv = va_arg(ap, unsigned int);
            }
            out_unsigned(&o, uv, conv == 'u' ? 10 : 16, conv == 'X', width,
                         zero_pad, left_align, 0);
            break;
        }
        case 'p': {
            u64 uv = (u64)(uintptr)va_arg(ap, void *);
            out_ch(&o, '0');
            out_ch(&o, 'x');
            out_unsigned(&o, uv, 16, 0, 16, 1, 0, 0);
            break;
        }
        default:
            out_ch(&o, '%');
            out_ch(&o, conv);
            break;
        }
    }

    if (o.cap) {
        usize end = o.len < o.cap ? o.len : o.cap - 1;
        o.buf[end] = '\0';
    }
    return (int)o.len;
}

int ksnprintf(char *buf, usize size, const char *fmt, ...)
{
    va_list ap;
    int rc;

    va_start(ap, fmt);
    rc = kvsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return rc;
}
