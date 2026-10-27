#pragma once

#include <crug/types.h>

#define AES_BLOCK_SIZE 16u
#define AES256_KEY_SIZE 32u
#define AES256_ROUND_KEYS 240u

void aes_init(void);
int aes_hardware_available(void);
const char *aes_implementation_name(void);

void aes256_expand_key(u8 rk[AES256_ROUND_KEYS], const u8 key[AES256_KEY_SIZE]);
void aes256_encrypt_block(const u8 rk[AES256_ROUND_KEYS], const u8 in[AES_BLOCK_SIZE],
                          u8 out[AES_BLOCK_SIZE]);
void aes256_decrypt_block(const u8 rk[AES256_ROUND_KEYS], const u8 in[AES_BLOCK_SIZE],
                          u8 out[AES_BLOCK_SIZE]);

/* Software cores kept visible so tests can compare them with the AES-NI path. */
void aes256_encrypt_block_soft(const u8 rk[AES256_ROUND_KEYS], const u8 in[AES_BLOCK_SIZE],
                               u8 out[AES_BLOCK_SIZE]);
void aes256_decrypt_block_soft(const u8 rk[AES256_ROUND_KEYS], const u8 in[AES_BLOCK_SIZE],
                               u8 out[AES_BLOCK_SIZE]);

#if defined(__x86_64__)
void aesni_encrypt_block(const u8 *rk, const u8 *in, u8 *out);
void aesni_decrypt_block(const u8 *rk, const u8 *in, u8 *out);
#endif
