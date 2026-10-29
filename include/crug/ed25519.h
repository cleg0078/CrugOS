/* Ed25519 signatures (RFC 8032), freestanding and allocation free. */
#ifndef CRUG_ED25519_H
#define CRUG_ED25519_H

#include <crug/types.h>

#define ED25519_SEED_SIZE 32u
#define ED25519_PK_SIZE   32u
#define ED25519_SK_SIZE   64u
#define ED25519_SIG_SIZE  64u

/* Build a key pair from a 32-byte seed. sk = seed || public key. */
int ed25519_keypair(u8 pk[ED25519_PK_SIZE], u8 sk[ED25519_SK_SIZE],
                    const u8 seed[ED25519_SEED_SIZE]);

/* Sign msg with sk. Returns 0, or -1 on a bad argument. */
int ed25519_sign(u8 sig[ED25519_SIG_SIZE], const u8 sk[ED25519_SK_SIZE],
                 const void *msg, usize len);

/* Verify sig over msg with pk. Returns 0 when the signature is valid, -1
   when it is not (or the public key is not a valid point). */
int ed25519_verify(const u8 pk[ED25519_PK_SIZE], const void *msg, usize len,
                   const u8 sig[ED25519_SIG_SIZE]);

#endif /* CRUG_ED25519_H */
