/* Freestanding memory and string routines. */
#include <crug/types.h>
#include <crug/string.h>

void *memcpy(void *dst, const void *src, usize n)
{
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    while (n--) {
        *d++ = *s++;
    }
    return dst;
}

void *memmove(void *dst, const void *src, usize n)
{
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;

    if (d == s || n == 0) {
        return dst;
    }
    if (d < s) {
        while (n--) {
            *d++ = *s++;
        }
    } else {
        d += n;
        s += n;
        while (n--) {
            *--d = *--s;
        }
    }
    return dst;
}

/* The compiler must not be able to drop this store: a plain memset over a
   dead buffer is legal to elide, a volatile store is not. */
void secure_zero(void *dst, usize n)
{
    volatile u8 *d = (volatile u8 *)dst;

    while (n-- > 0)
        *d++ = 0;
}

void *memset(void *dst, int value, usize n)
{
    u8 *d = (u8 *)dst;
    while (n--) {
        *d++ = (u8)value;
    }
    return dst;
}

int memcmp(const void *a, const void *b, usize n)
{
    const u8 *pa = (const u8 *)a;
    const u8 *pb = (const u8 *)b;

    while (n--) {
        if (*pa != *pb) {
            return (int)*pa - (int)*pb;
        }
        pa++;
        pb++;
    }
    return 0;
}

usize strlen(const char *s)
{
    usize n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncmp(const char *a, const char *b, usize n)
{
    while (n && *a && *a == *b) {
        a++;
        b++;
        n--;
    }
    if (n == 0) {
        return 0;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

char *strncpy(char *dst, const char *src, usize n)
{
    char *d = dst;

    while (n && *src) {
        *d++ = *src++;
        n--;
    }
    while (n--) {
        *d++ = 0;
    }
    return dst;
}

char *strstr(const char *haystack, const char *needle)
{
    usize n;

    if (!*needle)
        return (char *)haystack;
    n = strlen(needle);
    while (*haystack) {
        if (*haystack == *needle && strncmp(haystack, needle, n) == 0)
            return (char *)haystack;
        haystack++;
    }
    return 0;
}
