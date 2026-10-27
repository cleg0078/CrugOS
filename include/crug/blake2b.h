#pragma once

#include <crug/types.h>

#define BLAKE2B_BLOCK_SIZE 128u
#define BLAKE2B_MAX_OUT 64u

struct blake2b_ctx {
    u64 h[8];
    u64 t[2];
    u8 buf[BLAKE2B_BLOCK_SIZE];
    usize buflen;
    usize outlen;
};

void blake2b_init(struct blake2b_ctx *ctx, usize outlen);
void blake2b_init_key(struct blake2b_ctx *ctx, usize outlen, const void *key, usize keylen);
void blake2b_update(struct blake2b_ctx *ctx, const void *data, usize len);
void blake2b_final(struct blake2b_ctx *ctx, void *out);
void blake2b_hash(void *out, usize outlen, const void *key, usize keylen, const void *data,
                  usize len);
