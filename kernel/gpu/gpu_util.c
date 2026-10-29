/* Pure virtio-gpu protocol helpers (phase 8A).
   Everything in this file is free of port io, so the host unit tests link it
   against synthetic command and response buffers. */
#include <crug/gpu.h>
#include <crug/string.h>

/* virtio-gpu control header: type, flags, fence_id, ctx_id, padding. */
#define GPU_HDR_SIZE 24u
#define GPU_DISPLAY_ONE 24u
#define GPU_DISPLAY_MAX 16u

void gpu_put32(u8 *buf, u32 off, u32 value)
{
    buf[off + 0u] = (u8)(value & 0xFFu);
    buf[off + 1u] = (u8)((value >> 8) & 0xFFu);
    buf[off + 2u] = (u8)((value >> 16) & 0xFFu);
    buf[off + 3u] = (u8)((value >> 24) & 0xFFu);
}

void gpu_put64(u8 *buf, u32 off, u64 value)
{
    gpu_put32(buf, off, (u32)(value & 0xFFFFFFFFu));
    gpu_put32(buf, off + 4u, (u32)(value >> 32));
}

u32 gpu_get32(const u8 *buf, u32 off)
{
    return (u32)buf[off + 0u] | ((u32)buf[off + 1u] << 8) |
           ((u32)buf[off + 2u] << 16) | ((u32)buf[off + 3u] << 24);
}

u32 gpu_cmd_init(u8 *buf, u32 cap, u32 type)
{
    if (!buf || cap < GPU_HDR_SIZE) {
        return 0u;
    }
    memset(buf, 0, cap);
    gpu_put32(buf, 0u, type);
    gpu_put32(buf, 4u, 0u);
    gpu_put64(buf, 8u, 0u);
    gpu_put32(buf, 16u, 0u);
    gpu_put32(buf, 20u, 0u);
    return GPU_HDR_SIZE;
}

/* 1 = OK_NODATA, 2 = OK_DISPLAY_INFO, -1 = protocol error, 0 = unknown. */
int gpu_resp_class(u32 type)
{
    if (type == 0x1100u) {
        return 1;
    }
    if (type == 0x1101u) {
        return 2;
    }
    if (type >= 0x1200u && type <= 0x1205u) {
        return -1;
    }
    return 0;
}

const char *gpu_resp_name(u32 type)
{
    switch (type) {
    case 0x1100u:
        return "ok";
    case 0x1101u:
        return "display-info";
    case 0x1200u:
        return "err-unspec";
    case 0x1201u:
        return "err-out-of-memory";
    case 0x1202u:
        return "err-invalid-scanout";
    case 0x1203u:
        return "err-invalid-resource";
    case 0x1204u:
        return "err-invalid-context";
    case 0x1205u:
        return "err-invalid-parameter";
    default:
        return "unknown";
    }
}

int gpu_rect_clip(struct gpu_rect *r, u32 width, u32 height)
{
    u32 right;
    u32 bottom;

    if (!r || !width || !height) {
        return 0;
    }
    if (r->x >= width || r->y >= height || r->w == 0u || r->h == 0u) {
        r->w = 0u;
        r->h = 0u;
        return 0;
    }
    right = r->x + r->w;
    bottom = r->y + r->h;
    if (right > width) {
        right = width;
    }
    if (bottom > height) {
        bottom = height;
    }
    r->w = right - r->x;
    r->h = bottom - r->y;
    return 1;
}

u32 gpu_backing_pages(u32 width, u32 height)
{
    u64 bytes;
    u64 pages;

    if (!width || !height || width > GPU_RECT_MAX_W || height > GPU_RECT_MAX_H) {
        return 0u;
    }
    bytes = (u64)width * (u64)height * 4u;
    pages = (bytes + 4095u) / 4096u;
    if (pages > GPU_BACKING_MAX_PAGES) {
        return 0u;
    }
    return (u32)pages;
}

/* Response layout: 24-byte header, then 16 records of
   {rect x,y,w,h}{enabled}{flags}.  The preferred size wins when present;
   otherwise the first enabled mode is used. */
int gpu_display_pick(const u8 *resp, u32 len, u32 want_w, u32 want_h,
                     u32 *width, u32 *height)
{
    u32 i;
    int first = -1;

    if (!resp || len < GPU_HDR_SIZE + GPU_DISPLAY_MAX * GPU_DISPLAY_ONE) {
        return -1;
    }
    for (i = 0; i < GPU_DISPLAY_MAX; i++) {
        u32 off = GPU_HDR_SIZE + i * GPU_DISPLAY_ONE;
        u32 w = gpu_get32(resp, off + 8u);
        u32 h = gpu_get32(resp, off + 12u);
        u32 enabled = gpu_get32(resp, off + 16u);

        if (!enabled || !w || !h || w > GPU_RECT_MAX_W || h > GPU_RECT_MAX_H) {
            continue;
        }
        if (want_w && want_h && w == want_w && h == want_h) {
            *width = w;
            *height = h;
            return 0;
        }
        if (first < 0) {
            first = (int)i;
        }
    }
    if (first < 0) {
        return -1;
    }
    *width = gpu_get32(resp, GPU_HDR_SIZE + (u32)first * GPU_DISPLAY_ONE + 8u);
    *height = gpu_get32(resp, GPU_HDR_SIZE + (u32)first * GPU_DISPLAY_ONE + 12u);
    return 0;
}

void gpu_damage_reset(struct gpu_damage *dmg)
{
    if (!dmg) {
        return;
    }
    dmg->x0 = 0u;
    dmg->y0 = 0u;
    dmg->x1 = 0u;
    dmg->y1 = 0u;
    dmg->valid = 0;
}

void gpu_damage_add(struct gpu_damage *dmg, u32 x, u32 y, u32 w, u32 h)
{
    u32 right;
    u32 bottom;

    if (!dmg || !w || !h) {
        return;
    }
    right = x + w;
    bottom = y + h;
    if (!dmg->valid) {
        dmg->x0 = x;
        dmg->y0 = y;
        dmg->x1 = right;
        dmg->y1 = bottom;
        dmg->valid = 1;
        return;
    }
    if (x < dmg->x0) {
        dmg->x0 = x;
    }
    if (y < dmg->y0) {
        dmg->y0 = y;
    }
    if (right > dmg->x1) {
        dmg->x1 = right;
    }
    if (bottom > dmg->y1) {
        dmg->y1 = bottom;
    }
}

int gpu_damage_rect(const struct gpu_damage *dmg, struct gpu_rect *out)
{
    if (!dmg || !out || !dmg->valid || dmg->x1 <= dmg->x0 || dmg->y1 <= dmg->y0) {
        return 0;
    }
    out->x = dmg->x0;
    out->y = dmg->y0;
    out->w = dmg->x1 - dmg->x0;
    out->h = dmg->y1 - dmg->y0;
    return 1;
}
