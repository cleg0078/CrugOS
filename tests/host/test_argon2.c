/* Host tests for Argon2id (RFC 9106), cross-checked against libargon2. */
#include "test.h"
#include <crug/argon2.h>
#include <crug/string.h>

static u8 workspace[128 * 1024];

static usize hex_to_bytes(const char *hex, u8 *out, usize cap)
{
    usize n = 0;

    while (hex[0] && hex[1] && n < cap) {
        u8 v = 0;
        int i;

        for (i = 0; i < 2; i++) {
            char c = hex[i];
            u8 nib = (c >= '0' && c <= '9') ? (u8)(c - '0') : (u8)((c | 0x20) - 'a' + 10);

            v = (u8)((v << 4) | nib);
        }
        out[n++] = v;
        hex += 2;
    }
    return n;
}

static void check_argon2(const char *what, const u8 *pwd, usize pwdlen, const u8 *salt,
                         usize saltlen, u32 m_cost, u32 t_cost, u32 lanes, const char *hex)
{
    u8 want[32];
    u8 got[32];

    (void)what;
    CHECK(hex_to_bytes(hex, want, sizeof(want)) == sizeof(want));
    memset(got, 0, sizeof(got));
    CHECK(argon2id_hash_mem(got, sizeof(got), pwd, pwdlen, salt, saltlen, m_cost, t_cost, lanes,
                            workspace, sizeof(workspace)) == 0);
    CHECK(memcmp(got, want, sizeof(want)) == 0);
}

void test_argon2_functions(void)
{
    u8 pwd[] = "crugos-test";
    u8 salt[16];
    u8 ones[32];
    u8 twos[16];
    usize i;

    for (i = 0; i < sizeof(salt); i++) {
        salt[i] = (u8)i;
    }
    memset(ones, 0x01, sizeof(ones));
    memset(twos, 0x02, sizeof(twos));

    /* Vectors generated with libargon2 (host reference implementation). */
    check_argon2("m32-t1-p1", pwd, sizeof(pwd) - 1, salt, sizeof(salt), 32, 1, 1,
                 "7279ce0338eb34da7778816ff287c58b7848c0477f5b3029aef77d76968041a2");
    check_argon2("m32-t3-p4", pwd, sizeof(pwd) - 1, salt, sizeof(salt), 32, 3, 4,
                 "e1186857faedd2cfb1d9dfff86f4fd3e9006ebff41d4e74ca0d2fd7ef95294d9");
    check_argon2("m32-t3-p4-bin", ones, sizeof(ones), twos, sizeof(twos), 32, 3, 4,
                 "03aab965c12001c9d7d0d2de33192c0494b684bb148196d73c1df1acaf6d0c2e");
    /* RFC 9106 section 5.3 style parameters (password/somesalt, m=64, t=2, p=1). */
    check_argon2("m64-t2-p1", (const u8 *)"password", 8, (const u8 *)"somesalt", 8, 64, 2, 1,
                 "16a1a498734609dd01456da406de9f3d9da93e6c86c300a12fc1465214ce4922");

    /* Rejects nonsense parameters. */
    CHECK(argon2id_hash_mem(workspace, 32, pwd, sizeof(pwd) - 1, salt, sizeof(salt), 32, 0, 1,
                            workspace, sizeof(workspace)) == -1);
    CHECK(argon2id_hash_mem(workspace, 32, pwd, sizeof(pwd) - 1, salt, sizeof(salt), 2, 1, 4,
                            workspace, sizeof(workspace)) == -1);
    CHECK(argon2id_hash_mem(workspace, 32, pwd, sizeof(pwd) - 1, salt, sizeof(salt), 32, 1, 1,
                            workspace, 1024) == -1);
}