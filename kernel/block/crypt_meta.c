/* crypt.device metadata: header layout, keyslot sealing and opening.
 *
 * Deliberately allocation-free so the same code backs the kernel and the host
 * tool tools/mkcrypt. Slots are sealed with XTS under an Argon2id-derived
 * key-encryption key and authenticated with a keyed BLAKE2b-256 over the
 * immutable header plus the slot itself. See include/crug/crypt.h.
 */
#include <crug/argon2.h>
#include <crug/blake2b.h>
#include <crug/crypt.h>
#include <crug/string.h>
#include <crug/xts.h>

static const u8 crypt_mac_domain[16] = {
    'C', 'R', 'U', 'G', '-', 'S', 'L', 'O', 'T', '-', 'M', 'A', 'C', 0, 0, 0
};

static u32 le32_get(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static void le32_put(u8 *p, u32 v)
{
    p[0] = (u8)(v & 0xff);
    p[1] = (u8)((v >> 8) & 0xff);
    p[2] = (u8)((v >> 16) & 0xff);
    p[3] = (u8)((v >> 24) & 0xff);
}

static void le64_put(u8 *p, u64 v)
{
    le32_put(p, (u32)(v & 0xffffffffu));
    le32_put(p + 4, (u32)(v >> 32));
}

static int crypt_ct_equal(const u8 *a, const u8 *b, usize len)
{
    u8 diff = 0;
    usize i;

    for (i = 0; i < len; i++) {
        diff |= (u8)(a[i] ^ b[i]);
    }
    return diff == 0 ? 1 : 0;
}

static u8 *crypt_slot_ptr(u8 *header, u32 slot)
{
    return header + CRUG_CRYPT_SLOTS_OFFSET + (usize)slot * CRUG_CRYPT_SLOT_BYTES;
}

static const u8 *crypt_slot_const(const u8 *header, u32 slot)
{
    return header + CRUG_CRYPT_SLOTS_OFFSET + (usize)slot * CRUG_CRYPT_SLOT_BYTES;
}

/* tweak = LE64(slot index) || nonce[0..8): unique per slot and per reformat. */
static void crypt_slot_tweak(const u8 *slot, u32 index, u8 tweak[16])
{
    memset(tweak, 0, 16);
    le64_put(tweak, (u64)index);
    memcpy(tweak + 8, slot + 8, 8);
}

static void crypt_slot_mac(const u8 *header, u32 slot, const u8 mac_key[32], u8 out[32])
{
    struct blake2b_ctx ctx;
    u8 index[4];
    u8 copy[CRUG_CRYPT_SLOT_BYTES];

    memcpy(copy, crypt_slot_const(header, slot), CRUG_CRYPT_SLOT_BYTES);
    memset(copy + 0x68, 0, CRUG_CRYPT_MAC_BYTES);
    le32_put(index, slot);

    blake2b_init_key(&ctx, CRUG_CRYPT_MAC_BYTES, mac_key, 32);
    blake2b_update(&ctx, crypt_mac_domain, sizeof(crypt_mac_domain));
    blake2b_update(&ctx, index, sizeof(index));
    blake2b_update(&ctx, header, CRUG_CRYPT_SLOTS_OFFSET);
    blake2b_update(&ctx, copy, sizeof(copy));
    blake2b_final(&ctx, out);
}

int crypt_meta_format_header(u8 *header, u64 volume_sectors, u32 m_cost_kib, u32 t_cost,
                             u32 lanes, const u8 salt[32], const u8 volume_id[16])
{
    struct crug_crypt_header *h = (struct crug_crypt_header *)header;

    if (header == 0 || salt == 0 || volume_id == 0) {
        return -1;
    }
    memset(header, 0, CRUG_CRYPT_HEADER_BYTES);
    memcpy(h->magic, CRUG_CRYPT_MAGIC, CRUG_CRYPT_MAGIC_LEN);
    h->version = CRUG_CRYPT_VERSION;
    h->header_bytes = CRUG_CRYPT_HEADER_BYTES;
    h->cipher = CRUG_CRYPT_CIPHER_AES256_XTS;
    h->kdf = CRUG_CRYPT_KDF_ARGON2ID;
    h->kdf_m_cost_kib = m_cost_kib;
    h->kdf_t_cost = t_cost;
    h->kdf_lanes = lanes;
    h->slot_count = CRUG_CRYPT_SLOT_COUNT;
    h->slot_bytes = CRUG_CRYPT_SLOT_BYTES;
    h->slots_offset = CRUG_CRYPT_SLOTS_OFFSET;
    h->data_offset_sectors = CRUG_CRYPT_HEADER_SECTORS;
    h->volume_sectors = volume_sectors;
    memcpy(h->salt, salt, CRUG_CRYPT_SALT_BYTES);
    memcpy(h->volume_id, volume_id, CRUG_CRYPT_VOLUME_ID_BYTES);
    h->flags = 0;
    return crypt_meta_validate(h);
}

int crypt_meta_validate(const struct crug_crypt_header *h)
{
    if (h == 0) {
        return -1;
    }
    if (memcmp(h->magic, CRUG_CRYPT_MAGIC, CRUG_CRYPT_MAGIC_LEN) != 0) {
        return -1;
    }
    if (h->version != CRUG_CRYPT_VERSION || h->header_bytes != CRUG_CRYPT_HEADER_BYTES) {
        return -1;
    }
    if (h->cipher != CRUG_CRYPT_CIPHER_AES256_XTS || h->kdf != CRUG_CRYPT_KDF_ARGON2ID) {
        return -1;
    }
    if (h->kdf_lanes < 1 || h->kdf_lanes > CRUG_CRYPT_MAX_LANES) {
        return -1;
    }
    if (h->kdf_t_cost < 1 || h->kdf_t_cost > CRUG_CRYPT_MAX_T_COST) {
        return -1;
    }
    if (h->kdf_m_cost_kib < 8u * h->kdf_lanes ||
        h->kdf_m_cost_kib > CRUG_CRYPT_MAX_M_COST_KIB) {
        return -1;
    }
    if (h->slot_count < 1 || h->slot_count > CRUG_CRYPT_SLOT_COUNT) {
        return -1;
    }
    if (h->slot_bytes != CRUG_CRYPT_SLOT_BYTES || h->slots_offset != CRUG_CRYPT_SLOTS_OFFSET) {
        return -1;
    }
    if (h->slots_offset + h->slot_count * h->slot_bytes > h->header_bytes) {
        return -1;
    }
    if (h->data_offset_sectors != CRUG_CRYPT_HEADER_SECTORS) {
        return -1;
    }
    if (h->volume_sectors <= CRUG_CRYPT_HEADER_SECTORS) {
        return -1;
    }
    return 0;
}

u64 crypt_meta_scratch_bytes(const struct crug_crypt_header *h)
{
    if (h == 0) {
        return 0;
    }
    return (u64)h->kdf_m_cost_kib * 1024u;
}

int crypt_meta_seal_slot(u8 *header, u32 slot, u32 flags, const u8 nonce[32],
                         const u8 *password, usize password_len, const u8 master[64],
                         void *scratch, usize scratch_bytes)
{
    struct crug_crypt_header *h = (struct crug_crypt_header *)header;
    u8 *s;
    u8 derived[CRUG_CRYPT_DERIVED_BYTES];
    u8 tweak[16];
    int ret;

    if (header == 0 || nonce == 0 || password == 0 || password_len == 0 ||
        password_len > CRUG_CRYPT_MAX_PASSWORD || master == 0 || scratch == 0) {
        return -1;
    }
    if (crypt_meta_validate(h) != 0 || slot >= h->slot_count) {
        return -1;
    }
    if (scratch_bytes < crypt_meta_scratch_bytes(h)) {
        return -1;
    }
    if (argon2id_hash_mem(derived, sizeof(derived), password, password_len, h->salt,
                          CRUG_CRYPT_SALT_BYTES, h->kdf_m_cost_kib, h->kdf_t_cost, h->kdf_lanes,
                          scratch, scratch_bytes) != 0) {
        memset(derived, 0, sizeof(derived));
        return -1;
    }

    s = crypt_slot_ptr(header, slot);
    memset(s, 0, CRUG_CRYPT_SLOT_BYTES);
    le32_put(s + 0x00, CRUG_CRYPT_SLOT_ACTIVE);
    le32_put(s + 0x04, flags);
    memcpy(s + 0x08, nonce, CRUG_CRYPT_NONCE_BYTES);
    memcpy(s + 0x28, master, CRUG_CRYPT_MASTER_BYTES);
    crypt_slot_tweak(s, slot, tweak);
    {
        struct aes256_xts_key key;

        aes256_xts_set_key(&key, derived);
        aes256_xts_encrypt(&key, tweak, s + 0x28, CRUG_CRYPT_MASTER_BYTES);
        aes256_xts_wipe(&key);
    }
    crypt_slot_mac(header, slot, derived + 64, s + 0x68);

    /* Round-trip check with the just-written slot: catches bad nonces or params. */
    ret = crypt_meta_unseal_slot(header, slot, password, password_len, derived, scratch,
                                 scratch_bytes);
    if (ret == 0) {
        ret = crypt_ct_equal(derived, master, CRUG_CRYPT_MASTER_BYTES) ? 0 : -1;
    }
    memset(derived, 0, sizeof(derived));
    if (ret != 0) {
        memset(s, 0, CRUG_CRYPT_SLOT_BYTES);
    }
    return ret;
}

int crypt_meta_unseal_slot(const u8 *header, u32 slot, const u8 *password, usize password_len,
                           u8 master[64], void *scratch, usize scratch_bytes)
{
    const struct crug_crypt_header *h = (const struct crug_crypt_header *)header;
    const u8 *s;
    u8 derived[CRUG_CRYPT_DERIVED_BYTES];
    u8 mac[CRUG_CRYPT_MAC_BYTES];
    u8 tweak[16];
    int ok;

    if (header == 0 || password == 0 || password_len == 0 ||
        password_len > CRUG_CRYPT_MAX_PASSWORD || master == 0 || scratch == 0) {
        return -1;
    }
    if (crypt_meta_validate(h) != 0 || slot >= h->slot_count) {
        return -1;
    }
    if (scratch_bytes < crypt_meta_scratch_bytes(h)) {
        return -1;
    }
    s = crypt_slot_const(header, slot);
    if (le32_get(s + 0x00) != CRUG_CRYPT_SLOT_ACTIVE) {
        return -1;
    }
    if (argon2id_hash_mem(derived, sizeof(derived), password, password_len, h->salt,
                          CRUG_CRYPT_SALT_BYTES, h->kdf_m_cost_kib, h->kdf_t_cost, h->kdf_lanes,
                          scratch, scratch_bytes) != 0) {
        memset(derived, 0, sizeof(derived));
        return -1;
    }
    crypt_slot_mac(header, slot, derived + 64, mac);
    ok = crypt_ct_equal(mac, s + 0x68, CRUG_CRYPT_MAC_BYTES);
    memset(mac, 0, sizeof(mac));
    if (!ok) {
        memset(derived, 0, sizeof(derived));
        return -1;
    }
    crypt_slot_tweak(s, slot, tweak);
    memcpy(master, s + 0x28, CRUG_CRYPT_MASTER_BYTES);
    {
        struct aes256_xts_key key;

        /* aes256_xts_* work in place: master must hold the ciphertext first. */
        aes256_xts_set_key(&key, derived);
        aes256_xts_decrypt(&key, tweak, master, CRUG_CRYPT_MASTER_BYTES);
        aes256_xts_wipe(&key);
    }
    memset(derived, 0, sizeof(derived));
    return 0;
}
