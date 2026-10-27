#include <crug/raid.h>
#include <crug/block.h>
#include <crug/gpt.h>
#include <crug/heap.h>
#include <crug/kernel.h>
#include <crug/string.h>

#define RAID_META_SECTOR_COUNT 2u
#define RAID_META_BLANK 0
#define RAID_META_VALID 1
#define RAID_REBUILD_SECTORS 64u

struct raid_meta {
    u32 magic;
    u16 version;
    u16 level;
    u32 member_count;
    u32 slot;
    u32 chunk_sectors;
    u32 state;
    u64 array_sectors;
    u64 member_sectors;
    u64 generation;
    u8 array_uuid[16];
    u8 member_uuid[16];
    u8 reserved[428];
    u32 checksum;
} __attribute__((packed));

_Static_assert(sizeof(struct raid_meta) == 512,
              "raid metadata must be exactly one sector");

struct raid_member {
    struct block_device *bd;
    u32 slot;
};

struct raid_array {
    struct block_device bd;
    u32 level;
    u32 member_count;
    u32 chunk_sectors;
    u32 present;
    u32 rebuilt;
    u32 failed_mask;
    u64 sectors;
    u64 generation;
    u8 array_uuid[16];
    struct raid_member members[RAID_MAX_MEMBERS];
};

static int raid_meta_validate(struct raid_meta *m, struct block_device *bd)
{
    u32 sum;
    u32 crc;

    if (!m || !bd)
        return -1;
    if (m->magic != RAID_META_MAGIC || m->version != RAID_META_VERSION)
        return -1;
    if (m->level != RAID_LEVEL_1 && m->level != RAID_LEVEL_10)
        return -1;
    if (m->level == RAID_LEVEL_1) {
        if (m->chunk_sectors != 0)
            return -1;
        if (m->member_count < 1u || m->member_count > RAID_MAX_MEMBERS)
            return -1;
    } else {
        if (m->chunk_sectors == 0)
            return -1;
        if (m->member_count < 4u || (m->member_count & 1u))
            return -1;
        if (m->member_count > RAID_MAX_MEMBERS)
            return -1;
    }
    if (m->state != 0)
        return -1;
    if (m->slot >= m->member_count)
        return -1;
    if (bd->sectors < RAID_META_SECTOR_COUNT)
        return -1;
    if (m->member_sectors != bd->sectors)
        return -1;
    if (m->array_sectors == 0 ||
        m->array_sectors > bd->sectors - RAID_META_SECTOR_COUNT)
        return -1;
    sum = m->checksum;
    m->checksum = 0;
    crc = crc32(m, sizeof(*m));
    m->checksum = sum;
    return crc == sum ? 0 : -1;
}

static int raid_meta_probe(struct block_device *bd, struct raid_meta *m)
{
    int blank = 0;
    u32 i;

    if (bd->sectors < RAID_META_SECTOR_COUNT)
        return -1;
    for (i = 0; i < 2; i++) {
        if (block_read(bd, bd->sectors - 1u - i, 1, m) != 0)
            return -1;
        if (m->magic == 0) {
            blank = 1;
            continue;
        }
        if (m->magic != RAID_META_MAGIC)
            return -1;
        return raid_meta_validate(m, bd) == 0 ? RAID_META_VALID : -1;
    }
    return blank ? RAID_META_BLANK : -1;
}

static int raid_meta_write(struct block_device *bd, struct raid_meta *m)
{
    if (bd->sectors < RAID_META_SECTOR_COUNT)
        return -1;
    m->checksum = 0;
    m->checksum = crc32(m, sizeof(*m));
    if (block_write(bd, bd->sectors - 1u, 1, m) != 0)
        return -1;
    if (block_write(bd, bd->sectors - 2u, 1, m) != 0)
        return -1;
    return 0;
}

static void raid_member_uuid(u8 out[16], const u8 array_uuid[16], u32 slot,
                             u64 generation, const char *name)
{
    u32 i;

    for (i = 0; i < 16; i++) {
        out[i] = array_uuid[i] ^ (u8)(0x5Au + slot * 31u + i * 17u) ^
                 (u8)(generation >> ((i % 8u) * 8u));
        if (i < 8u && name[i])
            out[i] ^= (u8)name[i];
    }
    out[15] |= 1u;
}

static int raid_copy_range(struct block_device *src, struct block_device *dst,
                           u64 lba, u64 sectors)
{
    static u8 buffer[RAID_REBUILD_SECTORS * CRUG_SECTOR_SIZE];

    while (sectors > 0) {
        u32 chunk = sectors > RAID_REBUILD_SECTORS
                        ? RAID_REBUILD_SECTORS
                        : (u32)sectors;

        if (block_read(src, lba, chunk, buffer) != 0)
            return -1;
        if (block_write(dst, lba, chunk, buffer) != 0)
            return -1;
        lba += chunk;
        sectors -= chunk;
    }
    return 0;
}

static struct block_device *raid_rebuild_source(struct raid_array *a, u32 slot)
{
    u32 pair = slot >> 1;
    u32 i;

    for (i = 0; i < a->present; i++) {
        if (a->level == RAID_LEVEL_10 && (a->members[i].slot >> 1) != pair)
            continue;
        return a->members[i].bd;
    }
    return 0;
}

static int raid_rebuild_member(struct raid_array *a, struct block_device *dst,
                               u32 slot)
{
    struct block_device *src;
    struct raid_meta meta;
    u64 lba;
    u64 step;

    if (dst->sectors < a->sectors + RAID_META_SECTOR_COUNT)
        return -1;
    src = raid_rebuild_source(a, slot);
    if (!src)
        return -1;
    if (a->level == RAID_LEVEL_10) {
        u32 pair = slot >> 1;
        u32 pairs = a->member_count / 2u;
        u64 chunk = a->chunk_sectors;

        for (lba = (u64)pair * chunk; lba < a->sectors;
             lba += (u64)pairs * chunk) {
            step = a->sectors - lba;
            if (step > chunk)
                step = chunk;
            if (raid_copy_range(src, dst, lba, step) != 0)
                return -1;
        }
    } else if (raid_copy_range(src, dst, 0, a->sectors) != 0) {
        return -1;
    }
    if (block_flush(dst) != 0)
        return -1;
    memset(&meta, 0, sizeof(meta));
    meta.magic = RAID_META_MAGIC;
    meta.version = RAID_META_VERSION;
    meta.level = a->level;
    meta.member_count = a->member_count;
    meta.slot = slot;
    meta.chunk_sectors = a->chunk_sectors;
    meta.state = 0;
    meta.array_sectors = a->sectors;
    meta.member_sectors = dst->sectors;
    meta.generation = a->generation;
    memcpy(meta.array_uuid, a->array_uuid, 16);
    raid_member_uuid(meta.member_uuid, a->array_uuid, slot, a->generation,
                     dst->name);
    if (raid_meta_write(dst, &meta) != 0)
        return -1;
    if (block_flush(dst) != 0)
        return -1;
    a->members[a->present].bd = dst;
    a->members[a->present].slot = slot;
    a->present++;
    a->rebuilt++;
    kprintf("raid   : rebuilt %s as slot %u\n", dst->name, slot);
    return 0;
}

static u32 raid_pair_for(const struct raid_array *a, u64 lba)
{
    return (u32)((lba / a->chunk_sectors) % (a->member_count / 2u));
}

static int raid10_read(struct raid_array *a, u64 lba, u32 count, void *buf)
{
    u8 *dst = (u8 *)buf;
    u32 off;

    for (off = 0; off < count; off++) {
        u32 pair = raid_pair_for(a, lba + off);
        int ok = 0;
        u32 i;

        for (i = 0; i < a->present; i++) {
            struct raid_member *m = &a->members[i];

            if ((m->slot >> 1) != pair)
                continue;
            if (a->failed_mask & (1u << i))
                continue;
            if (block_read(m->bd, lba + off, 1,
                           dst + (usize)off * CRUG_SECTOR_SIZE) == 0) {
                ok = 1;
                break;
            }
            a->failed_mask |= 1u << i;
            kprintf("raid   : %s failed at lba %llu\n",
                    m->bd->name, lba + off);
        }
        if (!ok) {
            kprintf("raid   : no member for mirror pair %u at lba %llu\n",
                    pair, lba + off);
            return -1;
        }
    }
    return 0;
}

static int raid_read(struct block_device *bd, u64 lba, u32 count, void *buf)
{
    struct raid_array *a = (struct raid_array *)bd->priv;
    u32 i;

    if (!a)
        return -1;
    if (a->level == RAID_LEVEL_10)
        return raid10_read(a, lba, count, buf);
    for (i = 0; i < a->present; i++) {
        struct raid_member *m = &a->members[i];

        if (a->failed_mask & (1u << i))
            continue;
        if (block_read(m->bd, lba, count, buf) == 0)
            return 0;
        a->failed_mask |= 1u << i;
        kprintf("raid   : %s failed at lba %llu\n", m->bd->name, lba);
    }
    return -1;
}

static int raid10_write(struct raid_array *a, u64 lba, u32 count,
                        const void *buf)
{
    const u8 *src = (const u8 *)buf;
    u32 off;

    for (off = 0; off < count; off++) {
        u32 pair = raid_pair_for(a, lba + off);
        int ok = 0;
        u32 i;

        for (i = 0; i < a->present; i++) {
            struct raid_member *m = &a->members[i];

            if ((m->slot >> 1) != pair)
                continue;
            if (a->failed_mask & (1u << i))
                continue;
            if (block_write(m->bd, lba + off, 1,
                            src + (usize)off * CRUG_SECTOR_SIZE) == 0) {
                ok = 1;
                continue;
            }
            a->failed_mask |= 1u << i;
            kprintf("raid   : %s write failed at lba %llu\n",
                    m->bd->name, lba + off);
        }
        if (!ok) {
            kprintf("raid   : no writable member for pair %u at lba %llu\n",
                    pair, lba + off);
            return -1;
        }
    }
    return 0;
}

static int raid_write(struct block_device *bd, u64 lba, u32 count,
                      const void *buf)
{
    struct raid_array *a = (struct raid_array *)bd->priv;
    u32 i;
    int ok = 0;

    if (!a)
        return -1;
    if (a->level == RAID_LEVEL_10)
        return raid10_write(a, lba, count, buf);
    for (i = 0; i < a->present; i++) {
        struct raid_member *m = &a->members[i];

        if (a->failed_mask & (1u << i))
            continue;
        if (block_write(m->bd, lba, count, buf) == 0) {
            ok = 1;
            continue;
        }
        a->failed_mask |= 1u << i;
        kprintf("raid   : %s write failed at lba %llu\n", m->bd->name, lba);
    }
    return ok ? 0 : -1;
}

static int raid_flush(struct block_device *bd)
{
    struct raid_array *a = (struct raid_array *)bd->priv;
    u32 i;
    int ok = 0;

    if (!a)
        return -1;
    for (i = 0; i < a->present; i++) {
        if (block_flush(a->members[i].bd) == 0)
            ok = 1;
    }
    return ok ? 0 : -1;
}

int raid_assemble(struct block_device **members, u32 count, const char *name,
                  u32 *present, int *degraded)
{
    struct raid_array *a;
    struct raid_meta first;
    struct raid_meta m;
    u32 slots = 0;
    u32 missing;
    int spare[RAID_MAX_MEMBERS];
    u32 spares = 0;
    u32 i;
    u32 j;
    int rc;

    if (!members || !name || count == 0 || count > RAID_MAX_MEMBERS)
        return -1;
    a = (struct raid_array *)kzalloc(sizeof(*a));
    if (!a)
        return -1;
    memset(&first, 0, sizeof(first));
    for (i = 0; i < count; i++) {
        if (!members[i])
            continue;
        rc = raid_meta_probe(members[i], &m);
        if (rc < 0) {
            kprintf("raid   : %s metadata invalid\n", members[i]->name);
            kfree(a);
            return -1;
        }
        if (rc == RAID_META_BLANK) {
            if (spares < RAID_MAX_MEMBERS)
                spare[spares++] = (int)i;
            continue;
        }
        if (a->present == 0) {
            first = m;
            a->level = first.level;
            a->member_count = first.member_count;
            a->chunk_sectors = first.chunk_sectors;
            a->sectors = first.array_sectors;
            a->generation = first.generation;
            memcpy(a->array_uuid, first.array_uuid, 16);
        } else {
            if (memcmp(m.array_uuid, a->array_uuid, 16) != 0 ||
                m.member_count != a->member_count ||
                m.level != a->level ||
                m.chunk_sectors != a->chunk_sectors ||
                m.array_sectors != a->sectors ||
                m.generation != a->generation) {
                kprintf("raid   : %s does not match the array geometry\n",
                        members[i]->name);
                kfree(a);
                return -1;
            }
        }
        if (slots & (1u << m.slot)) {
            kprintf("raid   : duplicate member slot %u\n", m.slot);
            kfree(a);
            return -1;
        }
        slots |= 1u << m.slot;
        a->members[a->present].bd = members[i];
        a->members[a->present].slot = m.slot;
        a->present++;
    }
    if (a->present == 0 || a->member_count == 0) {
        kprintf("raid   : no member with valid metadata\n");
        kfree(a);
        return -1;
    }
    if (a->level == RAID_LEVEL_10) {
        for (j = 0; j < a->member_count / 2u; j++) {
            int found = 0;

            for (i = 0; i < a->present; i++) {
                if ((a->members[i].slot >> 1) == j)
                    found = 1;
            }
            if (!found) {
                kprintf("raid   : mirror pair %u has no member\n", j);
                kfree(a);
                return -1;
            }
        }
    }
    missing = ((1u << a->member_count) - 1u) & ~slots;
    for (i = 0; i < spares && missing != 0; i++) {
        u32 slot = 0;

        while (slot < a->member_count && !(missing & (1u << slot)))
            slot++;
        if (slot >= a->member_count)
            break;
        if (raid_rebuild_member(a, members[spare[i]], slot) != 0)
            kprintf("raid   : rebuild of %s failed\n", members[spare[i]]->name);
        missing &= ~(1u << slot);
    }
    for (; i < spares; i++)
        kprintf("raid   : spare %s has no missing slot\n", members[spare[i]]->name);
    strncpy(a->bd.name, name, BLOCK_NAME_MAX - 1);
    a->bd.sectors = a->sectors;
    a->bd.sector_size = CRUG_SECTOR_SIZE;
    a->bd.read = raid_read;
    a->bd.write = raid_write;
    a->bd.flush = raid_flush;
    a->bd.priv = a;
    if (block_register(&a->bd) != 0) {
        kfree(a);
        return -1;
    }
    if (present)
        *present = a->present;
    if (degraded)
        *degraded = a->present != a->member_count || a->failed_mask != 0;
    return 0;
}

int raid_is_degraded(struct block_device *bd)
{
    struct raid_array *a;

    if (!bd || !bd->priv)
        return 1;
    a = (struct raid_array *)bd->priv;
    return a->present != a->member_count || a->failed_mask != 0;
}

u32 raid_rebuilt_members(struct block_device *bd)
{
    struct raid_array *a;

    if (!bd || !bd->priv)
        return 0;
    a = (struct raid_array *)bd->priv;
    return a->rebuilt;
}

u32 raid_member_count(struct block_device *bd)
{
    struct raid_array *a;

    if (!bd || !bd->priv)
        return 0;
    a = (struct raid_array *)bd->priv;
    return a->member_count;
}

u32 raid_level(struct block_device *bd)
{
    struct raid_array *a;

    if (!bd || !bd->priv)
        return 0;
    a = (struct raid_array *)bd->priv;
    return a->level;
}

u32 raid_chunk_sectors(struct block_device *bd)
{
    struct raid_array *a;

    if (!bd || !bd->priv)
        return 0;
    a = (struct raid_array *)bd->priv;
    return a->chunk_sectors;
}

u32 raid_pairs(struct block_device *bd)
{
    struct raid_array *a;

    if (!bd || !bd->priv)
        return 0;
    a = (struct raid_array *)bd->priv;
    return a->level == RAID_LEVEL_10 ? a->member_count / 2u : 1u;
}
