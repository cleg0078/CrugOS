/* Phase 7B: the package installer and capability boot selftests.
 *
 * `pkg.test=1` (make test-pkg) drives the kernel installer end to end on the
 * image scripts/mkdisk.sh built:
 *
 *   - /System/packages/captest.crpkg is signed by the key in
 *     /System/trusted/keys.conf, so pkg_probe reports it trusted;
 *   - /System/packages/untrusted.crpkg is signed by a key that is NOT in the
 *     trust list, so both the probe and the install must refuse it;
 *   - a successful install writes /Applications/captest.app, records it in
 *     /System/pkgdb and registers it in the package registry;
 *   - installing the same package twice is refused as already-installed;
 *   - re-scanning /Applications makes the installed bundle launchable.
 *
 * `pkg.test=2` (make test-caps) launches the same bundle through app_launch(),
 * so the child gets cap_intersect(manifest [caps], /System/perms.conf) == 
 * fs.read.  Its own probe proves the wall from ring 3: reading works, writing
 * and pkg.install are refused with SYS_EPERM, and it exits 5 only when every
 * check held.
 *
 * Every default boot leaves the token out, so nothing here runs and no marker
 * is printed.  See docs/packages.md. */
#include <crug/app.h>
#include <crug/caps.h>
#include <crug/ext4.h>
#include <crug/kernel.h>
#include <crug/perms.h>
#include <crug/pkg.h>
#include <crug/storage.h>
#include <crug/string.h>
#include <crug/thread.h>

#define PKG_TEST_ID "captest"
#define PKG_TEST_PATH PKG_DIR "/captest.crpkg"
#define PKG_TEST_BAD PKG_DIR "/untrusted.crpkg"
#define PKG_TEST_PROG "/Applications/captest.app/bin/x86_64/captest"
#define PKG_TEST_MANIFEST "/Applications/captest.app/manifest.toml"
/* The exit code user/captest.c uses when every capability check held. */
#define PKG_TEST_EXIT 5

static int g_pkg_test;

static int pkg_test_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/* Returns the level of a `pkg.test=<digit>` token, or 0 when it does not
   match. */
static int pkg_test_token(const char *tok, u32 len)
{
    static const char key[] = "pkg.test";
    u32 klen = (u32)sizeof(key) - 1u;

    if (len != klen + 2u || strncmp(tok, key, klen) != 0)
        return 0;
    if (tok[klen] != '=')
        return 0;
    if (tok[klen + 1u] < '1' || tok[klen + 1u] > '9')
        return 0;
    return (int)(tok[klen + 1u] - '0');
}

void pkg_test_cmdline(const struct boot_info *bi)
{
    const char *cmd;
    u32 i = 0;

    if (!bi || !bi->command_line)
        return;
    cmd = bi->command_line;
    while (cmd[i]) {
        u32 start;
        u32 len;
        int level;

        while (cmd[i] && pkg_test_space(cmd[i]))
            i++;
        start = i;
        while (cmd[i] && !pkg_test_space(cmd[i]))
            i++;
        len = i - start;
        level = pkg_test_token(cmd + start, len);
        if (level > g_pkg_test)
            g_pkg_test = level;
    }
}

static int pkg_test_failed(const char *what, const char *reason)
{
    kprintf("CRUGOS: pkg selftest failed (%s%s%s)\n", what,
            reason ? ": " : "", reason ? reason : "");
    return -1;
}

/* level 1: the installer path, driven from the kernel. */
static int pkg_test_install(void)
{
    struct pkg_header h;
    struct ext4_fs *fs = storage_present() ? storage_root() : 0;
    const struct pkg *installed;
    const char *reason = 0;
    int trusted = 0;
    u32 ino = 0u;

    kprintf("CRUGOS: pkg selftest start\n");
    memset(&h, 0, sizeof(h));
    if (pkg_probe(PKG_TEST_PATH, &h, &trusted, &reason) != 0 || !trusted)
        return pkg_test_failed("probe", reason);
    kprintf("CRUGOS: pkg selftest probe %s %s trusted\n", h.id, h.version);

    memset(&h, 0, sizeof(h));
    trusted = 0;
    reason = 0;
    if (pkg_probe(PKG_TEST_BAD, &h, &trusted, &reason) == 0 || trusted ||
        !reason || strcmp(reason, "untrusted-key") != 0)
        return pkg_test_failed("untrusted probe", reason);
    kprintf("CRUGOS: pkg selftest probe untrusted rejected (untrusted-key)\n");

    reason = 0;
    if (pkg_install(PKG_TEST_PATH, &reason) != 0)
        return pkg_test_failed("install", reason);
    installed = pkg_find_id(PKG_TEST_ID);
    if (!installed)
        return pkg_test_failed("registry", 0);
    if (!fs || ext4_lookup(fs, PKG_TEST_PROG, &ino) != 0 ||
        ext4_lookup(fs, PKG_TEST_MANIFEST, &ino) != 0)
        return pkg_test_failed("installed files", 0);
    kprintf("CRUGOS: pkg selftest installed %s %s\n", installed->id,
            installed->version);

    reason = 0;
    if (pkg_install(PKG_TEST_PATH, &reason) == 0 || !reason ||
        strcmp(reason, "already-installed") != 0)
        return pkg_test_failed("second install", reason);
    kprintf("CRUGOS: pkg selftest rejected second install (already-installed)\n");

    reason = 0;
    if (pkg_install(PKG_TEST_BAD, &reason) == 0 || !reason ||
        strcmp(reason, "untrusted-key") != 0)
        return pkg_test_failed("untrusted install", reason);
    kprintf("CRUGOS: pkg selftest rejected untrusted (untrusted-key)\n");

    (void)app_init();
    if (!app_find_id(PKG_TEST_ID))
        return pkg_test_failed("app registry", 0);
    kprintf("CRUGOS: pkg selftest app registered %s\n", PKG_TEST_ID);
    kprintf("CRUGOS: pkg selftest ok (1 installed, 2 rejected)\n");
    return 0;
}

/* level 2: the capability wall, through the real bundle launch path. */
static int pkg_test_caps(void)
{
    const struct app *a;
    const char *reason = 0;
    struct thread *t;
    u32 granted;
    u32 tid = 0u;
    i64 code;

    kprintf("CRUGOS: caps selftest start\n");
    if (!pkg_find_id(PKG_TEST_ID)) {
        if (pkg_install(PKG_TEST_PATH, &reason) != 0)
            return pkg_test_failed("install", reason);
    }
    (void)app_init();
    a = app_find_id(PKG_TEST_ID);
    if (!a)
        return pkg_test_failed("app registry", 0);
    granted = cap_intersect(a->m.cap_mask,
                            perms_granted(perms_current(), a->m.id));
    kprintf("CRUGOS: caps selftest manifest 0x%x policy 0x%x granted 0x%x\n",
            (unsigned)a->m.cap_mask,
            (unsigned)perms_granted(perms_current(), a->m.id),
            (unsigned)granted);
    if (granted != CAP_FS_READ)
        return pkg_test_failed("granted mask", 0);
    if (app_launch(a, 0u, &tid) != 0)
        return pkg_test_failed("launch", 0);
    t = thread_find(tid);
    if (!t)
        return pkg_test_failed("child thread", 0);
    kprintf("CRUGOS: caps selftest launched tid %u caps 0x%x\n",
            (unsigned)tid, (unsigned)t->caps);
    if (t->caps != CAP_FS_READ)
        return pkg_test_failed("child caps", 0);
    code = thread_join_code(t);
    if (code != PKG_TEST_EXIT) {
        kprintf("CRUGOS: caps selftest failed (exit %lld)\n", code);
        return -1;
    }
    kprintf("CRUGOS: caps selftest ok (exit %lld, caps 0x%x)\n", code,
            (unsigned)CAP_FS_READ);
    return 0;
}

int pkg_selftest(void)
{
    if (g_pkg_test == 1)
        return pkg_test_install();
    if (g_pkg_test == 2)
        return pkg_test_caps();
    return 0;
}
