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

void test_gpu_functions(void)
{
    test_gpu_encoding();
    test_gpu_response();
    test_gpu_rect_clip();
    test_gpu_backing_pages();
    test_gpu_display_pick();
    test_gpu_damage();
}