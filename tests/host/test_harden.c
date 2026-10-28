/* Host tests for the hardening validators (include/crug/harden.h). These are
 * the checks that stand between device-supplied bytes and kernel memory, so
 * each one is exercised at its boundary and just past it. */
#include <crug/harden.h>
#include <string.h>
#include "test.h"

static void put16(u8 *p, u16 v)
{
    p[0] = (u8)(v & 0xFFu);
    p[1] = (u8)(v >> 8);
}

static void test_harden_range(void)
{
    CHECK(harden_range_ok(0, 0, HARDEN_IDENTITY_LIMIT) == 1);
    CHECK(harden_range_ok(0x1000, 0, HARDEN_IDENTITY_LIMIT) == 1);
    CHECK(harden_range_ok(0, 1, HARDEN_IDENTITY_LIMIT) == 0);
    CHECK(harden_range_ok(0x1000, 16, 0x2000) == 1);
    CHECK(harden_range_ok(0x1000, 0x1000, 0x2000) == 1);
    CHECK(harden_range_ok(0x1000, 0x1001, 0x2000) == 0);
    CHECK(harden_range_ok(0x2000, 1, 0x2000) == 0);
    CHECK(harden_range_ok(0x1FFF, 1, 0x2000) == 1);
    CHECK(harden_range_ok(0x1000, 16, 0) == 0);
    /* A length larger than the whole limit must not wrap the subtraction. */
    CHECK(harden_range_ok(0xFFFFFFFFFFFFFFF0ull, 0x20, 0x1000) == 0);
    CHECK(harden_range_ok(HARDEN_IDENTITY_LIMIT - 8, 8, HARDEN_IDENTITY_LIMIT) == 1);
    CHECK(harden_range_ok(HARDEN_IDENTITY_LIMIT - 7, 8, HARDEN_IDENTITY_LIMIT) == 0);
    /* [0x7FFFFFFF, 0x100000000) ends exactly at the limit. */
    CHECK(harden_range_ok(0x7FFFFFFF, 0x80000001ull, HARDEN_IDENTITY_LIMIT) == 1);
    CHECK(harden_range_ok(0x80000000, 0x80000001ull, HARDEN_IDENTITY_LIMIT) == 0);
}

static void test_harden_payload(void)
{
    int value = 7;

    CHECK(harden_payload_ok(0, 0) == 1);
    CHECK(harden_payload_ok(&value, 0) == 1);
    CHECK(harden_payload_ok(&value, sizeof(value)) == 1);
    CHECK(harden_payload_ok(0, 1) == 0);
    CHECK(harden_payload_ok(0, 1024) == 0);
}

static void test_harden_extent_header(void)
{
    u8 node[1012];

    memset(node, 0, sizeof(node));
    put16(node + 0, 0xF30Au);
    put16(node + 2, 2);   /* entries */
    put16(node + 4, 4);   /* max     */
    put16(node + 6, 0);   /* depth   */
    CHECK(harden_extent_header_ok(node, 60) == 1);
    CHECK(harden_extent_header_ok(node, sizeof(node)) == 1);
    CHECK(harden_extent_header_ok(node, 8) == 0);
    CHECK(harden_extent_header_ok(0, 60) == 0);

    /* The inline root only owns 60 bytes: max=5 would need 72. */
    put16(node + 4, 5);
    CHECK(harden_extent_header_ok(node, 60) == 0);
    CHECK(harden_extent_header_ok(node, 72) == 1);

    /* entries > max is a malformed header even when it fits. */
    put16(node + 2, 5);
    CHECK(harden_extent_header_ok(node, 72) == 1);
    put16(node + 2, 2);

    /* A leaf in a 1 KiB block can hold (1024-12-12)/12 = 83 extents. */
    put16(node + 4, 84);
    CHECK(harden_extent_header_ok(node, 1012) == 0);
    put16(node + 4, 83);
    CHECK(harden_extent_header_ok(node, 1012) == 1);

    /* Depth limit. */
    put16(node + 4, 4);
    put16(node + 6, 5);
    CHECK(harden_extent_header_ok(node, sizeof(node)) == 1);
    put16(node + 6, 6);
    CHECK(harden_extent_header_ok(node, sizeof(node)) == 0);
    put16(node + 6, 0);

    /* Magic is mandatory. */
    put16(node + 0, 0xF30Bu);
    CHECK(harden_extent_header_ok(node, 60) == 0);
}

static void test_harden_ext4_geometry(void)
{
    CHECK(harden_ext4_geometry_ok(1024, 8192, 2048, 20480, 5120) == 1);
    CHECK(harden_ext4_geometry_ok(1024, 1, 1, 1, 1) == 1);
    CHECK(harden_ext4_geometry_ok(1024, 8192, 8192, 8192, 8192) == 1);
    CHECK(harden_ext4_geometry_ok(1024, 8193, 2048, 20480, 5120) == 0);
    CHECK(harden_ext4_geometry_ok(1024, 8192, 8193, 20480, 5120) == 0);
    CHECK(harden_ext4_geometry_ok(4096, 32768, 8192, 65536, 16384) == 1);
    CHECK(harden_ext4_geometry_ok(4096, 32769, 8192, 65536, 16384) == 0);
    CHECK(harden_ext4_geometry_ok(1024, 0, 2048, 20480, 5120) == 0);
    CHECK(harden_ext4_geometry_ok(1024, 8192, 0, 20480, 5120) == 0);
    CHECK(harden_ext4_geometry_ok(1024, 8192, 2048, 0, 5120) == 0);
    CHECK(harden_ext4_geometry_ok(1024, 8192, 2048, 20480, 0) == 0);
    CHECK(harden_ext4_geometry_ok(0, 8192, 2048, 20480, 5120) == 0);
    CHECK(harden_ext4_geometry_ok(512, 128, 64, 1024, 256) == 0);
    CHECK(harden_ext4_geometry_ok(1023, 128, 64, 1024, 256) == 0);
    CHECK(harden_ext4_geometry_ok(1000, 128, 64, 1024, 256) == 0);
}

static void test_harden_doorbell(void)
{
    CHECK(harden_nvme_doorbell_ok(0x4000, 4, 0, 0) == 1);
    CHECK(harden_nvme_doorbell_ok(0x4000, 4, 0, 1) == 1);
    CHECK(harden_nvme_doorbell_ok(0x4000, 4, 1, 0) == 1);
    CHECK(harden_nvme_doorbell_ok(0x4000, 4, 1, 1) == 1);
    CHECK(harden_nvme_doorbell_ok(0x1004, 4, 0, 0) == 1);
    CHECK(harden_nvme_doorbell_ok(0x1003, 4, 0, 0) == 0);
    CHECK(harden_nvme_doorbell_ok(0x2000, 4, 1, 1) == 1);
    CHECK(harden_nvme_doorbell_ok(0x1FFF, 4, 1, 1) == 1);
    CHECK(harden_nvme_doorbell_ok(0x100C, 4, 1, 1) == 0);
    CHECK(harden_nvme_doorbell_ok(0x4000, 0, 0, 0) == 0);
    CHECK(harden_nvme_doorbell_ok(0x4000, 2, 0, 0) == 0);
    CHECK(harden_nvme_doorbell_ok(0x4000, 3, 0, 0) == 0);
    CHECK(harden_nvme_doorbell_ok(0, 4, 0, 0) == 0);
    /* CAP.DSTRD = 15 gives a 128 KiB stride far outside a 16 KiB BAR. */
    CHECK(harden_nvme_doorbell_ok(0x4000, 4u << 15, 1, 1) == 0);
    CHECK(harden_nvme_doorbell_ok(0x4000, 4u << 15, 0, 0) == 1);
}

static void test_harden_kdf(void)
{
    CHECK(harden_kdf_cost_ok(4096, 3, 1, 1024, 65536, 2, 8) == 1);
    CHECK(harden_kdf_cost_ok(1024, 2, 1, 1024, 65536, 2, 8) == 1);
    CHECK(harden_kdf_cost_ok(65536, 8, 1, 1024, 65536, 2, 8) == 1);
    CHECK(harden_kdf_cost_ok(1024, 2, 16, 1024, 65536, 2, 8) == 1);
    CHECK(harden_kdf_cost_ok(512, 3, 1, 1024, 65536, 2, 8) == 0);
    CHECK(harden_kdf_cost_ok(131072, 3, 1, 1024, 65536, 2, 8) == 0);
    CHECK(harden_kdf_cost_ok(4096, 1, 1, 1024, 65536, 2, 8) == 0);
    CHECK(harden_kdf_cost_ok(4096, 9, 1, 1024, 65536, 2, 8) == 0);
    CHECK(harden_kdf_cost_ok(4096, 3, 0, 1024, 65536, 2, 8) == 0);
    CHECK(harden_kdf_cost_ok(4096, 3, 17, 1024, 65536, 2, 8) == 0);
    /* m must still cover 8 KiB per lane. */
    CHECK(harden_kdf_cost_ok(1024, 3, 1, 512, 65536, 2, 8) == 1);
    CHECK(harden_kdf_cost_ok(100000, 3, 1, 1000, 65536, 2, 8) == 0);
    CHECK(harden_kdf_cost_ok(4096, 3, 1, 65536, 1024, 2, 8) == 0);
    CHECK(harden_kdf_cost_ok(4096, 3, 1, 1024, 65536, 8, 2) == 0);
}

static void test_harden_volume(void)
{
    CHECK(harden_volume_ok(24576, 24576, 0) == 1);
    CHECK(harden_volume_ok(24577, 24576, 0) == 0);
    CHECK(harden_volume_ok(24544, 24576, 32) == 1);
    CHECK(harden_volume_ok(24545, 24576, 32) == 0);
    CHECK(harden_volume_ok(0, 0, 0) == 1);
    CHECK(harden_volume_ok(1, 0, 0) == 0);
    CHECK(harden_volume_ok(1, 8, 9) == 0);
    CHECK(harden_volume_ok(0, 8, 8) == 1);
}

void test_harden_functions(void)
{
    test_harden_range();
    test_harden_payload();
    test_harden_extent_header();
    test_harden_ext4_geometry();
    test_harden_doorbell();
    test_harden_kdf();
    test_harden_volume();
}