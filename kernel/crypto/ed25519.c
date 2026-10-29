/* Phase 7B: Ed25519 signatures (RFC 8032).
   Field elements are five 51-bit limbs in u64 and the group uses extended
   twisted Edwards coordinates. Everything is allocation free, so the kernel
   can verify a package signature without touching the heap. */
#include <crug/ed25519.h>
#include <crug/sha512.h>
#include <crug/string.h>

#define FE_M51 0x7FFFFFFFFFFFFull

typedef struct { u64 v[5]; } fe;
typedef struct { fe x, y, z, t; } ge;

/* ---- field arithmetic ---- */

static void fe_zero(fe *r)
{
    r->v[0] = 0; r->v[1] = 0; r->v[2] = 0; r->v[3] = 0; r->v[4] = 0;
}

static void fe_one(fe *r)
{
    fe_zero(r);
    r->v[0] = 1;
}

static void fe_set_u64(fe *r, u64 x)
{
    fe_zero(r);
    r->v[0] = x;
}

static void fe_copy(fe *d, const fe *s) __attribute__((unused));

static void fe_copy(fe *d, const fe *s)
{
    u32 i;
    for (i = 0; i < 5u; i++)
        d->v[i] = s->v[i];
}

/* Fold every limb back under 2^51; 2^255 == 19 (mod p). */
static void fe_carry(fe *a)
{
    u64 c;

    c = a->v[0] >> 51; a->v[0] &= FE_M51; a->v[1] += c;
    c = a->v[1] >> 51; a->v[1] &= FE_M51; a->v[2] += c;
    c = a->v[2] >> 51; a->v[2] &= FE_M51; a->v[3] += c;
    c = a->v[3] >> 51; a->v[3] &= FE_M51; a->v[4] += c;
    c = a->v[4] >> 51; a->v[4] &= FE_M51; a->v[0] += c * 19u;
    c = a->v[0] >> 51; a->v[0] &= FE_M51; a->v[1] += c;
}

static void fe_add(fe *r, const fe *a, const fe *b)
{
    u32 i;
    for (i = 0; i < 5u; i++)
        r->v[i] = a->v[i] + b->v[i];
    fe_carry(r);
}

static void fe_sub(fe *r, const fe *a, const fe *b)
{
    static const u64 p2[5] = { FE_M51 - 18u, FE_M51, FE_M51, FE_M51, FE_M51 };
    u32 i;
    for (i = 0; i < 5u; i++)
        r->v[i] = a->v[i] + 2u * p2[i] - b->v[i];
    fe_carry(r);
}

static void fe_neg(fe *r, const fe *a)
{
    static const u64 p2[5] = { FE_M51 - 18u, FE_M51, FE_M51, FE_M51, FE_M51 };
    u32 i;
    for (i = 0; i < 5u; i++)
        r->v[i] = 2u * p2[i] - a->v[i];
    fe_carry(r);
}

static void fe_mul(fe *r, const fe *a, const fe *b)
{
    u64 a0 = a->v[0], a1 = a->v[1], a2 = a->v[2], a3 = a->v[3], a4 = a->v[4];
    u64 b0 = b->v[0], b1 = b->v[1], b2 = b->v[2], b3 = b->v[3], b4 = b->v[4];
    __int128 t0, t1, t2, t3, t4;
    u64 c;

    t0 = (__int128)a0 * b0 + (__int128)19 * ((__int128)a1 * b4 + (__int128)a2 * b3 +
                                          (__int128)a3 * b2 + (__int128)a4 * b1);
    t1 = (__int128)a0 * b1 + (__int128)a1 * b0 +
         (__int128)19 * ((__int128)a2 * b4 + (__int128)a3 * b3 + (__int128)a4 * b2);
    t2 = (__int128)a0 * b2 + (__int128)a1 * b1 + (__int128)a2 * b0 +
         (__int128)19 * ((__int128)a3 * b4 + (__int128)a4 * b3);
    t3 = (__int128)a0 * b3 + (__int128)a1 * b2 + (__int128)a2 * b1 + (__int128)a3 * b0 +
         (__int128)19 * ((__int128)a4 * b4);
    t4 = (__int128)a0 * b4 + (__int128)a1 * b3 + (__int128)a2 * b2 + (__int128)a3 * b1 +
         (__int128)a4 * b0;

    c = (u64)(t0 >> 51); r->v[0] = (u64)t0 & FE_M51; t1 += c;
    c = (u64)(t1 >> 51); r->v[1] = (u64)t1 & FE_M51; t2 += c;
    c = (u64)(t2 >> 51); r->v[2] = (u64)t2 & FE_M51; t3 += c;
    c = (u64)(t3 >> 51); r->v[3] = (u64)t3 & FE_M51; t4 += c;
    c = (u64)(t4 >> 51); r->v[4] = (u64)t4 & FE_M51; r->v[0] += c * 19u;
    c = r->v[0] >> 51; r->v[0] &= FE_M51; r->v[1] += c;
}

static void fe_sq(fe *r, const fe *a)
{
    fe_mul(r, a, a);
}

/* 32 little-endian bytes -> five limbs (bit by bit: simple and obviously
   correct, which matters more here than a few hundred cycles). */
static void fe_frombytes(fe *r, const u8 b[32])
{
    u32 i, j;
    for (i = 0; i < 5u; i++) {
        u64 v = 0;
        for (j = 0; j < 51u; j++) {
            u32 bit = i * 51u + j;
            u64 val = (u64)((b[bit >> 3] >> (bit & 7u)) & 1u);
            v |= val << j;
        }
        r->v[i] = v;
    }
    fe_carry(r);
}

static void fe_tobytes(u8 out[32], const fe *a)
{
    fe t = *a;
    u64 q, w0, w1, w2, w3;

    fe_carry(&t);
    fe_carry(&t);
    /* q == 1 when the value is at least p; adding 19*q then carrying wraps
       it back into the canonical range. */
    q = (t.v[0] + 19u) >> 51;
    q = (t.v[1] + q) >> 51;
    q = (t.v[2] + q) >> 51;
    q = (t.v[3] + q) >> 51;
    q = (t.v[4] + q) >> 51;
    t.v[0] += 19u * q;
    fe_carry(&t);

    w0 = t.v[0] | (t.v[1] << 51);
    w1 = (t.v[1] >> 13) | (t.v[2] << 38);
    w2 = (t.v[2] >> 26) | (t.v[3] << 25);
    w3 = (t.v[3] >> 39) | (t.v[4] << 12);

    out[0] = (u8)w0; out[1] = (u8)(w0 >> 8); out[2] = (u8)(w0 >> 16);
    out[3] = (u8)(w0 >> 24); out[4] = (u8)(w0 >> 32); out[5] = (u8)(w0 >> 40);
    out[6] = (u8)(w0 >> 48); out[7] = (u8)(w0 >> 56);
    out[8] = (u8)w1; out[9] = (u8)(w1 >> 8); out[10] = (u8)(w1 >> 16);
    out[11] = (u8)(w1 >> 24); out[12] = (u8)(w1 >> 32); out[13] = (u8)(w1 >> 40);
    out[14] = (u8)(w1 >> 48); out[15] = (u8)(w1 >> 56);
    out[16] = (u8)w2; out[17] = (u8)(w2 >> 8); out[18] = (u8)(w2 >> 16);
    out[19] = (u8)(w2 >> 24); out[20] = (u8)(w2 >> 32); out[21] = (u8)(w2 >> 40);
    out[22] = (u8)(w2 >> 48); out[23] = (u8)(w2 >> 56);
    out[24] = (u8)w3; out[25] = (u8)(w3 >> 8); out[26] = (u8)(w3 >> 16);
    out[27] = (u8)(w3 >> 24); out[28] = (u8)(w3 >> 32); out[29] = (u8)(w3 >> 40);
    out[30] = (u8)(w3 >> 48); out[31] = (u8)(w3 >> 56);
}

/* Exponents as 32-byte little-endian values. */
static const u8 ED_E_INV[32] = {
    0xEBu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu,
    0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu,
    0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu,
    0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0x7Fu,
};

/* (p-5)/8 = 2^252 - 3 */
static const u8 ED_E_P58[32] = {
    0xFDu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu,
    0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu,
    0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu,
    0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0x0Fu,
};

/* (p-1)/4 = 2^253 - 5 */
static const u8 ED_E_SQRTM1[32] = {
    0xFBu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu,
    0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu,
    0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu,
    0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0x1Fu,
};

static void fe_pow(fe *r, const fe *a, const u8 e[32])
{
    fe acc;
    int i;

    fe_one(&acc);
    for (i = 254; i >= 0; i--) {
        fe_sq(&acc, &acc);
        if ((e[i >> 3] >> (i & 7)) & 1u)
            fe_mul(&acc, &acc, a);
    }
    *r = acc;
}

static void fe_invert(fe *r, const fe *a)
{
    fe_pow(r, a, ED_E_INV);
}

static int fe_equal(const fe *a, const fe *b)
{
    u8 x[32], y[32];
    fe_tobytes(x, a);
    fe_tobytes(y, b);
    return memcmp(x, y, 32) == 0;
}

/* ---- group arithmetic ---- */

static void ge_identity(ge *p)
{
    fe_zero(&p->x);
    fe_one(&p->y);
    fe_one(&p->z);
    fe_zero(&p->t);
}

static fe g_d, g_d2, g_sqrtm1;
static ge g_base;
static int g_ed_init;

/* sqrt(u/v) for the twisted Edwards curve; returns 0 on success. */
static int fe_sqrt_ratio(fe *r, const fe *u, const fe *v)
{
    fe v3, v7, t, x, chk;
    fe neg;
    u8 xb[32];

    fe_sq(&v3, v);
    fe_mul(&v3, &v3, v);            /* v^3 */
    fe_sq(&v7, &v3);
    fe_mul(&v7, &v7, v);            /* v^7 */
    fe_mul(&t, u, &v7);
    fe_pow(&x, &t, ED_E_P58);
    fe_mul(&x, &x, u);
    fe_mul(&x, &x, &v3);            /* x = u*v^3*(u*v^7)^((p-5)/8) */
    fe_sq(&chk, &x);
    fe_mul(&chk, &chk, v);
    if (!fe_equal(&chk, u)) {
        fe_neg(&neg, u);
        if (!fe_equal(&chk, &neg))
            return -1;
        fe_mul(&x, &x, &g_sqrtm1);
    }
    fe_tobytes(xb, &x);
    if (xb[0] == 0u && (xb[1] | xb[2] | xb[3]) == 0u) {
        /* x == 0: only the sign bit 0 encoding is canonical */
    }
    *r = x;
    return 0;
}

static int ge_frombytes(ge *p, const u8 s[32])
{
    u8 yb[32], chk[32], xb[32];
    fe y, u, v, one, x;

    memcpy(yb, s, 32);
    yb[31] &= 0x7Fu;
    fe_frombytes(&y, yb);
    fe_tobytes(chk, &y);
    if (memcmp(chk, yb, 32) != 0)
        return -1;                     /* y >= p is not canonical */
    fe_one(&one);
    fe_sq(&u, &y);
    fe_sub(&u, &u, &one);
    fe_sq(&v, &y);
    fe_mul(&v, &v, &g_d);
    fe_add(&v, &v, &one);
    if (fe_sqrt_ratio(&x, &u, &v) != 0)
        return -1;
    fe_tobytes(xb, &x);
    if ((xb[0] & 1u) != (u8)((s[31] >> 7) & 1u))
        fe_neg(&x, &x);
    p->x = x;
    p->y = y;
    fe_one(&p->z);
    fe_mul(&p->t, &p->x, &p->y);
    return 0;
}

static void ge_tobytes(u8 out[32], const ge *p)
{
    fe zinv, x, y;
    u8 xb[32];

    fe_invert(&zinv, &p->z);
    fe_mul(&x, &p->x, &zinv);
    fe_mul(&y, &p->y, &zinv);
    fe_tobytes(out, &y);
    fe_tobytes(xb, &x);
    out[31] |= (u8)((xb[0] & 1u) << 7);
}

/* Unified add-2008-hwcd-3; also doubles when p == q == r. */
static void ge_add(ge *r, const ge *p, const ge *q)
{
    fe a, b, c, d, e, f, g, h, t0, t1;

    fe_sub(&t0, &p->y, &p->x);
    fe_sub(&t1, &q->y, &q->x);
    fe_mul(&a, &t0, &t1);
    fe_add(&t0, &p->y, &p->x);
    fe_add(&t1, &q->y, &q->x);
    fe_mul(&b, &t0, &t1);
    fe_mul(&c, &p->t, &q->t);
    fe_mul(&c, &c, &g_d2);
    fe_mul(&d, &p->z, &q->z);
    fe_add(&d, &d, &d);
    fe_sub(&e, &b, &a);
    fe_sub(&f, &d, &c);
    fe_add(&g, &d, &c);
    fe_add(&h, &b, &a);
    fe_mul(&r->x, &e, &f);
    fe_mul(&r->y, &g, &h);
    fe_mul(&r->t, &e, &h);
    fe_mul(&r->z, &f, &g);
}

static void ge_scalarmult(ge *r, const u8 s[32], const ge *p)
{
    ge acc;
    int i;

    ge_identity(&acc);
    for (i = 255; i >= 0; i--) {
        ge_add(&acc, &acc, &acc);
        if ((s[i >> 3] >> (i & 7)) & 1u)
            ge_add(&acc, &acc, p);
    }
    *r = acc;
}

static void ed_init(void)
{
    fe t, four, five, inv, y;
    u8 enc[32];

    if (g_ed_init)
        return;
    g_ed_init = 1;
    /* d = -121665/121666 */
    fe_set_u64(&t, 121666u);
    fe_invert(&inv, &t);
    fe_set_u64(&t, 121665u);
    fe_mul(&g_d, &inv, &t);
    fe_neg(&g_d, &g_d);
    fe_add(&g_d2, &g_d, &g_d);
    /* sqrt(-1) = 2^((p-1)/4) */
    fe_set_u64(&t, 2u);
    fe_pow(&g_sqrtm1, &t, ED_E_SQRTM1);
    /* B = (x, 4/5) with the sign bit clear */
    fe_set_u64(&four, 4u);
    fe_set_u64(&five, 5u);
    fe_invert(&inv, &five);
    fe_mul(&y, &four, &inv);
    fe_tobytes(enc, &y);
    enc[31] &= 0x7Fu;
    (void)ge_frombytes(&g_base, enc);
}

/* ---- scalars modulo the group order ---- */

static const u8 SC_L[32] = {
    0xEDu, 0xD3u, 0xF5u, 0x5Cu, 0x1Au, 0x63u, 0x12u, 0x58u,
    0xD6u, 0x9Cu, 0xF7u, 0xA2u, 0xDEu, 0xF9u, 0xDEu, 0x14u,
    0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
    0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x10u,
};

static int sc_lt_l(const u8 x[32])
{
    int i;
    for (i = 31; i >= 0; i--) {
        if (x[i] != SC_L[i])
            return x[i] < SC_L[i];
    }
    return 0;
}

static void sc_sub_l(u8 x[32])
{
    u32 borrow = 0, i;
    for (i = 0; i < 32u; i++) {
        u32 v = (u32)x[i] - (u32)SC_L[i] - borrow;
        borrow = (v >> 8) & 1u;
        x[i] = (u8)v;
    }
}

static void sc_add_mod(u8 r[32], const u8 a[32], const u8 b[32])
{
    u32 carry = 0, i;
    for (i = 0; i < 32u; i++) {
        u32 v = (u32)a[i] + (u32)b[i] + carry;
        r[i] = (u8)v;
        carry = v >> 8;
    }
    if (carry != 0u || !sc_lt_l(r))
        sc_sub_l(r);
}

static void sc_double(u8 r[32])
{
    u32 carry = 0, i;
    for (i = 0; i < 32u; i++) {
        u32 v = ((u32)r[i] << 1) | carry;
        r[i] = (u8)v;
        carry = v >> 8;
    }
    if (carry != 0u || !sc_lt_l(r))
        sc_sub_l(r);
}

static void sc_reduce(u8 out[32], const u8 in[64])
{
    u8 r[32];
    int i;

    memset(r, 0, sizeof(r));
    for (i = 511; i >= 0; i--) {
        sc_double(r);
        if ((in[i >> 3] >> (i & 7)) & 1u) {
            u32 k;
            for (k = 0; k < 32u; k++) {
                r[k]++;
                if (r[k] != 0u)
                    break;
            }
            if (!sc_lt_l(r))
                sc_sub_l(r);
        }
    }
    memcpy(out, r, 32);
}

static void sc_muladd(u8 out[32], const u8 a[32], const u8 b[32], const u8 c[32])
{
    u8 cur[32], acc[32];
    u32 i;

    memcpy(cur, a, 32);
    memset(acc, 0, sizeof(acc));
    for (i = 0; i < 256u; i++) {
        if ((b[i >> 3] >> (i & 7)) & 1u)
            sc_add_mod(acc, acc, cur);
        sc_double(cur);
    }
    sc_add_mod(acc, acc, c);
    memcpy(out, acc, 32);
}

/* ---- the public API ---- */

int ed25519_keypair(u8 pk[ED25519_PK_SIZE], u8 sk[ED25519_SK_SIZE],
                    const u8 seed[ED25519_SEED_SIZE])
{
    u8 az[64];
    ge a;

    if (!pk || !sk || !seed)
        return -1;
    ed_init();
    sha512_hash(az, seed, 32);
    az[0] &= 248u;
    az[31] &= 127u;
    az[31] |= 64u;
    ge_scalarmult(&a, az, &g_base);
    ge_tobytes(pk, &a);
    memcpy(sk, seed, 32);
    memcpy(sk + 32, pk, 32);
    secure_zero(az, sizeof(az));
    return 0;
}

int ed25519_sign(u8 sig[ED25519_SIG_SIZE], const u8 sk[ED25519_SK_SIZE],
                 const void *msg, usize len)
{
    struct sha512_ctx ctx;
    u8 az[64], h[64], r[32], k[32], s[32], R[32];
    ge rp;

    if (!sig || !sk || (!msg && len != 0u))
        return -1;
    ed_init();
    sha512_hash(az, sk, 32);
    az[0] &= 248u;
    az[31] &= 127u;
    az[31] |= 64u;
    sha512_init(&ctx);
    sha512_update(&ctx, az + 32, 32);
    sha512_update(&ctx, msg, len);
    sha512_final(&ctx, h);
    sc_reduce(r, h);
    ge_scalarmult(&rp, r, &g_base);
    ge_tobytes(R, &rp);
    sha512_init(&ctx);
    sha512_update(&ctx, R, 32);
    sha512_update(&ctx, sk + 32, 32);
    sha512_update(&ctx, msg, len);
    sha512_final(&ctx, h);
    sc_reduce(k, h);
    {
        /* az is the clamped secret scalar as an integer; it can exceed the
           group order, so reduce it before the modular multiplication. */
        u8 wide[64];
        u8 a_mod[32];
        memcpy(wide, az, 32);
        memset(wide + 32, 0, 32);
        sc_reduce(a_mod, wide);
        sc_muladd(s, k, a_mod, r);
    }
    memcpy(sig, R, 32);
    memcpy(sig + 32, s, 32);
    secure_zero(az, sizeof(az));
    secure_zero(h, sizeof(h));
    return 0;
}

#ifdef CRUG_HOST_TEST
void ed25519_selftest_reduce(const u8 in[64], u8 out[32])
{
    ed_init();
    sc_reduce(out, in);
}

void ed25519_selftest_enc_base(const u8 s[32], u8 out[32])
{
    ge g;
    ed_init();
    ge_scalarmult(&g, s, &g_base);
    ge_tobytes(out, &g);
}
#endif

int ed25519_verify(const u8 pk[ED25519_PK_SIZE], const void *msg, usize len,
                   const u8 sig[ED25519_SIG_SIZE])
{
    struct sha512_ctx ctx;
    ge A, sB, hA, rhs;
    u8 h[64], k[32], enc[32];

    if (!pk || !sig || (!msg && len != 0u))
        return -1;
    ed_init();
    if (!sc_lt_l(sig + 32))
        return -1;                     /* S must be canonical */
    if (ge_frombytes(&A, pk) != 0)
        return -1;
    sha512_init(&ctx);
    sha512_update(&ctx, sig, 32);
    sha512_update(&ctx, pk, 32);
    sha512_update(&ctx, msg, len);
    sha512_final(&ctx, h);
    sc_reduce(k, h);
    ge_scalarmult(&sB, sig + 32, &g_base);
    {
        ge neg = A;
        fe_neg(&neg.x, &A.x);
        fe_neg(&neg.t, &A.t);
        ge_scalarmult(&hA, k, &neg);
    }
    ge_add(&rhs, &sB, &hA);
    ge_tobytes(enc, &rhs);
    return memcmp(enc, sig, 32) == 0 ? 0 : -1;
}
