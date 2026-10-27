/* crypt.device: transparent AES-256-XTS block device over a GPT partition.
 *
 * The first 32 sectors of the parent partition hold the plaintext crypt header
 * (see include/crug/crypt.h); data sector N of this device is parent sector
 * 32 + N, and N is the XTS data-unit index. Writes are encrypted into a bounce
 * buffer so callers keep ownership of their buffers.
 */
#include <crug/aes.h>
#include <crug/block.h>
#include <crug/crypt.h>
#include <crug/heap.h>
#include <crug/kernel.h>
#include <crug/mm.h>
#include <crug/string.h>
#include <crug/xts.h>

#define CRYPT_BOUNCE_SECTORS 32u
#define CRYPT_BOUNCE_BYTES (CRYPT_BOUNCE_SECTORS * CRUG_SECTOR_SIZE)
#define CRYPT_MAX_DEVICES 4u

struct crypt_priv {
    struct block_device *parent;
    u64 offset;
    u8 *bounce;
    struct aes256_xts_key key;
    u32 wiped;
};

static const char *crypt_name(u32 index)
{
    static const char *names[CRYPT_MAX_DEVICES] = { "crypt0", "crypt1", "crypt2", "crypt3" };

    return index < CRYPT_MAX_DEVICES ? names[index] : "crypt";
}

static void crypt_sector_tweak(u8 tweak[16], u64 sector)
{
    memset(tweak, 0, 16);
    aes256_xts_sector_tweak(tweak, sector);
}

static int crypt_read(struct block_device *bd, u64 lba, u32 count, void *buf)
{
    struct crypt_priv *p = (struct crypt_priv *)bd->priv;
    u32 i;

    if (count == 0) {
        return 0;
    }
    if (p->wiped || buf == 0 || lba > bd->sectors || count > bd->sectors - lba) {
        return -1;
    }
    if (block_read(p->parent, p->offset + lba, count, buf) != 0) {
        return -1;
    }
    for (i = 0; i < count; i++) {
        u8 tweak[16];

        crypt_sector_tweak(tweak, lba + i);
        aes256_xts_decrypt(&p->key, tweak, (u8 *)buf + (usize)i * CRUG_SECTOR_SIZE,
                           CRUG_SECTOR_SIZE);
    }
    return 0;
}

static int crypt_write(struct block_device *bd, u64 lba, u32 count, const void *buf)
{
    struct crypt_priv *p = (struct crypt_priv *)bd->priv;
    const u8 *src = (const u8 *)buf;

    if (count == 0) {
        return 0;
    }
    if (p->wiped || buf == 0 || lba > bd->sectors || count > bd->sectors - lba) {
        return -1;
    }
    while (count > 0) {
        u32 chunk = MIN(count, CRYPT_BOUNCE_SECTORS);
        u32 i;

        memcpy(p->bounce, src, (usize)chunk * CRUG_SECTOR_SIZE);
        for (i = 0; i < chunk; i++) {
            u8 tweak[16];

            crypt_sector_tweak(tweak, lba + i);
            aes256_xts_encrypt(&p->key, tweak, p->bounce + (usize)i * CRUG_SECTOR_SIZE,
                               CRUG_SECTOR_SIZE);
        }
        if (block_write(p->parent, p->offset + lba, chunk, p->bounce) != 0) {
            return -1;
        }
        lba += chunk;
        src += (usize)chunk * CRUG_SECTOR_SIZE;
        count -= chunk;
    }
    return 0;
}

static int crypt_flush(struct block_device *bd)
{
    struct crypt_priv *p = (struct crypt_priv *)bd->priv;

    if (p->wiped)
        return -1;
    return block_flush(p->parent);
}

struct block_device *crypt_unlock(struct block_device *parent, u64 offset_sectors,
                                  const u8 *password, usize password_len)
{
    struct crug_crypt_header *h;
    struct crypt_priv *p;
    struct block_device *bd;
    u8 *header;
    void *scratch;
    u64 scratch_bytes;
    u64 pages;
    u64 pa;
    u8 master[CRUG_CRYPT_MASTER_BYTES];
    u32 slot;
    u32 found = CRUG_CRYPT_SLOT_COUNT;
    u64 volume_sectors;
    u32 m_cost, t_cost, lanes;
    static u32 device_count;

    if (parent == 0 || password == 0 || password_len == 0 ||
        parent->sectors < offset_sectors + CRUG_CRYPT_HEADER_SECTORS) {
        return 0;
    }
    aes_init();
    header = (u8 *)kmalloc(CRUG_CRYPT_HEADER_BYTES);
    if (header == 0) {
        kprintf("CRUGOS: crypt unlock failed (no memory for header)\n");
        return 0;
    }
    if (block_read(parent, offset_sectors, CRUG_CRYPT_HEADER_SECTORS, header) != 0) {
        kfree(header);
        kprintf("CRUGOS: crypt unlock failed (header read)\n");
        return 0;
    }
    h = (struct crug_crypt_header *)header;
    if (crypt_meta_validate(h) != 0) {
        kfree(header);
        kprintf("CRUGOS: crypt unlock failed (bad header)\n");
        return 0;
    }
    volume_sectors = h->volume_sectors;
    m_cost = h->kdf_m_cost_kib;
    t_cost = h->kdf_t_cost;
    lanes = h->kdf_lanes;
    scratch_bytes = crypt_meta_scratch_bytes(h);
    pages = (scratch_bytes + CRUG_PAGE_SIZE - 1) / CRUG_PAGE_SIZE;
    pa = pmm_alloc_pages(pages);
    if (pa == 0) {
        kfree(header);
        kprintf("CRUGOS: crypt unlock failed (%llu KiB KDF memory)\n", scratch_bytes / 1024);
        return 0;
    }
    scratch = pmm_phys_to_virt(pa);
    for (slot = 0; slot < h->slot_count; slot++) {
        if (crypt_meta_unseal_slot(header, slot, password, password_len, master, scratch,
                                   (usize)scratch_bytes) == 0) {
            found = slot;
            break;
        }
    }
    memset(scratch, 0, (usize)scratch_bytes);
    pmm_free_pages(pa, pages);
    if (found == CRUG_CRYPT_SLOT_COUNT) {
        memset(master, 0, sizeof(master));
        memset(header, 0, CRUG_CRYPT_HEADER_BYTES);
        kfree(header);
        kprintf("CRUGOS: crypt unlock failed\n");
        return 0;
    }
    p = (struct crypt_priv *)kzalloc(sizeof(*p));
    bd = (struct block_device *)kzalloc(sizeof(*bd));
    if (p == 0 || bd == 0) {
        kfree(p);
        kfree(bd);
        memset(master, 0, sizeof(master));
        memset(header, 0, CRUG_CRYPT_HEADER_BYTES);
        kfree(header);
        kprintf("CRUGOS: crypt unlock failed (no memory for device)\n");
        return 0;
    }
    p->bounce = (u8 *)kmalloc(CRYPT_BOUNCE_BYTES);
    if (p->bounce == 0) {
        kfree(p);
        kfree(bd);
        memset(master, 0, sizeof(master));
        memset(header, 0, CRUG_CRYPT_HEADER_BYTES);
        kfree(header);
        kprintf("CRUGOS: crypt unlock failed (no memory for bounce buffer)\n");
        return 0;
    }
    p->parent = parent;
    p->offset = offset_sectors + CRUG_CRYPT_HEADER_SECTORS;
    aes256_xts_set_key(&p->key, master);
    memset(master, 0, sizeof(master));
    memset(header, 0, CRUG_CRYPT_HEADER_BYTES);
    kfree(header);

    strncpy(bd->name, crypt_name(device_count), BLOCK_NAME_MAX - 1);
    bd->sectors = volume_sectors - CRUG_CRYPT_HEADER_SECTORS;
    bd->sector_size = CRUG_SECTOR_SIZE;
    bd->read = crypt_read;
    bd->write = crypt_write;
    bd->flush = crypt_flush;
    bd->priv = p;
    if (block_register(bd) != 0) {
        memset(p->bounce, 0, CRYPT_BOUNCE_BYTES);
        kfree(p->bounce);
        aes256_xts_wipe(&p->key);
        kfree(p);
        kfree(bd);
        kprintf("CRUGOS: crypt unlock failed (device register)\n");
        return 0;
    }
    device_count++;
    kprintf("CRUGOS: crypt ready (%llu sectors, keyslot %u, Argon2id m=%u KiB t=%u p=%u)\n",
            bd->sectors, found, m_cost, t_cost, lanes);
    kprintf("CRUGOS: crypt aes: %s\n", aes_implementation_name());
    return bd;
}

void crypt_wipe_keys(struct block_device *bd)
{
    struct crypt_priv *p;

    if (bd == 0 || bd->priv == 0) {
        return;
    }
    p = (struct crypt_priv *)bd->priv;
    aes256_xts_wipe(&p->key);
    if (p->bounce != 0)
        memset(p->bounce, 0, CRYPT_BOUNCE_BYTES);
    p->wiped = 1;
}
