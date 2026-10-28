/* CrugOS crypt.device: AES-256-XTS encrypted volume header and keyslot metadata.
 *
 * A crypt volume is a GPT partition with type GUID
 * 8f1c7d2e-3b45-4c6a-9e01-c2a1b7f0d9e3 named "crugcrypt". The first 32
 * sectors (16 KiB) hold a fixed header plus eight 256-byte keyslots; the rest of
 * the partition is the XTS-encrypted data area (data-relative sector LBA is the
 * XTS data unit index).
 *
 * Key hierarchy:
 *   Argon2id(password, salt, m, t, p) -> 96 bytes
 *     derived[0..64)  XTS key-encryption key
 *     derived[64..96) slot MAC key
 *   master[0..32) data key, master[32..64) tweak key  (64 random bytes)
 *   keyslot.enc_master = XTS(KEK, tweak = LE64(slot) || nonce[0..8), master)
 *   keyslot.mac        = BLAKE2b-256(key = MAC key, "CRUG-SLOT-MAC\0" ||
 *                        LE32(slot) || header[0..4096) || slot with mac zeroed)
 *
 * The per-slot MAC covers the immutable header, so new slots can be sealed
 * independently; it does not cover the other slots, so deleting one is a
 * denial of service rather than a forgery.
 */
#ifndef CRUG_CRYPT_H
#define CRUG_CRYPT_H

#include <crug/block.h>
#include <crug/types.h>

#define CRUG_CRYPT_MAGIC "CRUGCRYP"
#define CRUG_CRYPT_MAGIC_LEN 8u
#define CRUG_CRYPT_VERSION 1u
#define CRUG_CRYPT_HEADER_BYTES 16384u
#define CRUG_CRYPT_HEADER_SECTORS 32u
#define CRUG_CRYPT_CIPHER_AES256_XTS 1u
#define CRUG_CRYPT_KDF_ARGON2ID 1u
#define CRUG_CRYPT_SLOT_COUNT 8u
#define CRUG_CRYPT_SLOT_BYTES 256u
#define CRUG_CRYPT_SLOTS_OFFSET 4096u
#define CRUG_CRYPT_MASTER_BYTES 64u
#define CRUG_CRYPT_DERIVED_BYTES 96u
#define CRUG_CRYPT_MAC_BYTES 32u
#define CRUG_CRYPT_NONCE_BYTES 32u
#define CRUG_CRYPT_SALT_BYTES 32u
#define CRUG_CRYPT_VOLUME_ID_BYTES 16u
#define CRUG_CRYPT_MAX_PASSWORD 256u
#define CRUG_CRYPT_MAX_M_COST_KIB (1024u * 1024u)
#define CRUG_CRYPT_MAX_T_COST 64u
#define CRUG_CRYPT_MAX_LANES 16u

/* Unlock-time KDF policy. crypt_meta_validate() enforces the format-time
   ceiling (1 GiB, t=64); boot additionally refuses a header that would force
   a multi-minute pre-auth derivation or a trivially weak one. */
#define CRUG_CRYPT_UNLOCK_MIN_M_COST_KIB 1024u
#define CRUG_CRYPT_UNLOCK_MAX_M_COST_KIB 65536u
#define CRUG_CRYPT_UNLOCK_MIN_T_COST 2u
#define CRUG_CRYPT_UNLOCK_MAX_T_COST 8u

#define CRUG_CRYPT_SLOT_EMPTY 0u
#define CRUG_CRYPT_SLOT_ACTIVE 1u
#define CRUG_CRYPT_SLOT_FLAG_PASSWORD 0u
#define CRUG_CRYPT_SLOT_FLAG_RECOVERY 1u

struct crug_crypt_header {
    u8 magic[8];
    u32 version;
    u32 header_bytes;
    u32 cipher;
    u32 kdf;
    u32 kdf_m_cost_kib;
    u32 kdf_t_cost;
    u32 kdf_lanes;
    u32 slot_count;
    u32 slot_bytes;
    u32 slots_offset;
    u64 data_offset_sectors;
    u64 volume_sectors;
    u8 salt[32];
    u8 volume_id[16];
    u32 flags;
    u8 reserved[140];
};

struct crug_crypt_keyslot {
    u32 state;
    u32 flags;
    u8 nonce[32];
    u8 enc_master[64];
    u8 mac[32];
    u8 reserved[120];
};

/* Compile-time layout guarantees shared by kernel and host tool. */
typedef char crug_crypt_header_size_check[(sizeof(struct crug_crypt_header) == 256) ? 1 : -1];
typedef char crug_crypt_slot_size_check[(sizeof(struct crug_crypt_keyslot) == 256) ? 1 : -1];

/* Header/slot helpers shared by the kernel and tools/mkcrypt. They never allocate
 * and never touch block devices; scratch must hold crypt_meta_scratch_bytes(). */
int crypt_meta_format_header(u8 *header, u64 volume_sectors, u32 m_cost_kib, u32 t_cost,
                             u32 lanes, const u8 salt[32], const u8 volume_id[16]);
int crypt_meta_validate(const struct crug_crypt_header *header);
u64 crypt_meta_scratch_bytes(const struct crug_crypt_header *header);
int crypt_meta_seal_slot(u8 *header, u32 slot, u32 flags, const u8 nonce[32],
                         const u8 *password, usize password_len, const u8 master[64],
                         void *scratch, usize scratch_bytes);
int crypt_meta_unseal_slot(const u8 *header, u32 slot, const u8 *password, usize password_len,
                           u8 master[64], void *scratch, usize scratch_bytes);

/* Kernel block-device glue (kernel/block/crypt.c, not linked into the host tool on
 * purpose: it uses pmm_alloc_pages for the Argon2 workspace). Returns a new
 * block device on success, 0 if no active keyslot accepts the password. */
struct block_device *crypt_unlock(struct block_device *parent, u64 offset_sectors,
                                  const u8 *password, usize password_len);
void crypt_wipe_keys(struct block_device *bd);

#endif /* CRUG_CRYPT_H */
