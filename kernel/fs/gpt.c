#include <crug/gpt.h>
#include <crug/string.h>
#include <crug/heap.h>
#include <crug/kernel.h>

#define GPT_HEADER_LBA 1u
#define GPT_ENTRY_LBA  2u
#define GPT_HEADER_SIZE 92u

struct gpt_header {
    u8 signature[8];
    u32 revision;
    u32 header_size;
    u32 header_crc;
    u32 reserved;
    u64 current_lba;
    u64 backup_lba;
    u64 first_usable_lba;
    u64 last_usable_lba;
    u8 disk_guid[16];
    u64 entry_lba;
    u32 entry_count;
    u32 entry_size;
    u32 entry_crc;
} __attribute__((packed));

struct gpt_entry {
    u8 type_guid[16];
    u8 unique_guid[16];
    u64 first_lba;
    u64 last_lba;
    u64 attributes;
    u8 name[72];
} __attribute__((packed));

u32 crc32(const void *data, usize len)
{
    const u8 *p = (const u8 *)data;
    u32 crc = 0xFFFFFFFFu;
    usize i;
    int bit;

    for (i = 0; i < len; i++) {
        crc ^= p[i];
        for (bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static void guid_to_ascii(const u8 guid[16], char out[37])
{
    static const char hex[] = "0123456789abcdef";
    int i;

    for (i = 0; i < 16; i++) {
        out[i * 2] = hex[guid[i] >> 4];
        out[i * 2 + 1] = hex[guid[i] & 0x0Fu];
    }
    out[32] = 0;
}

static int gpt_validate_header(const u8 *sector)
{
    const struct gpt_header *h = (const struct gpt_header *)sector;
    u8 copy[512];

    if (memcmp(h->signature, "EFI PART", 8) != 0)
        return -1;
    if (h->header_size < GPT_HEADER_SIZE || h->header_size > 512u)
        return -1;
    if (h->entry_size < 128u || h->entry_size > 4096u)
        return -1;
    if (h->entry_count == 0 || h->entry_count > 128u)
        return -1;
    memcpy(copy, sector, 512);
    memset(copy + 16, 0, 4);
    if (crc32(copy, h->header_size) != h->header_crc)
        return -1;
    return 0;
}

int gpt_parse(struct block_device *bd, struct gpt_table *out)
{
    u8 sector[512];
    const struct gpt_header *h;
    u8 *raw;
    usize bytes;
    usize sectors;
    u32 i;

    if (!bd || !out)
        return -1;
    memset(out, 0, sizeof(*out));
    if (block_read(bd, 0, 1, sector) != 0)
        return -1;
    if (sector[446 + 4] != 0xEEu)
        return -1;
    if (block_read(bd, GPT_HEADER_LBA, 1, sector) != 0)
        return -1;
    h = (const struct gpt_header *)sector;
    if (gpt_validate_header(sector) != 0)
        return -1;
    bytes = (usize)h->entry_count * h->entry_size;
    sectors = (bytes + 511u) / 512u;
    raw = (u8 *)kmalloc(sectors * 512u);
    if (!raw)
        return -1;
    if (block_read(bd, h->entry_lba, (u32)sectors, raw) != 0) {
        kfree(raw);
        return -1;
    }
    if (crc32(raw, bytes) != h->entry_crc) {
        kfree(raw);
        return -1;
    }
    for (i = 0; i < h->entry_count && out->count < GPT_MAX_PARTITIONS; i++) {
        struct gpt_partition *p;
        struct gpt_entry *e = (struct gpt_entry *)(raw + (usize)i * h->entry_size);
        u32 j;

        for (j = 0; j < 16; j++) {
            if (e->type_guid[j])
                break;
        }
        if (j == 16)
            continue;
        p = &out->partitions[out->count++];
        memcpy(p->type_guid, e->type_guid, 16);
        memcpy(p->unique_guid, e->unique_guid, 16);
        p->first_lba = e->first_lba;
        p->last_lba = e->last_lba;
        for (j = 0; j < GPT_NAME_CHARS; j++) {
            u16 c = (u16)e->name[j * 2] | (u16)((u16)e->name[j * 2 + 1] << 8);
            p->name[j] = (c < 128u) ? (char)c : '?';
        }
        p->name[GPT_NAME_CHARS] = 0;
        for (j = GPT_NAME_CHARS; j > 0; j--) {
            if (p->name[j - 1] != 0 && p->name[j - 1] != ' ')
                break;
            p->name[j - 1] = 0;
        }
    }
    kfree(raw);
    return (int)out->count;
}

struct gpt_ram {
    u8 data[4u * 512u];
};

static int gpt_ram_read(struct block_device *bd, u64 lba, u32 count, void *buf)
{
    struct gpt_ram *r = (struct gpt_ram *)bd->priv;
    if (lba + count > 4u)
        return -1;
    memcpy(buf, r->data + lba * 512u, count * 512u);
    return 0;
}

int gpt_selftest(void)
{
    static struct gpt_ram ram;
    struct block_device bd;
    struct gpt_table table;
    struct gpt_header *h;
    struct gpt_entry *e;
    char type[37];
    char guid[37];

    memset(&ram, 0, sizeof(ram));
    ram.data[446 + 4] = 0xEEu;
    h = (struct gpt_header *)(ram.data + 512);
    memcpy(h->signature, "EFI PART", 8);
    h->revision = 0x00010000u;
    h->header_size = GPT_HEADER_SIZE;
    h->current_lba = 1;
    h->backup_lba = 3;
    h->first_usable_lba = 2;
    h->last_usable_lba = 3;
    h->entry_lba = GPT_ENTRY_LBA;
    h->entry_count = 4;
    h->entry_size = 128;
    e = (struct gpt_entry *)(ram.data + 1024);
    e->type_guid[0] = 0xAFu;
    e->type_guid[1] = 0x3Du;
    e->first_lba = 4;
    e->last_lba = 7;
    e->name[0] = 'r';
    e->name[1] = 0;
    e->name[2] = 'o';
    e->name[3] = 0;
    h->entry_crc = crc32(e, h->entry_count * 128u);
    h->header_crc = 0;
    h->header_crc = crc32(h, h->header_size);

    memset(&bd, 0, sizeof(bd));
    strncpy(bd.name, "gptram", BLOCK_NAME_MAX - 1);
    bd.sectors = 4;
    bd.sector_size = 512;
    bd.read = gpt_ram_read;
    bd.priv = &ram;
    if (gpt_parse(&bd, &table) != 1)
        return 0;
    if (table.partitions[0].first_lba != 4 || table.partitions[0].last_lba != 7)
        return 0;
    if (strcmp(table.partitions[0].name, "ro") != 0)
        return 0;
    if (crc32("123456789", 9) != 0xCBF43926u)
        return 0;
    guid_to_ascii(table.partitions[0].type_guid, type);
    guid_to_ascii(table.partitions[0].unique_guid, guid);
    if (strlen(type) != 32 || strlen(guid) != 32)
        return 0;
    return 1;
}
