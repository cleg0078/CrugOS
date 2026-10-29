/* CrugOS capability probe (phase 7B).
 *
 * The package installer launches this bundle through app_launch(), so the
 * kernel hands it only the intersection of the manifest's [caps] require list
 * and /System/perms.conf: fs.read and nothing else.  The probe proves the wall
 * from the user side -- it reads a file, is refused a write, is refused a
 * package install, and reads its own mask back through SYS_CAP_QUERY -- and
 * then exits with a code the boot selftest asserts (5).  An unexpected
 * success exits with a distinct code, so the gate reports which wall broke.
 */
#include "lib.h"

#define CAPTEST_OK 5
#define CAPTEST_QUERY 1
#define CAPTEST_CHECK 6
#define CAPTEST_READ 7
#define CAPTEST_WRITE 8
#define CAPTEST_PKG 9
#define CAPTEST_REMOVE 10
#define CAPTEST_SPAWN 11
#define CAPTEST_EXEC 12

static int fail(const char *what, int code)
{
    u_puts("CRUGOS: captest failed (");
    u_puts(what);
    u_puts(")\n");
    return code;
}

int main(int argc, char **argv, char **envp)
{
    char probe[16];
    i64 caps;
    i64 fd;
    i64 n;

    (void)argc;
    (void)argv;
    (void)envp;
    caps = u_cap_query();
    if (caps < 0)
        return fail("cap query", CAPTEST_QUERY);
    u_puts("CRUGOS: captest caps ");
    u_puthex((u64)caps);
    u_puts("\n");
    if ((caps & (i64)U_CAP_FS_READ) == 0)
        return fail("missing fs.read", CAPTEST_CHECK);
    if ((caps & (i64)U_CAP_FS_WRITE) != 0)
        return fail("unexpected fs.write", CAPTEST_CHECK);
    if (u_cap_check(U_CAP_FS_READ) != 1)
        return fail("cap check fs.read", CAPTEST_CHECK);
    if (u_cap_check(U_CAP_FS_WRITE) != 0)
        return fail("cap check fs.write", CAPTEST_CHECK);
    u_puts("CRUGOS: captest cap check ok\n");

    /* fs.read really works: the hello file the image ships. */
    fd = u_open("/hello.txt", U_O_RDONLY, 0);
    if (fd < 0)
        return fail("read open", CAPTEST_READ);
    n = u_read((u32)fd, probe, (u32)sizeof(probe));
    (void)u_close((u32)fd);
    if (n <= 0)
        return fail("read data", CAPTEST_READ);
    u_puts("CRUGOS: captest read ok\n");

    /* fs.write is not held, so the kernel must answer SYS_EPERM before it
       even resolves the path. */
    fd = u_open("/System/captest-denied", U_O_WRONLY | U_O_CREAT | U_O_TRUNC,
                420);
    if (fd >= 0) {
        (void)u_close((u32)fd);
        return fail("write allowed", CAPTEST_WRITE);
    }
    u_puts("CRUGOS: captest denied fs.write\n");

    /* Nor pkg.install: the same path the installer used is refused here. */
    if (u_pkg_install("/System/packages/captest.crpkg") >= 0)
        return fail("package install allowed", CAPTEST_PKG);
    u_puts("CRUGOS: captest denied pkg.install\n");

    /* Phase 7C: removal is the same capability, and spawning or exec'ing is
       CAP_APP_LAUNCH, so a narrowed bundle cannot regain its mask that way
       either. */
    if (u_pkg_remove("captest") >= 0)
        return fail("package remove allowed", CAPTEST_REMOVE);
    u_puts("CRUGOS: captest denied pkg.remove\n");
    if (u_spawn("/bin/hello", 0, 0) >= 0)
        return fail("spawn allowed", CAPTEST_SPAWN);
    u_puts("CRUGOS: captest denied spawn\n");
    if (u_exec("/bin/hello", 0, 0) >= 0)
        return fail("exec allowed", CAPTEST_EXEC);
    u_puts("CRUGOS: captest denied exec\n");
    if (u_pkg_count() < 0)
        return fail("pkg count", CAPTEST_PKG);
    u_puts("CRUGOS: captest ok\n");
    return CAPTEST_OK;
}
