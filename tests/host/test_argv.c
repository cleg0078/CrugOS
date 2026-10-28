/* Host unit tests for the argv/envp block builder (phase U3).
 *
 * The builder produces the initial stack image that user/start.S reads as
 * argc at rsp, argv at rsp + 8 and envp after the argv NULL terminator. A
 * wrong alignment here silently gives a program argc 0, which is exactly the
 * bug this test pins down. */
#include <crug/exec.h>
#include <crug/string.h>
#include "test.h"

#define ARGV_BASE 0x0000008020000000ull

static u8 g_area[EXEC_ARGS_BLOCK_MAX];
static u8 g_big_area[EXEC_ARGS_BLOCK_MAX * 2u];

static u64 rd64(const u8 *area, u64 off)
{
    u64 v = 0;

    memcpy(&v, area + off, 8);
    return v;
}

static const char *at(const u8 *area, u64 area_size, u64 addr)
{
    u64 off;

    if (addr < ARGV_BASE)
        return 0;
    off = addr - ARGV_BASE;
    if (off >= area_size)
        return 0;
    return (const char *)(area + off);
}

void test_argv_functions(void)
{
    const char *argv[3];
    const char *envp[2];
    u64 rsp = 0;
    u64 p;

    argv[0] = "/bin/sh";
    argv[1] = "/etc/sh.rc";
    argv[2] = 0;
    envp[0] = "PATH=/bin";
    envp[1] = 0;

    memset(g_area, 0xAA, sizeof(g_area));
    CHECK(exec_args_build(g_area, sizeof(g_area), ARGV_BASE, 2, argv, envp,
                          &rsp) == 0);
    CHECK((rsp % 16ull) == 0);
    CHECK(rsp + 48ull <= sizeof(g_area));
    /* argc, argv[0], argv[1], NULL, envp[0], NULL */
    CHECK(rd64(g_area, rsp) == 2);
    p = rd64(g_area, rsp + 8ull);
    CHECK(p > rsp + 48ull);
    CHECK(p < ARGV_BASE + sizeof(g_area));
    CHECK_STR(at(g_area, sizeof(g_area), p), "/bin/sh");
    p = rd64(g_area, rsp + 16ull);
    CHECK_STR(at(g_area, sizeof(g_area), p), "/etc/sh.rc");
    CHECK(rd64(g_area, rsp + 24ull) == 0);
    p = rd64(g_area, rsp + 32ull);
    CHECK_STR(at(g_area, sizeof(g_area), p), "PATH=/bin");
    CHECK(rd64(g_area, rsp + 40ull) == 0);

    /* argc 0 with no argv and no envp: the block is just the argc word. */
    memset(g_area, 0xAA, sizeof(g_area));
    rsp = 1;
    CHECK(exec_args_build(g_area, sizeof(g_area), ARGV_BASE, 0, 0, 0, &rsp) == 0);
    CHECK((rsp % 16ull) == 0);
    CHECK(rd64(g_area, rsp) == 0);
    CHECK(rsp + 16ull <= sizeof(g_area));

    /* An odd argc still leaves a 16-byte aligned rsp. */
    argv[0] = "a";
    argv[1] = "b";
    argv[2] = "c";
    memset(g_area, 0xAA, sizeof(g_area));
    CHECK(exec_args_build(g_area, sizeof(g_area), ARGV_BASE, 3, argv, 0,
                          &rsp) == 0);
    CHECK((rsp % 16ull) == 0);
    CHECK(rd64(g_area, rsp) == 3);
    CHECK_STR(at(g_area, sizeof(g_area), rd64(g_area, rsp + 8ull)), "a");
    CHECK_STR(at(g_area, sizeof(g_area), rd64(g_area, rsp + 16ull)), "b");
    CHECK_STR(at(g_area, sizeof(g_area), rd64(g_area, rsp + 24ull)), "c");
    CHECK(rd64(g_area, rsp + 32ull) == 0);

    /* The area is capped at EXEC_ARGS_BLOCK_MAX even when the caller hands in
       a bigger buffer, so every pointer stays inside one page. */
    memset(g_big_area, 0xAA, sizeof(g_big_area));
    CHECK(exec_args_build(g_big_area, sizeof(g_big_area), ARGV_BASE, 3, argv, 0,
                          &rsp) == 0);
    CHECK(rsp < EXEC_ARGS_BLOCK_MAX);
    CHECK(rd64(g_big_area, rsp) == 3);

    /* Rejections. */
    CHECK(exec_args_build(0, sizeof(g_area), ARGV_BASE, 2, argv, 0, &rsp) == -1);
    CHECK(exec_args_build(g_area, sizeof(g_area), ARGV_BASE, 2, argv, 0, 0) == -1);
    CHECK(exec_args_build(g_area, sizeof(g_area), ARGV_BASE, 2, 0, 0, &rsp) == -1);
    CHECK(exec_args_build(g_area, 63u, ARGV_BASE, 2, argv, 0, &rsp) == -1);

    /* Phase U4: exec_args_ok is the predicate the syscall layer applies to
       argv copied out of user memory before it builds the block. */
    CHECK(exec_args_ok(0u, 0u) == 1);
    CHECK(exec_args_ok(2u, 17u) == 1);
    CHECK(exec_args_ok(EXEC_ARGS_MAX, EXEC_ARGS_STR_MAX) == 1);
    CHECK(exec_args_ok(EXEC_ARGS_MAX + 1u, 0u) == 0);
    CHECK(exec_args_ok(0xffffffffu, 0u) == 0);
    CHECK(exec_args_ok(0u, EXEC_ARGS_STR_MAX + 1ull) == 0);
    CHECK(exec_args_ok(2u, ~0ull) == 0);
}
