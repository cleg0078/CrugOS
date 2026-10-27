#include <stdarg.h>
#include <crug/kernel.h>
#include <crug/string.h>
#include <crug/types.h>
#include "test.h"

static const char *call(const char *format, ...)
{
    static char buffer[256];
    va_list args;

    va_start(args, format);
    kvsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    return buffer;
}

void test_printf_functions(void)
{
    char buffer[64];
    int written;

    written = ksnprintf(buffer, sizeof(buffer), "%s %d", "boot", 42);
    CHECK_STR(buffer, "boot 42");
    CHECK(written == 7);

    CHECK_STR(call("%x", 0xdeadbeefu), "deadbeef");
    CHECK_STR(call("%X", 0xdeadbeefu), "DEADBEEF");
    CHECK_STR(call("%u", 4000000000u), "4000000000");
    CHECK_STR(call("%d", -1234), "-1234");
    CHECK_STR(call("%d", 0), "0");
    CHECK_STR(call("%p", (void *)0x1000), "0x0000000000001000");
    CHECK_STR(call("%%"), "%");
    CHECK_STR(call("%-5d|", 42), "42   |");
    CHECK_STR(call("%5d|", 42), "   42|");
    CHECK_STR(call("%05d", 42), "00042");
    CHECK_STR(call("%llu", 1234567890123ull), "1234567890123");

    written = ksnprintf(buffer, 5, "abcdef");
    CHECK_STR(buffer, "abcd");
    CHECK(written == 6);

    written = ksnprintf(buffer, 1, "abcdef");
    CHECK(buffer[0] == 0);
    CHECK(written == 6);

    memset(buffer, 'z', sizeof(buffer));
    written = ksnprintf(buffer, 0, "abc");
    CHECK(written == 3);
    CHECK(buffer[0] == 'z');
}
