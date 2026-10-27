/* BLAKE2b (RFC 7693), unkeyed and keyed, variable output length. */
#include <crug/blake2b.h>
#include <crug/string.h>

static const u64 blake2b_iv[8] = {
    0x6a09e667f3bcc908ull, 0xbb67ae8584caa73bull, 0x3c6ef372fe94f82bull,
    0xa54ff53a5f1d36f1ull, 0x510e527fade682d1ull, 0x9b05688c2b3e6c1full,
    0x1f83d9abfb41bd6bull, 0x5be0cd19137e2179ull,
};

static const u8 blake2b_sigma[10][16] = {
    { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 },
    { 14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3 },
    { 11, 8, 12, 0, 5, 2, 15, 13, 10, 14, 3, 6, 7, 1, 9, 4 },
    { 7, 9, 3, 1, 13, 12, 11, 14, 2, 6, 5, 10, 4, 0, 15, 8 },
    { 9, 0, 5, 7, 2, 4, 10, 15, 14, 1, 11, 12, 6, 8, 3, 13 },
    { 2, 12, 6, 10, 0, 11, 8, 3, 4, 13, 7, 5, 15, 14, 1, 9 },
    { 12, 5, 1, 15, 14, 13, 4, 10, 0, 7, 6, 3, 9, 2, 8, 11 },
    { 13, 11, 7, 14, 12, 1, 3, 9, 5, 0, 15, 4, 8, 6, 2, 10 },
    { 6, 15, 14, 9, 11, 3, 0, 8, 12, 2, 13, 7, 1, 4, 10, 5 },
    { 10, 2, 8, 4, 7, 6, 1, 5, 15, 11, 9, 14, 3, 12, 13, 0 },
};

static u64 load_le64(const u8 *p)
{
    return (u64)p[0] | ((u64)p[1] << 8) | ((u64)p[2] << 16) | ((u64)p[3] << 24) |
           ((u64)p[4] << 32) | ((u64)p[5] << 40) | ((u64)p[6] << 48) |
           ((u64)p[7] << 56);
}

static void store_le64(u8 *p, u64 v)
{
    usize i;

    for (i = 0; i < 8; i++) {
        p[i] = (u8)(v >> (8 * i));
    }
}

#define ROTR64(x, n) (((x) >> (n)) | ((x) << (64 - (n))))

static void blake2b_compress(struct blake2b_ctx *ctx, const u8 block[BLAKE2B_BLOCK_SIZE],
                             int last)
{
    u64 m[16];
    u64 v[16];
    usize i;
    u32 r;

#define G(a, b, c, d, x, y)                                      \
    do {                                                         \
        v[a] = v[a] + v[b] + (x);                                \
        v[d] = ROTR64(v[d] ^ v[a], 32);                          \
        v[c] = v[c] + v[d];                                      \
        v[b] = ROTR64(v[b] ^ v[c], 24);                          \
        v[a] = v[a] + v[b] + (y);                                \
        v[d] = ROTR64(v[d] ^ v[a], 16);                          \
        v[c] = v[c] + v[d];                                      \
        v[b] = ROTR64(v[b] ^ v[c], 63);                          \
    } while (0)

    for (i = 0; i < 16; i++) {
        m[i] = load_le64(block + 8 * i);
    }
    for (i = 0; i < 8; i++) {
        v[i] = ctx->h[i];
        v[8 + i] = blake2b_iv[i];
    }
    v[12] ^= ctx->t[0];
    v[13] ^= ctx->t[1];
    if (last) {
        v[14] = ~v[14];
    }
    for (r = 0; r < 12; r++) {
        const u8 *s = blake2b_sigma[r % 10];

        G(0, 4, 8, 12, m[s[0]], m[s[1]]);
        G(1, 5, 9, 13, m[s[2]], m[s[3]]);
        G(2, 6, 10, 14, m[s[4]], m[s[5]]);
        G(3, 7, 11, 15, m[s[6]], m[s[7]]);
        G(0, 5, 10, 15, m[s[8]], m[s[9]]);
        G(1, 6, 11, 12, m[s[10]], m[s[11]]);
        G(2, 7, 8, 13, m[s[12]], m[s[13]]);
        G(3, 4, 9, 14, m[s[14]], m[s[15]]);
    }
    for (i = 0; i < 8; i++) {
        ctx->h[i] ^= v[i] ^ v[8 + i];
    }

#undef G
}

static void blake2b_init_salt(struct blake2b_ctx *ctx, usize outlen, usize keylen)
{
    usize i;

    memset(ctx, 0, sizeof(*ctx));
    ctx->outlen = outlen;
    for (i = 0; i < 8; i++) {
        ctx->h[i] = blake2b_iv[i];
    }
    ctx->h[0] ^= 0x01010000ull ^ ((u64)keylen << 8) ^ (u64)outlen;
}

void blake2b_init(struct blake2b_ctx *ctx, usize outlen)
{
    blake2b_init_salt(ctx, outlen, 0);
}

void blake2b_init_key(struct blake2b_ctx *ctx, usize outlen, const void *key, usize keylen)
{
    u8 block[BLAKE2B_BLOCK_SIZE];

    blake2b_init_salt(ctx, outlen, keylen);
    if (keylen == 0) {
        return;
    }
    memset(block, 0, sizeof(block));
    memcpy(block, key, keylen);
    blake2b_update(ctx, block, sizeof(block));
    memset(block, 0, sizeof(block));
}

void blake2b_update(struct blake2b_ctx *ctx, const void *data, usize len)
{
    const u8 *p = (const u8 *)data;
    usize left;
    usize fill;

    if (len == 0) {
        return;
    }
    left = ctx->buflen;
    fill = BLAKE2B_BLOCK_SIZE - left;
    if (len > fill) {
        memcpy(ctx->buf + left, p, fill);
        ctx->t[0] += BLAKE2B_BLOCK_SIZE;
        if (ctx->t[0] < BLAKE2B_BLOCK_SIZE) {
            ctx->t[1]++;
        }
        ctx->buflen = 0;
        blake2b_compress(ctx, ctx->buf, 0);
        p += fill;
        len -= fill;
        while (len > BLAKE2B_BLOCK_SIZE) {
            ctx->t[0] += BLAKE2B_BLOCK_SIZE;
            if (ctx->t[0] < BLAKE2B_BLOCK_SIZE) {
                ctx->t[1]++;
            }
            blake2b_compress(ctx, p, 0);
            p += BLAKE2B_BLOCK_SIZE;
            len -= BLAKE2B_BLOCK_SIZE;
        }
        left = 0;
    }
    memcpy(ctx->buf + left, p, len);
    ctx->buflen = left + len;
}

void blake2b_final(struct blake2b_ctx *ctx, void *out)
{
    u8 full[BLAKE2B_MAX_OUT];
    u8 *dst = (u8 *)out;
    usize i;

    ctx->t[0] += ctx->buflen;
    if (ctx->t[0] < ctx->buflen) {
        ctx->t[1]++;
    }
    memset(ctx->buf + ctx->buflen, 0, BLAKE2B_BLOCK_SIZE - ctx->buflen);
    blake2b_compress(ctx, ctx->buf, 1);
    for (i = 0; i < 8; i++) {
        store_le64(full + 8 * i, ctx->h[i]);
    }
    memcpy(dst, full, ctx->outlen);
    memset(full, 0, sizeof(full));
    memset(ctx, 0, sizeof(*ctx));
}

void blake2b_hash(void *out, usize outlen, const void *key, usize keylen, const void *data,
                  usize len)
{
    struct blake2b_ctx ctx;

    if (keylen) {
        blake2b_init_key(&ctx, outlen, key, keylen);
    } else {
        blake2b_init(&ctx, outlen);
    }
    blake2b_update(&ctx, data, len);
    blake2b_final(&ctx, out);
}
