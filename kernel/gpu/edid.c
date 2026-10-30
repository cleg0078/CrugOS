/* Phase 8B: EDID base-block decoding for virtio-gpu mode selection.
   VirtIO-GPU answers GET_EDID with the monitor's EDID byte stream; the
   driver decodes the base block here so the scanout can adopt the preferred
   detailed timing instead of falling back to a fixed 1024x768 list.  Pure
   code without device access, also linked by the host tests. */
#include <crug/gpu.h>

#define EDID_BASE 128u
#define EDID_DESCRIPTOR_OFF 54u
#define EDID_DESCRIPTOR_LEN 18u
#define EDID_DESCRIPTOR_COUNT 4u

static u32 edid_rd8(const u8 *d, u32 off)
{
    return (u32)d[off];
}

static u32 edid_rd16(const u8 *d, u32 off)
{
    return (u32)d[off] | ((u32)d[off + 1u] << 8);
}

/* The manufacturer id at byte 8 is the one big-endian word in the EDID
   header: its high bits belong to the first letter. */
static u32 edid_rd16be(const u8 *d, u32 off)
{
    return ((u32)d[off] << 8) | (u32)d[off + 1u];
}

static u32 edid_rd32(const u8 *d, u32 off)
{
    return (u32)d[off] | ((u32)d[off + 1u] << 8) | ((u32)d[off + 2u] << 16) |
           ((u32)d[off + 3u] << 24);
}

static void edid_zero(struct gpu_edid *out)
{
    u8 *p = (u8 *)out;
    u32 i;

    for (i = 0; i < (u32)sizeof(*out); i++) {
        p[i] = 0u;
    }
}

static int edid_header_ok(const u8 *d)
{
    static const u8 header[8] = { 0x00u, 0xFFu, 0xFFu, 0xFFu,
                                  0xFFu, 0xFFu, 0xFFu, 0x00u };
    u32 i;

    for (i = 0; i < 8u; i++) {
        if (d[i] != header[i]) {
            return 0;
        }
    }
    return 1;
}

static int edid_checksum_ok(const u8 *block)
{
    u32 i;
    u32 sum = 0u;

    for (i = 0; i < EDID_BASE; i++) {
        sum += (u32)block[i];
    }
    return (sum & 0xFFu) == 0u;
}

/* One 18-byte detailed timing descriptor: pixel clock in 10 kHz units and
   the active/blanking split of each dimension. */
static void edid_detail(const u8 *d, struct gpu_edid_mode *m)
{
    u32 hblank;
    u32 vblank;
    u32 htotal;
    u32 vtotal;

    m->clock_khz = edid_rd16(d, 0u) * 10u;
    m->width = edid_rd8(d, 2u) | ((edid_rd8(d, 4u) & 0xF0u) << 4);
    hblank = edid_rd8(d, 3u) | ((edid_rd8(d, 4u) & 0x0Fu) << 8);
    m->height = edid_rd8(d, 5u) | ((edid_rd8(d, 7u) & 0xF0u) << 4);
    vblank = edid_rd8(d, 6u) | ((edid_rd8(d, 7u) & 0x0Fu) << 8);
    m->refresh_hz = 0u;
    htotal = m->width + hblank;
    vtotal = m->height + vblank;
    if (m->clock_khz && htotal && vtotal) {
        m->refresh_hz = (m->clock_khz * 1000u) / (htotal * vtotal);
    }
}

int gpu_edid_parse(const u8 *data, u32 len, struct gpu_edid *out)
{
    u32 i;
    u32 blocks;

    if (!data || !out) {
        return -1;
    }
    edid_zero(out);
    if (len < EDID_BASE || !edid_header_ok(data) || !edid_checksum_ok(data)) {
        return -1;
    }
    blocks = len / EDID_BASE;
    if (blocks > 8u) {
        blocks = 8u;
    }
    if (blocks == 0u) {
        blocks = 1u;
    }
    out->block_count = blocks;
    out->vendor = (u16)edid_rd16be(data, 8u);
    out->product = (u16)edid_rd16(data, 10u);
    out->serial = edid_rd32(data, 12u);
    out->week = edid_rd8(data, 16u);
    out->year = 1990u + edid_rd8(data, 17u);
    out->width_cm = edid_rd8(data, 21u);
    out->height_cm = edid_rd8(data, 22u);
    for (i = 0; i < EDID_DESCRIPTOR_COUNT; i++) {
        const u8 *d = data + EDID_DESCRIPTOR_OFF + i * EDID_DESCRIPTOR_LEN;
        struct gpu_edid_mode m;

        if (edid_rd16(d, 0u) == 0u) {
            continue;   /* display descriptor, not a timing */
        }
        edid_detail(d, &m);
        if (!m.width || !m.height) {
            continue;
        }
        if (out->mode_count < GPU_EDID_MAX_MODES) {
            out->modes[out->mode_count] = m;
            out->mode_count++;
        }
        if (!out->has_preferred) {
            out->preferred = m;
            out->has_preferred = 1;
        }
    }
    if (!out->mode_count) {
        edid_zero(out);
        return -1;
    }
    out->valid = 1;
    return 0;
}

/* The manufacturer id packs three 5-bit letters big-endian at EDID offset 8;
   bit 15 is the high bit of the first letter, not a validity flag. */
void gpu_edid_vendor(const struct gpu_edid *edid, char name[4])
{
    u16 id;
    u32 a;
    u32 b;
    u32 c;

    if (!name) {
        return;
    }
    id = edid ? edid->vendor : 0u;
    a = (id >> 10) & 0x1Fu;
    b = (id >> 5) & 0x1Fu;
    c = id & 0x1Fu;
    if (a < 1u || a > 26u || b < 1u || b > 26u || c < 1u || c > 26u) {
        name[0] = '?';
        name[1] = '?';
        name[2] = '?';
        name[3] = 0;
        return;
    }
    name[0] = (char)('A' + a - 1);
    name[1] = (char)('A' + b - 1);
    name[2] = (char)('A' + c - 1);
    name[3] = 0;
}
