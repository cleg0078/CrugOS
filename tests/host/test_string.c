#include <crug/string.h>
#include <crug/types.h>
#include "test.h"

void test_string_functions(void)
{
    char buffer[16];
    char overlap[8] = "abcdef";

    memset(buffer, 'x', sizeof(buffer));
    CHECK(buffer[0] == 'x');
    CHECK(buffer[15] == 'x');
    memset(buffer, 0, sizeof(buffer));
    CHECK(buffer[0] == 0);

    memcpy(buffer, "hello", 6);
    CHECK(memcmp(buffer, "hello", 6) == 0);
    CHECK(strlen(buffer) == 5);

    memmove(overlap + 1, overlap, 5);
    CHECK(memcmp(overlap, "aabcde", 6) == 0);

    CHECK(strcmp("abc", "abc") == 0);
    CHECK(strcmp("abc", "abd") < 0);
    CHECK(strcmp("abd", "abc") > 0);
    CHECK(strncmp("abcdef", "abcxyz", 3) == 0);
    CHECK(strncmp("abcdef", "abcxyz", 4) != 0);

    CHECK(memcmp("abc", "abc", 3) == 0);
    CHECK(memcmp("abc", "abd", 3) < 0);
    CHECK(memcmp("abd", "abc", 3) > 0);
}
