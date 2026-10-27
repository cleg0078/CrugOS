#pragma once

#include <crug/types.h>
#include <crug/aes.h>

#define AES_XTS_KEY_SIZE 64u

struct aes256_xts_key {
    u8 data_rk[AES256_ROUND_KEYS];
    u8 tweak_rk[AES256_ROUND_KEYS];
};

void aes256_xts_set_key(struct aes256_xts_key *key, const u8 raw[AES_XTS_KEY_SIZE]);
int aes256_xts_encrypt(const struct aes256_xts_key *key, const u8 tweak[AES_BLOCK_SIZE],
                       void *buf, usize len);
int aes256_xts_decrypt(const struct aes256_xts_key *key, const u8 tweak[AES_BLOCK_SIZE],
                       void *buf, usize len);
void aes256_xts_wipe(struct aes256_xts_key *key);
void aes256_xts_sector_tweak(u8 tweak[AES_BLOCK_SIZE], u64 index);
