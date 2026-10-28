/* CrugOS device-supplied value validators (hardening pass 1). */
#include <crug/harden.h>

/* Little-endian u16, independent of struct packing. */
static u16 harden_ld16(const u8 *p)
{
    return (u16)((u16)p[0] | ((u16)p[1] << 8));
}

int harden_range_ok(u64 addr, u64 len, u64 limit)
{
    if (len == 0)
        return 1;
    if (addr == 0 || limit == 0 || len > limit)
        return 0;
    return addr <= limit - len;
}

int harden_payload_ok(const void *data, u64 len)
{
    return !(len != 0 && data == 0);
}

int harden_extent_header_ok(const u8 *node, u32 node_bytes)
{
    u16 entries;
    u16 max;
    u16 depth;
    u64 need;

    if (!node || node_bytes < HARDEN_EXTENT_HEADER_BYTES)
        return 0;
    if (harden_ld16(node + 0) != HARDEN_EXTENT_MAGIC)
        return 0;
    entries = harden_ld16(node + 2);
    max = harden_ld16(node + 4);
    depth = harden_ld16(node + 6);
    if (entries > max || depth > HARDEN_EXTENT_MAX_DEPTH)
        return 0;
    need = (u64)HARDEN_EXTENT_HEADER_BYTES +
           (u64)max * (u64)HARDEN_EXTENT_BYTES;
    return need <= (u64)node_bytes;
}

int harden_ext4_geometry_ok(u32 block_size, u32 blocks_per_group,
                            u32 inodes_per_group, u32 blocks_count,
                            u32 inodes_count)
{
    u32 limit;

    if (block_size < 1024u || (block_size & (block_size - 1u)) != 0)
        return 0;
    if (blocks_per_group == 0 || inodes_per_group == 0 ||
        blocks_count == 0 || inodes_count == 0)
        return 0;
    /* The bitmaps are one block each: a group cannot be larger than the
       bits a bitmap block can hold, or indexing a bit reads out of it. */
    limit = block_size * 8u;
    if (blocks_per_group > limit || inodes_per_group > limit)
        return 0;
    return 1;
}

int harden_nvme_doorbell_ok(u64 bar_size, u32 stride, u32 qid, int cq)
{
    u64 off;

    if (bar_size == 0 || stride < 4u || (stride & (stride - 1u)) != 0)
        return 0;
    off = HARDEN_NVME_DOORBELL_BASE +
          (2ull * (u64)qid + (cq ? 1ull : 0ull)) * (u64)stride;
    return off + 4ull <= bar_size;
}

int harden_kdf_cost_ok(u32 m_cost_kib, u32 t_cost, u32 lanes, u32 min_m_kib,
                       u32 max_m_kib, u32 min_t_cost, u32 max_t_cost)
{
    if (lanes == 0 || lanes > HARDEN_KDF_MAX_LANES)
        return 0;
    if (min_m_kib > max_m_kib || min_t_cost > max_t_cost)
        return 0;
    if (m_cost_kib < min_m_kib || m_cost_kib > max_m_kib)
        return 0;
    if (m_cost_kib < 8u * lanes)
        return 0;
    if (t_cost < min_t_cost || t_cost > max_t_cost)
        return 0;
    return 1;
}

int harden_volume_ok(u64 volume_sectors, u64 parent_sectors,
                     u64 offset_sectors)
{
    if (offset_sectors > parent_sectors)
        return 0;
    return volume_sectors <= parent_sectors - offset_sectors;
}