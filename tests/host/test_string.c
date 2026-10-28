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

    {
        u8 secret[8];
        u8 i;

        /* secure_zero must overwrite exactly the requested bytes and must
         * leave the rest of the buffer alone, including a zero length. */
        for (i = 0; i < sizeof(secret); i++) {
            secret[i] = 0xAA;
        }
        secure_zero(secret, 3);
        CHECK(secret[0] == 0);
        CHECK(secret[1] == 0);
        CHECK(secret[2] == 0);
        CHECK(secret[3] == 0xAA);
        secure_zero(secret, 0);
        CHECK(secret[3] == 0xAA);
        secure_zero(secret, sizeof(secret));
        for (i = 0; i < sizeof(secret); i++) {
            CHECK(secret[i] == 0);
        }
    }
}
