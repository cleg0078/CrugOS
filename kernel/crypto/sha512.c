/* SHA-512 (FIPS 180-4), freestanding and allocation free. */
#include <crug/sha512.h>
#include <crug/string.h>

static const u64 sha512_k[80] = {
    0x428a2f98d728ae22ull, 0x7137449123ef65cdull, 0xb5c0fbcfec4d3b2full, 0xe9b5dba58189dbbcull,
    0x3956c25bf348b538ull, 0x59f111f1b605d019ull, 0x923f82a4af194f9bull, 0xab1c5ed5da6d8118ull,
    0xd807aa98a3030242ull, 0x12835b0145706fbeull, 0x243185be4ee4b28cull, 0x550c7dc3d5ffb4e2ull,
    0x72be5d74f27b896full, 0x80deb1fe3b1696b1ull, 0x9bdc06a725c71235ull, 0xc19bf174cf692694ull,
    0xe49b69c19ef14ad2ull, 0xefbe4786384f25e3ull, 0x0fc19dc68b8cd5b5ull, 0x240ca1cc77ac9c65ull,
    0x2de92c6f592b0275ull, 0x4a7484aa6ea6e483ull, 0x5cb0a9dcbd41fbd4ull, 0x76f988da831153b5ull,
    0x983e5152ee66dfabull, 0xa831c66d2db43210ull, 0xb00327c898fb213full, 0xbf597fc7beef0ee4ull,
    0xc6e00bf33da88fc2ull, 0xd5a79147930aa725ull, 0x06ca6351e003826full, 0x142929670a0e6e70ull,
    0x27b70a8546d22ffcull, 0x2e1b21385c26c926ull, 0x4d2c6dfc5ac42aedull, 0x53380d139d95b3dfull,
    0x650a73548baf63deull, 0x766a0abb3c77b2a8ull, 0x81c2c92e47edaee6ull, 0x92722c851482353bull,
    0xa2bfe8a14cf10364ull, 0xa81a664bbc423001ull, 0xc24b8b70d0f89791ull, 0xc76c51a30654be30ull,
    0xd192e819d6ef5218ull, 0xd69906245565a910ull, 0xf40e35855771202aull, 0x106aa07032bbd1b8ull,
    0x19a4c116b8d2d0c8ull, 0x1e376c085141ab53ull, 0x2748774cdf8eeb99ull, 0x34b0bcb5e19b48a8ull,
    0x391c0cb3c5c95a63ull, 0x4ed8aa4ae3418acbull, 0x5b9cca4f7763e373ull, 0x682e6ff3d6b2b8a3ull,
    0x748f82ee5defb2fcull, 0x78a5636f43172f60ull, 0x84c87814a1f0ab72ull, 0x8cc702081a6439ecull,
    0x90befffa23631e28ull, 0xa4506cebde82bde9ull, 0xbef9a3f7b2c67915ull, 0xc67178f2e372532bull,
    0xca273eceea26619cull, 0xd186b8c721c0c207ull, 0xeada7dd6cde0eb1eull, 0xf57d4f7fee6ed178ull,
    0x06f067aa72176fbaull, 0x0a637dc5a2c898a6ull, 0x113f9804bef90daeull, 0x1b710b35131c471bull,
    0x28db77f523047d84ull, 0x32caab7b40c72493ull, 0x3c9ebe0a15c9bebcull, 0x431d67c49c100d4cull,
    0x4cc5d4becb3e42b6ull, 0x597f299cfc657e2aull, 0x5fcb6fab3ad6faecull, 0x6c44198c4a475817ull,
};

static const u64 sha512_iv[8] = {
    0x6a09e667f3bcc908ull, 0xbb67ae8584caa73bull, 0x3c6ef372fe94f82bull, 0xa54ff53a5f1d36f1ull,
    0x510e527fade682d1ull, 0x9b05688c2b3e6c1full, 0x1f83d9abfb41bd6bull, 0x5be0cd19137e2179ull,
};

static u64 sha512_rotr(u64 x, u32 n) {
    return (x >> n) | (x << (64u - n));
}

static void sha512_compress(u64 h[8], const u8 *block) {
    u64 w[80];
    u32 i;

    for (i = 0; i < 16u; i++) {
        const u8 *p = block + (usize)i * 8u;
        w[i] = ((u64)p[0] << 56) | ((u64)p[1] << 48) | ((u64)p[2] << 40) | ((u64)p[3] << 32) |
               ((u64)p[4] << 24) | ((u64)p[5] << 16) | ((u64)p[6] << 8) | (u64)p[7];
    }
    for (i = 16u; i < 80u; i++) {
        u64 s0 = sha512_rotr(w[i - 15u], 1u) ^ sha512_rotr(w[i - 15u], 8u) ^ (w[i - 15u] >> 7);
        u64 s1 = sha512_rotr(w[i - 2u], 19u) ^ sha512_rotr(w[i - 2u], 61u) ^ (w[i - 2u] >> 6);
        w[i] = w[i - 16u] + s0 + w[i - 7u] + s1;
    }

    u64 a = h[0], b = h[1], c = h[2], d = h[3];
    u64 e = h[4], f = h[5], g = h[6], hh = h[7];
    for (i = 0; i < 80u; i++) {
        u64 s1 = sha512_rotr(e, 14u) ^ sha512_rotr(e, 18u) ^ sha512_rotr(e, 41u);
        u64 ch = (e & f) ^ (~e & g);
        u64 t1 = hh + s1 + ch + sha512_k[i] + w[i];
        u64 s0 = sha512_rotr(a, 28u) ^ sha512_rotr(a, 34u) ^ sha512_rotr(a, 39u);
        u64 maj = (a & b) ^ (a & c) ^ (b & c);
        u64 t2 = s0 + maj;

        hh = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

void sha512_init(struct sha512_ctx *ctx) {
    memcpy(ctx->h, sha512_iv, sizeof(sha512_iv));
    ctx->bytes = 0;
    ctx->buflen = 0;
}

void sha512_update(struct sha512_ctx *ctx, const void *data, usize len) {
    const u8 *p = (const u8 *)data;

    ctx->bytes += (u64)len;
    while (len > 0) {
        usize space = SHA512_BLOCK_SIZE - ctx->buflen;
        usize take = len < space ? len : space;
        memcpy(ctx->buf + ctx->buflen, p, take);
        ctx->buflen += take;
        p += take;
        len -= take;
        if (ctx->buflen == SHA512_BLOCK_SIZE) {
            sha512_compress(ctx->h, ctx->buf);
            ctx->buflen = 0;
        }
    }
}

void sha512_final(struct sha512_ctx *ctx, u8 out[SHA512_DIGEST_SIZE]) {
    u64 bits = ctx->bytes * 8u;
    u32 i;

    ctx->buf[ctx->buflen++] = 0x80u;
    if (ctx->buflen > SHA512_BLOCK_SIZE - 16u) {
        while (ctx->buflen < SHA512_BLOCK_SIZE) {
            ctx->buf[ctx->buflen++] = 0;
        }
        sha512_compress(ctx->h, ctx->buf);
        ctx->buflen = 0;
    }
    while (ctx->buflen < SHA512_BLOCK_SIZE - 16u) {
        ctx->buf[ctx->buflen++] = 0;
    }
    for (i = 0; i < 8u; i++) {
        ctx->buf[SHA512_BLOCK_SIZE - 1u - i] = (u8)(bits >> (i * 8u));
    }
    for (i = 0; i < 8u; i++) {
        ctx->buf[SHA512_BLOCK_SIZE - 9u - i] = 0;
    }
    sha512_compress(ctx->h, ctx->buf);

    for (i = 0; i < 8u; i++) {
        out[i * 8u + 0u] = (u8)(ctx->h[i] >> 56);
        out[i * 8u + 1u] = (u8)(ctx->h[i] >> 48);
        out[i * 8u + 2u] = (u8)(ctx->h[i] >> 40);
        out[i * 8u + 3u] = (u8)(ctx->h[i] >> 32);
        out[i * 8u + 4u] = (u8)(ctx->h[i] >> 24);
        out[i * 8u + 5u] = (u8)(ctx->h[i] >> 16);
        out[i * 8u + 6u] = (u8)(ctx->h[i] >> 8);
        out[i * 8u + 7u] = (u8)(ctx->h[i]);
    }
    secure_zero(ctx, sizeof(*ctx));
}

void sha512_hash(u8 out[SHA512_DIGEST_SIZE], const void *data, usize len) {
    struct sha512_ctx ctx;

    sha512_init(&ctx);
    sha512_update(&ctx, data, len);
    sha512_final(&ctx, out);
}
