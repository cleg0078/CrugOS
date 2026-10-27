#include <crug/block.h>
#include <crug/heap.h>
#include <crug/string.h>
#include <crug/kernel.h>

static struct block_device *g_devices;
static u64 g_device_count;

void block_init(void)
{
    g_devices = 0;
    g_device_count = 0;
}

int block_register(struct block_device *bd)
{
    struct block_device *it;

    if (!bd || !bd->read || bd->sector_size != CRUG_SECTOR_SIZE)
        return -1;
    for (it = g_devices; it; it = it->next) {
        if (strcmp(it->name, bd->name) == 0)
            return -1;
    }
    bd->next = g_devices;
    g_devices = bd;
    g_device_count++;
    return 0;
}

struct block_device *block_find(const char *name)
{
    struct block_device *it;

    for (it = g_devices; it; it = it->next) {
        if (strcmp(it->name, name) == 0)
            return it;
    }
    return 0;
}

struct block_device *block_first(void)
{
    return g_devices;
}

u64 block_device_count(void)
{
    return g_device_count;
}

int block_read(struct block_device *bd, u64 lba, u32 count, void *buf)
{
    if (!bd || !bd->read || !buf)
        return -1;
    if (lba > bd->sectors || count > bd->sectors - lba)
        return -1;
    return bd->read(bd, lba, count, buf);
}

int block_write(struct block_device *bd, u64 lba, u32 count, const void *buf)
{
    if (!bd || !bd->write || !buf)
        return -1;
    if (lba > bd->sectors || count > bd->sectors - lba)
        return -1;
    return bd->write(bd, lba, count, buf);
}

int block_flush(struct block_device *bd)
{
    if (!bd)
        return -1;
    return bd->flush ? bd->flush(bd) : 0;
}

struct partition_priv {
    struct block_device *parent;
    u64 first_lba;
};

static int partition_read(struct block_device *bd, u64 lba, u32 count, void *buf)
{
    struct partition_priv *p = (struct partition_priv *)bd->priv;
    return block_read(p->parent, p->first_lba + lba, count, buf);
}

static int partition_write(struct block_device *bd, u64 lba, u32 count,
                           const void *buf)
{
    struct partition_priv *p = (struct partition_priv *)bd->priv;
    return block_write(p->parent, p->first_lba + lba, count, buf);
}

static int partition_flush(struct block_device *bd)
{
    struct partition_priv *p = (struct partition_priv *)bd->priv;
    return block_flush(p->parent);
}

struct block_device *block_partition(struct block_device *parent, u64 first_lba,
                                     u64 sector_count, const char *name)
{
    struct block_device *bd;
    struct partition_priv *p;

    if (!parent || sector_count == 0)
        return 0;
    if (first_lba > parent->sectors ||
        sector_count > parent->sectors - first_lba)
        return 0;
    bd = (struct block_device *)kzalloc(sizeof(*bd));
    p = (struct partition_priv *)kzalloc(sizeof(*p));
    if (!bd || !p) {
        kfree(bd);
        kfree(p);
        return 0;
    }
    p->parent = parent;
    p->first_lba = first_lba;
    strncpy(bd->name, name, BLOCK_NAME_MAX - 1);
    bd->sectors = sector_count;
    bd->sector_size = CRUG_SECTOR_SIZE;
    bd->read = partition_read;
    bd->write = parent->write ? partition_write : 0;
    bd->flush = parent->flush ? partition_flush : 0;
    bd->priv = p;
    if (block_register(bd) != 0) {
        kfree(p);
        kfree(bd);
        return 0;
    }
    return bd;
}

struct ram_disk {
    u8 data[8u * CRUG_SECTOR_SIZE];
};

static int ram_read(struct block_device *bd, u64 lba, u32 count, void *buf)
{
    struct ram_disk *r = (struct ram_disk *)bd->priv;
    memcpy(buf, r->data + lba * CRUG_SECTOR_SIZE, count * CRUG_SECTOR_SIZE);
    return 0;
}

static int ram_write(struct block_device *bd, u64 lba, u32 count,
                     const void *buf)
{
    struct ram_disk *r = (struct ram_disk *)bd->priv;
    memcpy(r->data + lba * CRUG_SECTOR_SIZE, buf, count * CRUG_SECTOR_SIZE);
    return 0;
}

int block_selftest(void)
{
    static struct ram_disk ram;
    struct block_device bd;
    struct block_device *part;
    u8 buf[CRUG_SECTOR_SIZE];
    u8 out[CRUG_SECTOR_SIZE];
    u32 i;

    memset(&ram, 0, sizeof(ram));
    memset(&bd, 0, sizeof(bd));
    strncpy(bd.name, "ramtest", BLOCK_NAME_MAX - 1);
    bd.sectors = 8;
    bd.sector_size = CRUG_SECTOR_SIZE;
    bd.read = ram_read;
    bd.write = ram_write;
    bd.priv = &ram;
    if (block_read(&bd, 0, 1, buf) != 0)
        return 0;
    for (i = 0; i < CRUG_SECTOR_SIZE; i++)
        buf[i] = (u8)i;
    if (block_write(&bd, 3, 1, buf) != 0)
        return 0;
    part = block_partition(&bd, 2, 4, "rampart");
    if (!part)
        return 0;
    if (block_read(part, 1, 1, out) != 0)
        return 0;
    for (i = 0; i < CRUG_SECTOR_SIZE; i++) {
        if (out[i] != (u8)i)
            return 0;
    }
    if (block_read(part, 4, 1, out) == 0)
        return 0;
    return 1;
}
