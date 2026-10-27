/* Host tests for BLAKE2b (RFC 7693). */
#include "test.h"
#include <crug/blake2b.h>
#include <crug/string.h>

static usize hex_to_bytes(const char *hex, u8 *out, usize cap)
{
    usize n = 0;

    while (hex[0] && hex[1] && n < cap) {
        u8 v = 0;
        int i;

        for (i = 0; i < 2; i++) {
            char c = hex[i];
            u8 nib = (c >= '0' && c <= '9') ? (u8)(c - '0')
                                         : (u8)((c | 0x20) - 'a' + 10);
            v = (u8)((v << 4) | nib);
        }
        out[n++] = v;
        hex += 2;
    }
    return n;
}

static void check_hex(const char *what, usize outlen, const void *key, usize keylen,
                      const void *data, usize len, const char *hex)
{
    u8 want[64];
    u8 got[64];
    usize want_len = hex_to_bytes(hex, want, sizeof(want));

    (void)what;
    CHECK(want_len == outlen);
    blake2b_hash(got, outlen, key, keylen, data, len);
    CHECK(memcmp(got, want, outlen) == 0);
}

void test_blake2b_functions(void)
{
    struct blake2b_ctx ctx;
    u8 split[64];
    u8 whole[64];
    u8 keyed[64];
    u8 pat[1000];
    u8 data[777];
    usize i;

    check_hex("abc-512", 64, 0, 0, "abc", 3,
              "ba80a53f981c4d0d6a2797b69f12f6e94c212f14685ac4b74b12bb6fdbffa2d1"
              "7d87c5392aab792dc252d5de4533cc9518d38aa8dbf1925ab92386edd4009923");
    check_hex("empty-512", 64, 0, 0, "", 0,
              "786a02f742015903c6c6fd852552d272912f4740e15847618a86e217f71f5419"
              "d25e1031afee585313896444934eb04b903a685b1448b755d56f701afe9be2ce");
    check_hex("keyed-256", 32, "key", 3, "data", 4,
              "0b05c30009bdbad5c1a1e7013a2e85ea700731cf9dda9d45eb12ea6ac755485a");

    for (i = 0; i < sizeof(pat); i++) {
        pat[i] = (u8)((i * 7 + 3) & 0xFF);
    }
    check_hex("pattern-256", 32, 0, 0, pat, sizeof(pat),
              "d62b6c768ce1afc8367e0498ab2f8e3f7c178c35b1429f14c4604b545d200f52");

    /* Streaming in odd-sized chunks matches the one-shot hash. */
    for (i = 0; i < sizeof(data); i++) {
        data[i] = (u8)(i * 31 + 7);
    }
    blake2b_hash(whole, sizeof(whole), 0, 0, data, sizeof(data));
    blake2b_init(&ctx, sizeof(split));
    for (i = 0; i < sizeof(data);) {
        usize step = (i % 37) + 1;

        if (i + step > sizeof(data)) {
            step = sizeof(data) - i;
        }
        blake2b_update(&ctx, data + i, step);
        i += step;
    }
    blake2b_final(&ctx, split);
    CHECK(memcmp(whole, split, sizeof(whole)) == 0);

    /* Keyed and unkeyed hashes of the same input differ. */
    blake2b_hash(whole, sizeof(whole), 0, 0, "abc", 3);
    blake2b_hash(keyed, sizeof(keyed), "k", 1, "abc", 3);
    CHECK(memcmp(whole, keyed, 16) != 0);
}
