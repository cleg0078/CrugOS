#pragma once

#include <crug/types.h>
#include <crug/block.h>

#define GPT_MAX_PARTITIONS 16u
#define GPT_NAME_CHARS 36u

struct gpt_partition {
    u8 type_guid[16];
    u8 unique_guid[16];
    u64 first_lba;
    u64 last_lba;
    char name[GPT_NAME_CHARS + 1];
};

struct gpt_table {
    u32 count;
    struct gpt_partition partitions[GPT_MAX_PARTITIONS];
};

u32 crc32(const void *data, usize len);
int gpt_parse(struct block_device *bd, struct gpt_table *out);
int gpt_selftest(void);
