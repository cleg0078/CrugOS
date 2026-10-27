#pragma once

#include <crug/types.h>
#include <crug/block.h>

#define RAID_MAX_MEMBERS 8u
#define RAID_META_MAGIC 0x43524131u /* 'CRA1' */
#define RAID_META_VERSION 1u
#define RAID_LEVEL_1 1u

int raid_assemble(struct block_device **members, u32 count, const char *name,
                  u32 *present, int *degraded);
int raid_is_degraded(struct block_device *bd);
u32 raid_member_count(struct block_device *bd);
