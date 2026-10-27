#include <crug/raid.h>
#include <crug/block.h>
#include <crug/gpt.h>
#include <crug/heap.h>
#include <crug/string.h>
#include <crug/kernel.h>

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
    u32 member_count;
    u32 present;
    u32 failed_mask;
    u64 sectors;
    u64 generation;
    u8 uuid[16];
    struct raid_member members[RAID_MAX_MEMBERS];
};

static int raid_meta_validate(const u8 *raw, struct block_device *bd,
                              struct raid_meta *out)
{
    struct raid_meta meta;
    u8 copy[CRUG_SECTOR_SIZE];
    u32 stored;

    if (!raw || !bd || !out || bd->sectors < 3)
        return -1;
    memcpy(&meta, raw, sizeof(meta));
    if (meta.magic != RAID_META_MAGIC || meta.version != RAID_META_VERSION)
        return -1;
    if (meta.level != RAID_LEVEL_1 || meta.chunk_sectors != 0 || meta.state != 0)
        return -1;
    if (meta.member_count == 0 || meta.member_count > RAID_MAX_MEMBERS)
        return -1;
    if (meta.slot >= meta.member_count)
        return -1;
    if (meta.member_sectors != bd->sectors)
        return -1;
    if (meta.array_sectors == 0 || meta.array_sectors > bd->sectors - 2u)
        return -1;
    memcpy(copy, raw, CRUG_SECTOR_SIZE);
    memcpy(&stored, copy + CRUG_SECTOR_SIZE - sizeof(stored), sizeof(stored));
    memset(copy + CRUG_SECTOR_SIZE - sizeof(stored), 0, sizeof(stored));
    if (crc32(copy, CRUG_SECTOR_SIZE) != stored)
        return -1;
    *out = meta;
    return 0;
}

static int raid_meta_read(struct block_device *bd, struct raid_meta *out)
{
    u8 raw[CRUG_SECTOR_SIZE];

    if (block_read(bd, bd->sectors - 1u, 1, raw) == 0 &&
        raid_meta_validate(raw, bd, out) == 0)
        return 0;
    if (block_read(bd, bd->sectors - 2u, 1, raw) == 0 &&
        raid_meta_validate(raw, bd, out) == 0)
        return 0;
    return -1;
}

static int raid_read(struct block_device *bd, u64 lba, u32 count, void *buf)
{
    struct raid_array *a = (struct raid_array *)bd->priv;
    u32 i;

    for (i = 0; i < a->present; i++) {
        if (a->failed_mask & (1u << i))
            continue;
        if (block_read(a->members[i].bd, lba, count, buf) == 0)
            return 0;
        a->failed_mask |= 1u << i;
        kprintf("raid   : member %u read failed, marking degraded\n",
                a->members[i].slot);
    }
    return -1;
}

static int raid_write(struct block_device *bd, u64 lba, u32 count,
                      const void *buf)
{
    struct raid_array *a = (struct raid_array *)bd->priv;
    u32 i;
    u32 ok = 0;

    for (i = 0; i < a->present; i++) {
        if (a->failed_mask & (1u << i))
            continue;
        if (block_write(a->members[i].bd, lba, count, buf) == 0) {
            ok++;
            continue;
        }
        a->failed_mask |= 1u << i;
        kprintf("raid   : member %u write failed, marking degraded\n",
                a->members[i].slot);
    }
    return ok ? 0 : -1;
}

static int raid_flush(struct block_device *bd)
{
    struct raid_array *a = (struct raid_array *)bd->priv;
    u32 i;
    u32 ok = 0;

    for (i = 0; i < a->present; i++) {
        if (a->failed_mask & (1u << i))
            continue;
        if (block_flush(a->members[i].bd) == 0) {
            ok++;
            continue;
        }
        a->failed_mask |= 1u << i;
        kprintf("raid   : member %u flush failed, marking degraded\n",
                a->members[i].slot);
    }
    return ok ? 0 : -1;
}

int raid_assemble(struct block_device **members, u32 count, const char *name,
                  u32 *present, int *degraded)
{
    struct raid_array *a;
    struct raid_meta first;
    u32 slots = 0;
    u32 i;
    int have_first = 0;

    if (!members || !name || count == 0 || count > RAID_MAX_MEMBERS)
        return -1;
    a = (struct raid_array *)kzalloc(sizeof(*a));
    if (!a)
        return -1;
    for (i = 0; i < count; i++) {
        struct raid_meta meta;

        if (!members[i] || raid_meta_read(members[i], &meta) != 0) {
            kprintf("raid   : member %u has no valid metadata\n", i);
            kfree(a);
            return -1;
        }
        if (!have_first) {
            first = meta;
            have_first = 1;
        } else if (memcmp(first.array_uuid, meta.array_uuid, 16) != 0 ||
                   first.member_count != meta.member_count ||
                   first.array_sectors != meta.array_sectors ||
                   first.generation != meta.generation) {
            kprintf("raid   : member %u metadata mismatch\n", i);
            kfree(a);
            return -1;
        }
        if (slots & (1u << meta.slot)) {
            kprintf("raid   : duplicate member slot %u\n", meta.slot);
            kfree(a);
            return -1;
        }
        slots |= 1u << meta.slot;
        a->members[a->present].bd = members[i];
        a->members[a->present].slot = meta.slot;
        a->present++;
    }
    a->member_count = first.member_count;
    a->sectors = first.array_sectors;
    a->generation = first.generation;
    memcpy(a->uuid, first.array_uuid, 16);
    a->bd.sectors = a->sectors;
    a->bd.sector_size = CRUG_SECTOR_SIZE;
    a->bd.read = raid_read;
    a->bd.write = raid_write;
    a->bd.flush = raid_flush;
    a->bd.priv = a;
    strncpy(a->bd.name, name, BLOCK_NAME_MAX - 1);
    if (block_register(&a->bd) != 0) {
        kfree(a);
        return -1;
    }
    if (present)
        *present = a->present;
    if (degraded)
        *degraded = a->present < a->member_count;
    return 0;
}

int raid_is_degraded(struct block_device *bd)
{
    struct raid_array *a;

    if (!bd || !bd->priv)
        return 1;
    a = (struct raid_array *)bd->priv;
    return a->present < a->member_count || a->failed_mask != 0;
}

u32 raid_member_count(struct block_device *bd)
{
    struct raid_array *a;

    if (!bd || !bd->priv)
        return 0;
    a = (struct raid_array *)bd->priv;
    return a->member_count;
}
