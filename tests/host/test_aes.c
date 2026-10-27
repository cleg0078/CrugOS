/* Host tests for AES-256 (FIPS-197) and AES-256-XTS (IEEE 1619 / SP 800-38E). */
#include "test.h"
#include <crug/aes.h>
#include <crug/blake2b.h>
#include <crug/string.h>
#include <crug/xts.h>

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

static void check_block(const u8 rk[AES256_ROUND_KEYS], const u8 in[16], const char *hex,
                        const char *what)
{
    u8 want[16];
    u8 got[16];
    u8 back[16];

    (void)what;
    CHECK(hex_to_bytes(hex, want, sizeof(want)) == 16);
    aes256_encrypt_block_soft(rk, in, got);
    CHECK(memcmp(got, want, 16) == 0);
    aes256_decrypt_block_soft(rk, want, back);
    CHECK(memcmp(back, in, 16) == 0);
    aes256_encrypt_block(rk, in, got);
    CHECK(memcmp(got, want, 16) == 0);
    aes256_decrypt_block(rk, want, back);
    CHECK(memcmp(back, in, 16) == 0);
#if defined(__x86_64__)
    if (aes_hardware_available()) {
        aesni_encrypt_block(rk, in, got);
        CHECK(memcmp(got, want, 16) == 0);
        aesni_decrypt_block(rk, want, back);
        CHECK(memcmp(back, in, 16) == 0);
    }
#endif
}

static void xts_vectors(void)
{
    static const char cipher_hex[] =
              "dc0cc5f6b94de1791a2a0b2c6ece7cd55b1fb0e89675ba4442be5516e8bfb9bf"
              "aa16b7a1e631deebae12ef1722767c840e9a07c7114dde3fe6a1c11b2d8a7c51"
              "8687f6c1e521a8e73182dd9522f8025170fdae43da7b54e082a7435495216cad"
              "cb7de456940f6b6c9e0dfab4edda7f52bd2cdd86a73e580b2c55f8e6276f5811"
              "d4cf0a07ffde3bca2c71b50a2572098a91b678a877027c53f92680931beb21d0"
              "da49bb8f874226ca40ba8574cafcbd50496407b227546d67178b93a939858c6a"
              "75908324c4f177f4c088c296a02506f34cea8331da451b17ce7b3911352c5a1f"
              "66875e9b1afe35042add255654170eef9ebd99a1b69c8ad4c8055b5c52f92239"
              "21c6a43bede87d6ef94a82c00e5d50fcb65b2b954c1cd11bf9bd017d1a8263a5"
              "4b9ee65ef700e8cc86e6bd0f2fba321fe1737e1fc518c01563ec8efbe306a61a"
              "1bbd80d9da585c61a5c1d06bf11e53c7d24ecd6ed3c9cf328bea6916a64dad49"
              "0475e5e4992926437797bddc1c8f4587a471f70d67fea0a03168fcc957e67d5a"
              "9ecc76d8ca574c8ecb68cb0ee2520542f25b85ed6e1ecb4028a06f6854227e76"
              "a2c63f4dc08f814aa77e9bfc62b23afdab06701a9968fc440611703a2f71cc17"
              "72f8003e08e3f651729e40d5ca5e04dfdd60e804cb7589d6cda55f45f2a54b2c"
              "63f4bf2662bf403156ddd0060179e8b7c5137839b838a4489815ef0a99a0b828";
    u8 raw[64];
    u8 buf[512];
    u8 original[512];
    u8 want[512];
    u8 tweak[16];
    struct aes256_xts_key key;
    usize i;

    for (i = 0; i < sizeof(raw); i++) {
        raw[i] = (u8)i;
    }
    for (i = 0; i < sizeof(buf); i++) {
        buf[i] = (u8)((i * 13 + 7) & 0xFF);
    }
    memcpy(original, buf, sizeof(buf));
    CHECK(hex_to_bytes(cipher_hex, want, sizeof(want)) == sizeof(want));

    aes256_xts_set_key(&key, raw);
    aes256_xts_sector_tweak(tweak, 1);
    CHECK(aes256_xts_encrypt(&key, tweak, buf, sizeof(buf)) == 0);
    CHECK(memcmp(buf, want, sizeof(want)) == 0);
    CHECK(aes256_xts_decrypt(&key, tweak, buf, sizeof(buf)) == 0);
    CHECK(memcmp(buf, original, sizeof(buf)) == 0);

    /* A different tweak must produce different ciphertext. */
    aes256_xts_sector_tweak(tweak, 2);
    CHECK(aes256_xts_encrypt(&key, tweak, buf, sizeof(buf)) == 0);
    CHECK(memcmp(buf, want, sizeof(want)) != 0);
    CHECK(aes256_xts_decrypt(&key, tweak, buf, sizeof(buf)) == 0);
    CHECK(memcmp(buf, original, sizeof(buf)) == 0);

    /* Multi-block tweak progression, cross-checked against python cryptography. */
    {
        u8 big[1024];
        u8 copy[1024];
        u8 digest[32];
        u8 want_digest[32];

        for (i = 0; i < sizeof(big); i++) {
            big[i] = (u8)((i * 29 + 11) & 0xFF);
        }
        memcpy(copy, big, sizeof(big));
        aes256_xts_sector_tweak(tweak, 0x0102030405060708ull);
        CHECK(aes256_xts_encrypt(&key, tweak, big, sizeof(big)) == 0);
        blake2b_hash(digest, sizeof(digest), 0, 0, big, sizeof(big));
        CHECK(hex_to_bytes("4994847fc10706bf208dbb996c9af4eb4f234a29a2f62760eed8765230ff2af1", want_digest, sizeof(want_digest)) == 32);
        CHECK(memcmp(digest, want_digest, sizeof(digest)) == 0);
        CHECK(aes256_xts_decrypt(&key, tweak, big, sizeof(big)) == 0);
        CHECK(memcmp(big, copy, sizeof(big)) == 0);
    }

    /* IEEE 1619 / P1619-2007 Appendix B, vector 10 (XTS-AES-256): data unit
     * sequence number 0xFF, 512-byte data unit 00 01 .. ff 00 01 .. ff. */
    {
        u8 ieee_buf[512];
        u8 ieee_key[64];
        u8 ieee_back[512];
        u8 ieee_tweak[16];
        struct aes256_xts_key ieee_xts;

        CHECK(hex_to_bytes("2718281828459045235360287471352662497757247093699959574966967627"
                           "3141592653589793238462643383279502884197169399375105820974944592",
                           ieee_key, sizeof(ieee_key)) == sizeof(ieee_key));
        memset(ieee_tweak, 0, sizeof(ieee_tweak));
        ieee_tweak[0] = 0xFF;
        for (i = 0; i < sizeof(ieee_buf); i++) {
            ieee_buf[i] = (u8)i;
        }
        memcpy(ieee_back, ieee_buf, sizeof(ieee_back));
        aes256_xts_set_key(&ieee_xts, ieee_key);
        CHECK(aes256_xts_encrypt(&ieee_xts, ieee_tweak, ieee_buf, sizeof(ieee_buf)) == 0);
        CHECK(hex_to_bytes("1c3b3a102f770386e4836c99e370cf9b", want, 16) == 16);
        CHECK(memcmp(ieee_buf, want, 16) == 0);
        CHECK(hex_to_bytes("ea00803f5e482357a4ae12d414a3e63b", want, 16) == 16);
        CHECK(memcmp(ieee_buf + 16, want, 16) == 0);
        CHECK(aes256_xts_decrypt(&ieee_xts, ieee_tweak, ieee_buf, sizeof(ieee_buf)) == 0);
        CHECK(memcmp(ieee_buf, ieee_back, sizeof(ieee_buf)) == 0);
        aes256_xts_wipe(&ieee_xts);
    }

    /* Non-block-multiple lengths and empty input are rejected. */
    CHECK(aes256_xts_encrypt(&key, tweak, buf, 15) == -1);
    CHECK(aes256_xts_encrypt(&key, tweak, buf, 0) == -1);
    aes256_xts_wipe(&key);
}

void test_aes_functions(void)
{
    u8 key[32];
    u8 rk[AES256_ROUND_KEYS];
    u8 pt[16];
    usize i;

    aes_init();
    for (i = 0; i < sizeof(key); i++) {
        key[i] = (u8)i;
    }
    CHECK(hex_to_bytes("00112233445566778899aabbccddeeff", pt, sizeof(pt)) == 16);
    aes256_expand_key(rk, key);
    check_block(rk, pt, "8ea2b7ca516745bfeafc49904b496089", "fips-197-c3");

    /* NIST SP 800-38A F.1.5 ECB-AES256, first two blocks. */
    CHECK(hex_to_bytes("603deb1015ca71be2b73aef0857d77811f352c073b6108d72d9810a30914dff4",
                       key, sizeof(key)) == 32);
    aes256_expand_key(rk, key);
    CHECK(hex_to_bytes("6bc1bee22e409f96e93d7e117393172a", pt, sizeof(pt)) == 16);
    check_block(rk, pt, "f3eed1bdb5d2a03c064b5a7e3db181f8", "sp800-38a-b1");
    CHECK(hex_to_bytes("ae2d8a571e03ac9c9eb76fac45af8e51", pt, sizeof(pt)) == 16);
    check_block(rk, pt, "591ccb10d410ed26dc5ba74a31362870", "sp800-38a-b2");

    xts_vectors();
}
