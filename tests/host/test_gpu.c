/* Host tests for the pure virtio-gpu helpers (kernel/gpu/gpu_util.c). */
#include <crug/gpu.h>
#include <string.h>
#include "test.h"

static void test_gpu_encoding(void)
{
    u8 buf[64];

    memset(buf, 0xAA, sizeof(buf));
    gpu_put32(buf, 3u, 0x12345678u);
    CHECK(buf[3] == 0x78u);
    CHECK(buf[4] == 0x56u);
    CHECK(buf[5] == 0x34u);
    CHECK(buf[6] == 0x12u);
    CHECK(gpu_get32(buf, 3u) == 0x12345678u);

    gpu_put64(buf, 8u, 0x0123456789ABCDEFull);
    CHECK(gpu_get32(buf, 8u) == 0x89ABCDEFu);
    CHECK(gpu_get32(buf, 12u) == 0x01234567u);

    /* The header is zeroed and the type lands in the first word. */
    CHECK(gpu_cmd_init(buf, sizeof(buf), 0x0100u) == 24u);
    CHECK(gpu_get32(buf, 0u) == 0x0100u);
    CHECK(gpu_get32(buf, 4u) == 0u);
    CHECK(gpu_get32(buf, 8u) == 0u);
    CHECK(gpu_get32(buf, 12u) == 0u);
    CHECK(gpu_get32(buf, 16u) == 0u);
    CHECK(gpu_get32(buf, 20u) == 0u);
    CHECK(gpu_get32(buf, 24u) == 0u);
    CHECK(gpu_cmd_init(buf, 23u, 0x0100u) == 0u);
    CHECK(gpu_cmd_init(NULL, sizeof(buf), 0x0100u) == 0u);
}

static void test_gpu_response(void)
{
    CHECK(gpu_resp_class(0x1100u) == 1);
    CHECK(gpu_resp_class(0x1101u) == 2);
    CHECK(gpu_resp_class(0x1200u) == -1);
    CHECK(gpu_resp_class(0x1205u) == -1);
    CHECK(gpu_resp_class(0x1206u) == 0);
    CHECK(gpu_resp_class(0x0100u) == 0);
    CHECK(gpu_resp_class(0u) == 0);

    CHECK_STR(gpu_resp_name(0x1100u), "ok");
    CHECK_STR(gpu_resp_name(0x1101u), "display-info");
    CHECK_STR(gpu_resp_name(0x1200u), "err-unspec");
    CHECK_STR(gpu_resp_name(0x1201u), "err-out-of-memory");
    CHECK_STR(gpu_resp_name(0x1202u), "err-invalid-scanout");
    CHECK_STR(gpu_resp_name(0x1203u), "err-invalid-resource");
    CHECK_STR(gpu_resp_name(0x1204u), "err-invalid-context");
    CHECK_STR(gpu_resp_name(0x1205u), "err-invalid-parameter");
    CHECK_STR(gpu_resp_name(0x1234u), "unknown");
}

static void test_gpu_rect_clip(void)
{
    struct gpu_rect r;

    r.x = 10u; r.y = 20u; r.w = 30u; r.h = 40u;
    CHECK(gpu_rect_clip(&r, 1280u, 800u) == 1);
    CHECK(r.x == 10u && r.y == 20u && r.w == 30u && r.h == 40u);

    /* Clipped on the right and bottom edges. */
    r.x = 1270u; r.y = 790u; r.w = 100u; r.h = 100u;
    CHECK(gpu_rect_clip(&r, 1280u, 800u) == 1);
    CHECK(r.w == 10u && r.h == 10u);

    /* Completely outside. */
    r.x = 1280u; r.y = 0u; r.w = 10u; r.h = 10u;
    CHECK(gpu_rect_clip(&r, 1280u, 800u) == 0);
    CHECK(r.w == 0u && r.h == 0u);
    r.x = 0u; r.y = 800u; r.w = 10u; r.h = 10u;
    CHECK(gpu_rect_clip(&r, 1280u, 800u) == 0);

    /* Degenerate input. */
    r.x = 0u; r.y = 0u; r.w = 0u; r.h = 10u;
    CHECK(gpu_rect_clip(&r, 1280u, 800u) == 0);
    r.x = 0u; r.y = 0u; r.w = 10u; r.h = 10u;
    CHECK(gpu_rect_clip(&r, 0u, 800u) == 0);
    CHECK(gpu_rect_clip(&r, 1280u, 0u) == 0);
    CHECK(gpu_rect_clip(NULL, 1280u, 800u) == 0);
}

static void test_gpu_backing_pages(void)
{
    /* 1280x800x4 = 4096000 bytes, exactly 1000 pages. */
    CHECK(gpu_backing_pages(1280u, 800u) == 1000u);
    CHECK(gpu_backing_pages(1u, 1u) == 1u);
    CHECK(gpu_backing_pages(1024u, 768u) == 768u);
    CHECK(gpu_backing_pages(0u, 800u) == 0u);
    CHECK(gpu_backing_pages(1280u, 0u) == 0u);
    CHECK(gpu_backing_pages(GPU_RECT_MAX_W + 1u, 100u) == 0u);
    CHECK(gpu_backing_pages(100u, GPU_RECT_MAX_H + 1u) == 0u);
    /* 4096x2160x4 needs 8640 pages, past the 8192 page ceiling. */
    CHECK(gpu_backing_pages(GPU_RECT_MAX_W, GPU_RECT_MAX_H) == 0u);
}

static void put_mode(u8 *resp, u32 index, u32 w, u32 h, u32 enabled)
{
    u32 off = 24u + index * 24u;

    gpu_put32(resp, off + 0u, w);
    gpu_put32(resp, off + 4u, h);
    gpu_put32(resp, off + 8u, w);
    gpu_put32(resp, off + 12u, h);
    gpu_put32(resp, off + 16u, enabled);
    gpu_put32(resp, off + 20u, 0u);
}

static void test_gpu_display_pick(void)
{
    u8 resp[24u + 16u * 24u];
    u32 w = 0u;
    u32 h = 0u;

    memset(resp, 0, sizeof(resp));
    put_mode(resp, 0u, 1024u, 768u, 1u);
    put_mode(resp, 1u, 1280u, 800u, 1u);

    /* No preference: the first enabled mode wins. */
    CHECK(gpu_display_pick(resp, sizeof(resp), 0u, 0u, &w, &h) == 0);
    CHECK(w == 1024u && h == 768u);

    /* The preferred size wins wherever it sits in the table. */
    CHECK(gpu_display_pick(resp, sizeof(resp), 1280u, 800u, &w, &h) == 0);
    CHECK(w == 1280u && h == 800u);

    /* A missing preferred size falls back to the first enabled mode. */
    CHECK(gpu_display_pick(resp, sizeof(resp), 800u, 600u, &w, &h) == 0);
    CHECK(w == 1024u && h == 768u);

    /* Disabled and oversized modes are skipped. */
    put_mode(resp, 0u, 1024u, 768u, 0u);
    put_mode(resp, 2u, 5000u, 800u, 1u);
    CHECK(gpu_display_pick(resp, sizeof(resp), 0u, 0u, &w, &h) == 0);
    CHECK(w == 1280u && h == 800u);

    /* Nothing enabled at all. */
    memset(resp, 0, sizeof(resp));
    CHECK(gpu_display_pick(resp, sizeof(resp), 0u, 0u, &w, &h) == -1);

    /* A short or missing response is a protocol error. */
    CHECK(gpu_display_pick(resp, 24u, 0u, 0u, &w, &h) == -1);
    CHECK(gpu_display_pick(NULL, sizeof(resp), 0u, 0u, &w, &h) == -1);
}

static void test_gpu_damage(void)
{
    struct gpu_damage d;
    struct gpu_rect r;

    memset(&d, 0xAA, sizeof(d));
    gpu_damage_reset(&d);
    CHECK(d.valid == 0);
    CHECK(d.x0 == 0u && d.y0 == 0u && d.x1 == 0u && d.y1 == 0u);
    CHECK(gpu_damage_rect(&d, &r) == 0);

    /* The first rectangle seeds the box. */
    gpu_damage_add(&d, 10u, 20u, 30u, 40u);
    CHECK(d.valid == 1);
    CHECK(gpu_damage_rect(&d, &r) == 1);
    CHECK(r.x == 10u && r.y == 20u && r.w == 30u && r.h == 40u);

    /* A contained rectangle does not grow it. */
    gpu_damage_add(&d, 12u, 22u, 4u, 4u);
    CHECK(gpu_damage_rect(&d, &r) == 1);
    CHECK(r.x == 10u && r.y == 20u && r.w == 30u && r.h == 40u);

    /* A rectangle touching the upper left and lower right grows it. */
    gpu_damage_add(&d, 0u, 0u, 4u, 4u);
    gpu_damage_add(&d, 100u, 200u, 8u, 8u);
    CHECK(gpu_damage_rect(&d, &r) == 1);
    CHECK(r.x == 0u && r.y == 0u && r.w == 108u && r.h == 208u);

    /* Zero-size and null rectangles are ignored. */
    gpu_damage_add(&d, 500u, 500u, 0u, 10u);
    gpu_damage_add(&d, 500u, 500u, 10u, 0u);
    gpu_damage_add(NULL, 0u, 0u, 10u, 10u);
    CHECK(gpu_damage_rect(&d, &r) == 1);
    CHECK(r.w == 108u && r.h == 208u);
    CHECK(gpu_damage_rect(&d, NULL) == 0);
    CHECK(gpu_damage_rect(NULL, &r) == 0);

    gpu_damage_reset(&d);
    CHECK(gpu_damage_rect(&d, &r) == 0);
    gpu_damage_reset(NULL);
}


/* --- EDID base block (phase 8B) ------------------------------------- */

static void edid_set16(u8 *blk, u32 off, u32 value)
{
    blk[off] = (u8)(value & 0xFFu);
    blk[off + 1u] = (u8)((value >> 8) & 0xFFu);
}

static void edid_seal(u8 *blk)
{
    u32 i;
    u32 sum = 0u;

    for (i = 0; i < 127u; i++) {
        sum += (u32)blk[i];
    }
    blk[127] = (u8)((256u - (sum & 0xFFu)) & 0xFFu);
}

static void edid_detail_timing(u8 *blk, u32 index, u32 clock_10khz,
                               u32 hactive, u32 hblank, u32 vactive,
                               u32 vblank)
{
    u8 *d = blk + 54u + index * 18u;

    edid_set16(d, 0u, clock_10khz);
    d[2] = (u8)(hactive & 0xFFu);
    d[3] = (u8)(hblank & 0xFFu);
    d[4] = (u8)(((hactive >> 8) & 0x0Fu) << 4) |
           (u8)((hblank >> 8) & 0x0Fu);
    d[5] = (u8)(vactive & 0xFFu);
    d[6] = (u8)(vblank & 0xFFu);
    d[7] = (u8)(((vactive >> 8) & 0x0Fu) << 4) |
           (u8)((vblank >> 8) & 0x0Fu);
}

static void edid_build(u8 *blk)
{
    u32 i;

    for (i = 0; i < 128u; i++) {
        blk[i] = 0u;
    }
    blk[0] = 0x00u; blk[1] = 0xFFu; blk[2] = 0xFFu; blk[3] = 0xFFu;
    blk[4] = 0xFFu; blk[5] = 0xFFu; blk[6] = 0xFFu; blk[7] = 0x00u;
    /* 'RHT' = 18/8/20 packed big-endian at offset 8. */
    blk[8] = 0x49u; blk[9] = 0x14u;
    blk[10] = 0x34u; blk[11] = 0x12u;
    blk[12] = 0x45u; blk[13] = 0x23u;
    blk[16] = 12u;
    blk[17] = 34u;   /* 1990 + 34 = 2024 */
    blk[18] = 1u; blk[19] = 3u;
    blk[21] = 30u; blk[22] = 19u;
    edid_detail_timing(blk, 0u, 6500u, 1024u, 320u, 768u, 38u);
    /* Descriptor 1 stays zero: word 0 == 0 marks a display descriptor. */
    edid_detail_timing(blk, 2u, 4000u, 800u, 256u, 600u, 28u);
    edid_seal(blk);
}

static void test_gpu_edid(void)
{
    u8 blk[128];
    struct gpu_edid edid;
    char vendor[4];
    u32 i;

    edid_build(blk);
    CHECK(gpu_edid_parse(blk, sizeof(blk), &edid) == 0);
    CHECK(edid.valid == 1);
    CHECK(edid.block_count == 1u);
    CHECK(edid.vendor == 0x4914u);
    CHECK(edid.product == 0x1234u);
    CHECK(edid.serial == 0x2345u);
    CHECK(edid.week == 12u);
    CHECK(edid.year == 2024u);
    CHECK(edid.width_cm == 30u);
    CHECK(edid.height_cm == 19u);
    CHECK(edid.mode_count == 2u);
    CHECK(edid.has_preferred == 1);
    CHECK(edid.preferred.width == 1024u);
    CHECK(edid.preferred.height == 768u);
    CHECK(edid.preferred.clock_khz == 65000u);
    CHECK(edid.preferred.refresh_hz == 60u);
    CHECK(edid.modes[1].width == 800u);
    CHECK(edid.modes[1].height == 600u);
    CHECK(edid.modes[1].refresh_hz == 60u);
    gpu_edid_vendor(&edid, vendor);
    CHECK_STR(vendor, "RHT");

    /* A wrong checksum or header rejects the whole block and zeroes it. */
    blk[127] = (u8)(blk[127] + 1u);
    CHECK(gpu_edid_parse(blk, sizeof(blk), &edid) == -1);
    CHECK(edid.valid == 0);
    CHECK(edid.mode_count == 0u);
    edid_seal(blk);
    blk[0] = 0xAAu;
    edid_seal(blk);
    CHECK(gpu_edid_parse(blk, sizeof(blk), &edid) == -1);

    /* Truncated input, no timing and null pointers are all rejected. */
    edid_build(blk);
    CHECK(gpu_edid_parse(blk, 127u, &edid) == -1);
    for (i = 54u; i < 126u; i++) {
        blk[i] = 0u;
    }
    edid_seal(blk);
    CHECK(gpu_edid_parse(blk, sizeof(blk), &edid) == -1);
    CHECK(gpu_edid_parse(NULL, sizeof(blk), &edid) == -1);
    CHECK(gpu_edid_parse(blk, sizeof(blk), NULL) == -1);

    /* All four detailed timings are kept; the first stays preferred. */
    edid_build(blk);
    edid_detail_timing(blk, 1u, 4000u, 800u, 256u, 600u, 28u);
    edid_detail_timing(blk, 3u, 6500u, 1024u, 320u, 768u, 38u);
    edid_seal(blk);
    CHECK(gpu_edid_parse(blk, sizeof(blk), &edid) == 0);
    CHECK(edid.mode_count == 4u);
    CHECK(edid.preferred.width == 1024u);

    /* A manufacturer id outside A..Z has no printable name. */
    edid_build(blk);
    blk[8] = 0x00u;
    blk[9] = 0x00u;
    edid_seal(blk);
    CHECK(gpu_edid_parse(blk, sizeof(blk), &edid) == 0);
    gpu_edid_vendor(&edid, vendor);
    CHECK_STR(vendor, "???");
}

void test_gpu_functions(void)
{
    test_gpu_encoding();
    test_gpu_response();
    test_gpu_rect_clip();
    test_gpu_backing_pages();
    test_gpu_display_pick();
    test_gpu_damage();
    test_gpu_edid();
}
