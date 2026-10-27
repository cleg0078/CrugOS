/* AES-256-XTS (IEEE 1619 / NIST SP 800-38E) over 16-byte blocks.
 *
 * XEX construction: T = AES(tweak_key, data_unit), then per block
 * C = AES(data_key, P xor T) xor T, with T multiplied by alpha in GF(2^128)
 * (little-endian, reduction polynomial 0x87) after every block. */
#include <crug/xts.h>
#include <crug/string.h>

void aes256_xts_set_key(struct aes256_xts_key *key, const u8 raw[AES_XTS_KEY_SIZE])
{
    aes256_expand_key(key->data_rk, raw);
    aes256_expand_key(key->tweak_rk, raw + AES256_KEY_SIZE);
}

void aes256_xts_wipe(struct aes256_xts_key *key)
{
    memset(key, 0, sizeof(*key));
}

void aes256_xts_sector_tweak(u8 tweak[AES_BLOCK_SIZE], u64 index)
{
    usize i;

    memset(tweak, 0, AES_BLOCK_SIZE);
    for (i = 0; i < 8; i++) {
        tweak[i] = (u8)(index >> (8 * i));
    }
}

static void xts_mul_alpha(u8 t[AES_BLOCK_SIZE])
{
    u8 carry = (u8)(t[15] & 0x80);
    int i;

    for (i = 15; i > 0; i--) {
        t[i] = (u8)((t[i] << 1) | (t[i - 1] >> 7));
    }
    t[0] = (u8)(t[0] << 1);
    if (carry) {
        t[0] ^= 0x87;
    }
}

static void xts_xor(u8 *buf, const u8 *t)
{
    usize i;

    for (i = 0; i < AES_BLOCK_SIZE; i++) {
        buf[i] ^= t[i];
    }
}

static int xts_validate(const struct aes256_xts_key *key, const u8 *tweak, const void *buf,
                        usize len)
{
    if (!key || !tweak || !buf || len == 0 || (len & (AES_BLOCK_SIZE - 1)) != 0) {
        return -1;
    }
    return 0;
}

int aes256_xts_encrypt(const struct aes256_xts_key *key, const u8 tweak[AES_BLOCK_SIZE],
                       void *buf, usize len)
{
    u8 t[AES_BLOCK_SIZE];
    u8 *p = (u8 *)buf;
    usize off;

    if (xts_validate(key, tweak, buf, len) != 0) {
        return -1;
    }
    aes256_encrypt_block(key->tweak_rk, tweak, t);
    for (off = 0; off < len; off += AES_BLOCK_SIZE) {
        xts_xor(p + off, t);
        aes256_encrypt_block(key->data_rk, p + off, p + off);
        xts_xor(p + off, t);
        xts_mul_alpha(t);
    }
    memset(t, 0, sizeof(t));
    return 0;
}

int aes256_xts_decrypt(const struct aes256_xts_key *key, const u8 tweak[AES_BLOCK_SIZE],
                       void *buf, usize len)
{
    u8 t[AES_BLOCK_SIZE];
    u8 *p = (u8 *)buf;
    usize off;

    if (xts_validate(key, tweak, buf, len) != 0) {
        return -1;
    }
    aes256_encrypt_block(key->tweak_rk, tweak, t);
    for (off = 0; off < len; off += AES_BLOCK_SIZE) {
        xts_xor(p + off, t);
        aes256_decrypt_block(key->data_rk, p + off, p + off);
        xts_xor(p + off, t);
        xts_mul_alpha(t);
    }
    memset(t, 0, sizeof(t));
    return 0;
}
