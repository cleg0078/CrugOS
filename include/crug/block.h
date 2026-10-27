#pragma once

#include <crug/types.h>

#define CRUG_SECTOR_SIZE 512u
#define BLOCK_NAME_MAX 16u

struct block_device {
    char name[BLOCK_NAME_MAX];
    u64 sectors;
    u32 sector_size;
    u32 reserved;
    int (*read)(struct block_device *bd, u64 lba, u32 count, void *buf);
    int (*write)(struct block_device *bd, u64 lba, u32 count, const void *buf);
    int (*flush)(struct block_device *bd);
    void *priv;
    struct block_device *next;
};

void block_init(void);
int block_register(struct block_device *bd);
struct block_device *block_find(const char *name);
struct block_device *block_first(void);
u64 block_device_count(void);
int block_read(struct block_device *bd, u64 lba, u32 count, void *buf);
int block_write(struct block_device *bd, u64 lba, u32 count, const void *buf);
int block_flush(struct block_device *bd);
struct block_device *block_partition(struct block_device *parent, u64 first_lba,
                                     u64 sector_count, const char *name);
int block_selftest(void);
