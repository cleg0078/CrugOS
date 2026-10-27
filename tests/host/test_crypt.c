/* Host tests for crypt.device metadata: header layout, keyslot sealing/opening, MACs. */
#include "test.h"
#include <crug/crypt.h>
#include <crug/string.h>

static u8 header[CRUG_CRYPT_HEADER_BYTES];
static u8 scratch[256 * 1024];

static void fill_bytes(u8 *p, usize n, u8 seed)
{
    usize i;

    for (i = 0; i < n; i++) {
        p[i] = (u8)(seed + i * 7u + 3u);
    }
}

void test_crypt_functions(void)
{
    u8 salt[32];
    u8 volume_id[16];
    u8 nonce[32];
    u8 nonce2[32];
    u8 nonce3[32];
    u8 master[64];
    u8 out[64];
    u8 password[] = "correct horse battery";
    u8 recovery[] = "recovery passphrase";
    struct crug_crypt_header *h = (struct crug_crypt_header *)header;
    usize scratch_bytes;
    u32 i;

    fill_bytes(salt, sizeof(salt), 1);
    fill_bytes(volume_id, sizeof(volume_id), 2);
    fill_bytes(nonce, sizeof(nonce), 3);
    fill_bytes(nonce2, sizeof(nonce2), 5);
    fill_bytes(nonce3, sizeof(nonce3), 6);
    fill_bytes(master, sizeof(master), 4);

    CHECK(crypt_meta_format_header(header, 4096, 64, 1, 1, salt, volume_id) == 0);
    CHECK(memcmp(h->magic, CRUG_CRYPT_MAGIC, CRUG_CRYPT_MAGIC_LEN) == 0);
    CHECK(h->version == CRUG_CRYPT_VERSION);
    CHECK(h->header_bytes == CRUG_CRYPT_HEADER_BYTES);
    CHECK(h->cipher == CRUG_CRYPT_CIPHER_AES256_XTS);
    CHECK(h->kdf == CRUG_CRYPT_KDF_ARGON2ID);
    CHECK(h->data_offset_sectors == CRUG_CRYPT_HEADER_SECTORS);
    CHECK(h->volume_sectors == 4096);
    CHECK(h->slot_count == CRUG_CRYPT_SLOT_COUNT);
    CHECK(h->slot_bytes == CRUG_CRYPT_SLOT_BYTES);
    CHECK(h->slots_offset == CRUG_CRYPT_SLOTS_OFFSET);
    CHECK(memcmp(h->salt, salt, sizeof(salt)) == 0);
    CHECK(memcmp(h->volume_id, volume_id, sizeof(volume_id)) == 0);
    CHECK(crypt_meta_validate(h) == 0);
    scratch_bytes = crypt_meta_scratch_bytes(h);
    CHECK(scratch_bytes == 64u * 1024u);
    CHECK(scratch_bytes <= sizeof(scratch));

    /* Rejected headers. */
    CHECK(crypt_meta_format_header(header, 32, 64, 1, 1, salt, volume_id) != 0);
    CHECK(crypt_meta_format_header(header, 4096, 7, 1, 1, salt, volume_id) != 0);
    CHECK(crypt_meta_format_header(header, 4096, 64, 0, 1, salt, volume_id) != 0);
    CHECK(crypt_meta_format_header(header, 4096, 64, 1, 0, salt, volume_id) != 0);
    CHECK(crypt_meta_format_header(header, 4096, 65, 1, 17, salt, volume_id) != 0);
    CHECK(crypt_meta_format_header(header, 4096, 63, 1, 8, salt, volume_id) != 0);
    CHECK(crypt_meta_format_header(header, 4096, 64, 1, 1, salt, volume_id) == 0);

    /* Seal and open slot 0. */
    CHECK(crypt_meta_seal_slot(header, 0, CRUG_CRYPT_SLOT_FLAG_PASSWORD, nonce, password,
                               sizeof(password) - 1, master, scratch, scratch_bytes) == 0);
    {
        const u8 *s = header + CRUG_CRYPT_SLOTS_OFFSET;

        CHECK((s[0] | (s[1] << 8)) == 1);
        CHECK(memcmp(s + 8, nonce, sizeof(nonce)) == 0);
    }
    memset(out, 0, sizeof(out));
    CHECK(crypt_meta_unseal_slot(header, 0, password, sizeof(password) - 1, out, scratch,
                                 scratch_bytes) == 0);
    CHECK(memcmp(out, master, sizeof(master)) == 0);

    /* Wrong password, bad slot, short scratch. */
    CHECK(crypt_meta_unseal_slot(header, 0, recovery, sizeof(recovery) - 1, out, scratch,
                                 scratch_bytes) != 0);
    CHECK(crypt_meta_unseal_slot(header, CRUG_CRYPT_SLOT_COUNT, password, sizeof(password) - 1,
                                 out, scratch, scratch_bytes) != 0);
    CHECK(crypt_meta_unseal_slot(header, 0, password, sizeof(password) - 1, out, scratch,
                                 scratch_bytes - 1) != 0);

    /* A second, independent slot with the recovery passphrase. */
    CHECK(crypt_meta_seal_slot(header, 1, CRUG_CRYPT_SLOT_FLAG_RECOVERY, nonce2, recovery,
                               sizeof(recovery) - 1, master, scratch, scratch_bytes) == 0);
    memset(out, 0, sizeof(out));
    CHECK(crypt_meta_unseal_slot(header, 1, recovery, sizeof(recovery) - 1, out, scratch,
                                 scratch_bytes) == 0);
    CHECK(memcmp(out, master, sizeof(master)) == 0);
    CHECK(crypt_meta_unseal_slot(header, 1, password, sizeof(password) - 1, out, scratch,
                                 scratch_bytes) != 0);

    /* Tampering with the header, the slot MAC or the sealed master fails closed. */
    {
        u8 saved[8];

        memcpy(saved, header + 0x40, sizeof(saved));
        header[0x40] ^= 0x01;
        CHECK(crypt_meta_unseal_slot(header, 0, password, sizeof(password) - 1, out, scratch,
                                     scratch_bytes) != 0);
        memcpy(header + 0x40, saved, sizeof(saved));
        CHECK(crypt_meta_unseal_slot(header, 0, password, sizeof(password) - 1, out, scratch,
                                     scratch_bytes) == 0);

        header[CRUG_CRYPT_SLOTS_OFFSET + 0x28] ^= 0x80;
        CHECK(crypt_meta_unseal_slot(header, 0, password, sizeof(password) - 1, out, scratch,
                                     scratch_bytes) != 0);
        header[CRUG_CRYPT_SLOTS_OFFSET + 0x28] ^= 0x80;

        header[CRUG_CRYPT_SLOTS_OFFSET + 0x68] ^= 0x01;
        CHECK(crypt_meta_unseal_slot(header, 0, password, sizeof(password) - 1, out, scratch,
                                     scratch_bytes) != 0);
        header[CRUG_CRYPT_SLOTS_OFFSET + 0x68] ^= 0x01;

        header[CRUG_CRYPT_SLOTS_OFFSET + 0x00] = 0;
        CHECK(crypt_meta_unseal_slot(header, 0, password, sizeof(password) - 1, out, scratch,
                                     scratch_bytes) != 0);
        header[CRUG_CRYPT_SLOTS_OFFSET + 0x00] = 1;
        CHECK(crypt_meta_unseal_slot(header, 0, password, sizeof(password) - 1, out, scratch,
                                     scratch_bytes) == 0);

        header[0x70] ^= 0x04;
        CHECK(crypt_meta_unseal_slot(header, 0, password, sizeof(password) - 1, out, scratch,
                                     scratch_bytes) != 0);
        header[0x70] ^= 0x04;
    }

    /* Re-sealing a slot with a new passphrase retires the old one. */
    {
        u8 master2[64];
        u8 other[] = "a different passphrase";

        fill_bytes(master2, sizeof(master2), 9);
        CHECK(crypt_meta_seal_slot(header, 2, CRUG_CRYPT_SLOT_FLAG_PASSWORD, nonce3, other,
                                   sizeof(other) - 1, master2, scratch, scratch_bytes) == 0);
        CHECK(crypt_meta_unseal_slot(header, 2, other, sizeof(other) - 1, out, scratch,
                                     scratch_bytes) == 0);
        CHECK(memcmp(out, master2, sizeof(master2)) == 0);
        CHECK(crypt_meta_unseal_slot(header, 2, password, sizeof(password) - 1, out, scratch,
                                     scratch_bytes) != 0);
    }

    /* Odd-sized or over-long passphrases are rejected, not truncated. */
    CHECK(crypt_meta_unseal_slot(header, 0, password, 0, out, scratch, scratch_bytes) != 0);
    {
        u8 long_pass[CRUG_CRYPT_MAX_PASSWORD + 1];

        memset(long_pass, 0x5a, sizeof(long_pass));
        CHECK(crypt_meta_unseal_slot(header, 0, long_pass, sizeof(long_pass), out, scratch,
                                     scratch_bytes) != 0);
    }

    /* Every active slot still opens the original volume master key. */
    for (i = 0; i < 3; i++) {
        const u8 *s = header + CRUG_CRYPT_SLOTS_OFFSET + (usize)i * CRUG_CRYPT_SLOT_BYTES;
        u32 state = (u32)s[0] | ((u32)s[1] << 8) | ((u32)s[2] << 16) | ((u32)s[3] << 24);

        CHECK(state == CRUG_CRYPT_SLOT_ACTIVE);
    }

    memset(master, 0, sizeof(master));
    memset(out, 0, sizeof(out));
    memset(header, 0, sizeof(header));
}
