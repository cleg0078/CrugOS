/*
 * crc32c.h -- CRC32C (Castagnoli) used by ext4 metadata checksums and the
 * JBD2 journal checksum v2 format.
 *
 * The primitive is the raw reflected CRC32C without an implicit initial or
 * final xor, exactly as ext4 and JBD2 use it: the caller passes the seed and
 * may chain calls across a byte range in pieces, because
 * crc32c(crc32c(s, a), b) == crc32c(s, a || b).
 */
#ifndef CRUG_CRC32C_H
#define CRUG_CRC32C_H

#include <crug/types.h>

/* Seed values used by the on-disk formats. */
#define CRC32C_SEED_ZERO 0x00000000u
#define CRC32C_SEED_ONES 0xFFFFFFFFu

u32 crc32c(u32 seed, const void *data, usize len);

#endif /* CRUG_CRC32C_H */
