/* AES-256 (FIPS-197) with a software core and an AES-NI core. */
#include <crug/aes.h>
#include <crug/string.h>

static u8 aes_sbox[256];
static u8 aes_inv_sbox[256];
static int aes_tables_ready;
static int aes_hw_present;

#define AES_ROUNDS_256 14u
#define AES_WORDS_256 60u

static const u8 aes_rcon[11] = { 0x00, 0x01, 0x02, 0x04, 0x08, 0x10,
                                 0x20, 0x40, 0x80, 0x1b, 0x36 };

static u8 aes_gf_mul(u8 a, u8 b)
{
    u8 p = 0;
    int i;

    for (i = 0; i < 8; i++) {
        if (b & 1) {
            p ^= a;
        }
        if (a & 0x80) {
            a = (u8)((a << 1) ^ 0x1b);
        } else {
            a = (u8)(a << 1);
        }
        b >>= 1;
    }
    return p;
}

static u8 aes_rotl8(u8 x, u8 n)
{
    return (u8)((x << n) | (x >> (8 - n)));
}

static void aes_build_tables(void)
{
    int i;

    aes_sbox[0] = 0x63;
    aes_inv_sbox[0x63] = 0;
    for (i = 1; i < 256; i++) {
        u8 inv = 0;
        int j;
        u8 v;

        for (j = 1; j < 256; j++) {
            if (aes_gf_mul((u8)i, (u8)j) == 1) {
                inv = (u8)j;
                break;
            }
        }
        v = (u8)(inv ^ aes_rotl8(inv, 1) ^ aes_rotl8(inv, 2) ^ aes_rotl8(inv, 3) ^
                 aes_rotl8(inv, 4) ^ 0x63);
        aes_sbox[i] = v;
        aes_inv_sbox[v] = (u8)i;
    }
}

static void aes_cpuid(u32 leaf, u32 sub, u32 *a, u32 *b, u32 *c, u32 *d)
{
    __asm__ volatile("cpuid"
                     : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                     : "a"(leaf), "c"(sub)
                     : "memory");
}

void aes_init(void)
{
    u32 a = 0, b = 0, c = 0, d = 0;

    if (aes_tables_ready) {
        return;
    }
    aes_build_tables();
    aes_cpuid(1, 0, &a, &b, &c, &d);
    aes_hw_present = (c & (1u << 25)) ? 1 : 0;
    aes_tables_ready = 1;
}

int aes_hardware_available(void)
{
    aes_init();
    return aes_hw_present;
}

const char *aes_implementation_name(void)
{
    return aes_hardware_available() ? "aes-ni" : "software";
}

static u32 aes_sub_word(u32 w)
{
    return ((u32)aes_sbox[(w >> 24) & 0xFF] << 24) | ((u32)aes_sbox[(w >> 16) & 0xFF] << 16) |
           ((u32)aes_sbox[(w >> 8) & 0xFF] << 8) | (u32)aes_sbox[w & 0xFF];
}

static u32 aes_rot_word(u32 w)
{
    return (w << 8) | (w >> 24);
}

static u32 aes_load_be32(const u8 *p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | (u32)p[3];
}

static void aes_store_be32(u8 *p, u32 v)
{
    p[0] = (u8)(v >> 24);
    p[1] = (u8)(v >> 16);
    p[2] = (u8)(v >> 8);
    p[3] = (u8)v;
}

void aes256_expand_key(u8 rk[AES256_ROUND_KEYS], const u8 key[AES256_KEY_SIZE])
{
    u32 w[AES_WORDS_256];
    u32 i;

    aes_init();
    for (i = 0; i < 8; i++) {
        w[i] = aes_load_be32(key + 4 * i);
    }
    for (i = 8; i < AES_WORDS_256; i++) {
        u32 t = w[i - 1];

        if (i % 8 == 0) {
            t = aes_sub_word(aes_rot_word(t)) ^ ((u32)aes_rcon[i / 8] << 24);
        } else if (i % 8 == 4) {
            t = aes_sub_word(t);
        }
        w[i] = w[i - 8] ^ t;
    }
    for (i = 0; i < AES_WORDS_256; i++) {
        aes_store_be32(rk + 4 * i, w[i]);
    }
    memset(w, 0, sizeof(w));
}

static void aes_add_round_key(u8 s[AES_BLOCK_SIZE], const u8 *rk)
{
    usize i;

    for (i = 0; i < AES_BLOCK_SIZE; i++) {
        s[i] ^= rk[i];
    }
}

static void aes_sub_bytes(u8 s[AES_BLOCK_SIZE])
{
    usize i;

    for (i = 0; i < AES_BLOCK_SIZE; i++) {
        s[i] = aes_sbox[s[i]];
    }
}

static void aes_inv_sub_bytes(u8 s[AES_BLOCK_SIZE])
{
    usize i;

    for (i = 0; i < AES_BLOCK_SIZE; i++) {
        s[i] = aes_inv_sbox[s[i]];
    }
}

static void aes_shift_rows(u8 s[AES_BLOCK_SIZE])
{
    u8 t[AES_BLOCK_SIZE];
    usize r, c;

    for (r = 0; r < 4; r++) {
        for (c = 0; c < 4; c++) {
            t[r + 4 * c] = s[r + 4 * ((c + r) & 3)];
        }
    }
    memcpy(s, t, sizeof(t));
}

static void aes_inv_shift_rows(u8 s[AES_BLOCK_SIZE])
{
    u8 t[AES_BLOCK_SIZE];
    usize r, c;

    for (r = 0; r < 4; r++) {
        for (c = 0; c < 4; c++) {
            t[r + 4 * c] = s[r + 4 * ((c + 4 - r) & 3)];
        }
    }
    memcpy(s, t, sizeof(t));
}

static u8 aes_xtime(u8 x)
{
    return (u8)((x << 1) ^ ((x & 0x80) ? 0x1b : 0x00));
}

static void aes_mix_columns(u8 s[AES_BLOCK_SIZE])
{
    usize c;

    for (c = 0; c < 4; c++) {
        u8 *a = s + 4 * c;
        u8 a0 = a[0], a1 = a[1], a2 = a[2], a3 = a[3];
        u8 all = (u8)(a0 ^ a1 ^ a2 ^ a3);

        a[0] = (u8)(a0 ^ all ^ aes_xtime((u8)(a0 ^ a1)));
        a[1] = (u8)(a1 ^ all ^ aes_xtime((u8)(a1 ^ a2)));
        a[2] = (u8)(a2 ^ all ^ aes_xtime((u8)(a2 ^ a3)));
        a[3] = (u8)(a3 ^ all ^ aes_xtime((u8)(a3 ^ a0)));
    }
}

static void aes_inv_mix_columns(u8 s[AES_BLOCK_SIZE])
{
    usize c;

    for (c = 0; c < 4; c++) {
        u8 *a = s + 4 * c;
        u8 a0 = a[0], a1 = a[1], a2 = a[2], a3 = a[3];
        u8 t = aes_xtime(aes_xtime((u8)(a0 ^ a2)));
        u8 u = aes_xtime(aes_xtime((u8)(a1 ^ a3)));

        a[0] = (u8)(a0 ^ t);
        a[1] = (u8)(a1 ^ u);
        a[2] = (u8)(a2 ^ t);
        a[3] = (u8)(a3 ^ u);
    }
    aes_mix_columns(s);
}

void aes256_encrypt_block_soft(const u8 rk[AES256_ROUND_KEYS], const u8 in[AES_BLOCK_SIZE],
                               u8 out[AES_BLOCK_SIZE])
{
    u8 s[AES_BLOCK_SIZE];
    u32 round;

    aes_init();
    memcpy(s, in, sizeof(s));
    aes_add_round_key(s, rk);
    for (round = 1; round < AES_ROUNDS_256; round++) {
        aes_sub_bytes(s);
        aes_shift_rows(s);
        aes_mix_columns(s);
        aes_add_round_key(s, rk + round * AES_BLOCK_SIZE);
    }
    aes_sub_bytes(s);
    aes_shift_rows(s);
    aes_add_round_key(s, rk + AES_ROUNDS_256 * AES_BLOCK_SIZE);
    memcpy(out, s, sizeof(s));
}

void aes256_decrypt_block_soft(const u8 rk[AES256_ROUND_KEYS], const u8 in[AES_BLOCK_SIZE],
                               u8 out[AES_BLOCK_SIZE])
{
    u8 s[AES_BLOCK_SIZE];
    u32 round;

    aes_init();
    memcpy(s, in, sizeof(s));
    aes_add_round_key(s, rk + AES_ROUNDS_256 * AES_BLOCK_SIZE);
    for (round = AES_ROUNDS_256; round > 1; round--) {
        aes_inv_shift_rows(s);
        aes_inv_sub_bytes(s);
        aes_add_round_key(s, rk + (round - 1) * AES_BLOCK_SIZE);
        aes_inv_mix_columns(s);
    }
    aes_inv_shift_rows(s);
    aes_inv_sub_bytes(s);
    aes_add_round_key(s, rk);
    memcpy(out, s, sizeof(s));
}

void aes256_encrypt_block(const u8 rk[AES256_ROUND_KEYS], const u8 in[AES_BLOCK_SIZE],
                          u8 out[AES_BLOCK_SIZE])
{
    aes_init();
#if defined(__x86_64__)
    if (aes_hw_present) {
        aesni_encrypt_block(rk, in, out);
        return;
    }
#endif
    aes256_encrypt_block_soft(rk, in, out);
}

void aes256_decrypt_block(const u8 rk[AES256_ROUND_KEYS], const u8 in[AES_BLOCK_SIZE],
                          u8 out[AES_BLOCK_SIZE])
{
    aes_init();
#if defined(__x86_64__)
    if (aes_hw_present) {
        aesni_decrypt_block(rk, in, out);
        return;
    }
#endif
    aes256_decrypt_block_soft(rk, in, out);
}
