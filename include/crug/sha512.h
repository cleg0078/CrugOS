/* SHA-512 (FIPS 180-4). */
#ifndef CRUG_SHA512_H
#define CRUG_SHA512_H

#include <crug/types.h>

#define SHA512_BLOCK_SIZE 128u
#define SHA512_DIGEST_SIZE 64u

struct sha512_ctx {
    u64 h[8];
    u64 bytes;
    u8 buf[SHA512_BLOCK_SIZE];
    usize buflen;
};

void sha512_init(struct sha512_ctx *ctx);
void sha512_update(struct sha512_ctx *ctx, const void *data, usize len);
void sha512_final(struct sha512_ctx *ctx, u8 out[SHA512_DIGEST_SIZE]);
void sha512_hash(u8 out[SHA512_DIGEST_SIZE], const void *data, usize len);

#endif /* CRUG_SHA512_H */
