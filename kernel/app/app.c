/* Phase 7A: the .app bundle registry.

   app_init() scans /Applications once, right after the root filesystem is
   mounted and before the scheduler starts, so a bundle can never appear
   half-registered.  Every entry is <Name>.app holding manifest.toml and a
   bundled executable; the manifest is parsed by the pure parser in
   kernel/app/manifest.c and one bad bundle is skipped with a printed reason
   instead of failing the boot.  Static storage only: eight bundles, two
   KiB of manifest text, no heap.  Launches go through elf_spawn_user() with
   argv[0] set to the bundle directory so an application can find its own
   files.  See include/crug/app.h. */
#include <crug/app.h>
#include <crug/caps.h>
#include <crug/exec.h>
#include <crug/ext4.h>
#include <crug/kernel.h>
#include <crug/perms.h>
#include <crug/storage.h>
#include <crug/string.h>

#define APP_LIST_MAX 1024u
#define APP_NAME_SUFFIX ".app"
#define APP_NAME_SUFFIX_LEN 4u

static struct app g_apps[APP_MAX];
static u32 g_count;
static u32 g_rejected;
static int g_ready;
static char g_list_buf[APP_LIST_MAX];
static char g_manifest_buf[APP_MANIFEST_MAX + 1u];

void app_reset(void)
{
    memset(g_apps, 0, sizeof(g_apps));
    g_count = 0u;
    g_rejected = 0u;
    g_ready = 0;
}

static int app_ends_with_app(const char *name)
{
    usize n = strlen(name);

    if (n <= APP_NAME_SUFFIX_LEN) {
        return 0;
    }
    return strcmp(name + (n - APP_NAME_SUFFIX_LEN), APP_NAME_SUFFIX) == 0;
}

static void app_reject(const char *entry, const char *reason)
{
    g_rejected++;
    kprintf("CRUGOS: app rejected %s (%s)\n", entry,
            reason ? reason : "bad-entry");
}

static const struct app *app_by_id(const char *id)
{
    u32 i;

    for (i = 0u; i < g_count; i++) {
        if (strcmp(g_apps[i].m.id, id) == 0) {
            return &g_apps[i];
        }
    }
    return 0;
}

static const struct app *app_by_name(const char *name)
{
    u32 i;

    for (i = 0u; i < g_count; i++) {
        if (strcmp(g_apps[i].m.name, name) == 0) {
            return &g_apps[i];
        }
    }
    return 0;
}

/* Insertion sort by id so listing order does not depend on directory order. */
static int app_register(const struct app *candidate, const char **reason)
{
    struct app copy;
    u32 pos = 0u;
    u32 i;

    if (!candidate || !reason) {
        return -1;
    }
    if (g_count >= APP_MAX) {
        *reason = "registry-full";
        return -1;
    }
    if (app_by_id(candidate->m.id) != 0) {
        *reason = "dup-id";
        return -1;
    }
    if (app_by_name(candidate->m.name) != 0) {
        *reason = "dup-name";
        return -1;
    }
    memcpy(&copy, candidate, sizeof(copy));
    while (pos < g_count && strcmp(g_apps[pos].m.id, copy.m.id) < 0) {
        pos++;
    }
    for (i = g_count; i > pos; i--) {
        memcpy(&g_apps[i], &g_apps[i - 1u], sizeof(g_apps[i]));
    }
    memcpy(&g_apps[pos], &copy, sizeof(g_apps[pos]));
    g_count++;
    return 0;
}

static int app_load_bundle(struct ext4_fs *fs, const char *entry)
{
    struct app_manifest manifest;
    struct app candidate;
    const char *reason = 0;
    char dir[APP_PATH_MAX];
    char manifest_path[APP_PATH_MAX];
    char prog_path[APP_PATH_MAX];
    u32 ino = 0u;
    u64 size = 0u;
    int n;

    memset(&candidate, 0, sizeof(candidate));
    if (app_path_join(dir, sizeof(dir), APP_DIR, entry) != 0) {
        app_reject(entry, "bad-entry");
        return 0;
    }
    if (app_path_join(manifest_path, sizeof(manifest_path), dir,
                      "manifest.toml") != 0) {
        app_reject(entry, "bad-entry");
        return 0;
    }
    if (ext4_lookup(fs, manifest_path, &ino) != 0) {
        app_reject(entry, "no-manifest");
        return 0;
    }
    n = ext4_read_file(fs, manifest_path, g_manifest_buf,
                       APP_MANIFEST_MAX, &size);
    if (n != 0) {
        app_reject(entry, "manifest-too-large");
        return 0;
    }
    if (size == 0u || size > APP_MANIFEST_MAX) {
        app_reject(entry, size == 0u ? "no-manifest" :
                                    "manifest-too-large");
        return 0;
    }
    g_manifest_buf[size] = 0;
    if (app_manifest_parse(g_manifest_buf, (usize)size, &manifest,
                           &reason) != 0) {
        app_reject(entry, reason);
        return 0;
    }
    memcpy(&candidate.m, &manifest, sizeof(candidate.m));
    memcpy(candidate.dir, dir, sizeof(candidate.dir));
    if (app_path_join(prog_path, sizeof(prog_path), dir,
                      manifest.prog) != 0) {
        app_reject(entry, "bad-prog");
        return 0;
    }
    memcpy(candidate.prog, prog_path, sizeof(candidate.prog));
    if (ext4_lookup(fs, candidate.prog, &ino) != 0) {
        app_reject(entry, "bad-prog");
        return 0;
    }
    if (app_register(&candidate, &reason) != 0) {
        app_reject(entry, reason);
        return 0;
    }
    return 1;
}

int app_init(void)
{
    struct ext4_fs *fs;
    usize i = 0u;
    int n;
    int truncated = 0;

    app_reset();
    fs = storage_root();
    if (!storage_present() || !fs) {
        g_ready = 1;
        kprintf("CRUGOS: app registry ready (0 app(s), 0 rejected)\n");
        return 0;
    }
    n = ext4_list(fs, APP_DIR, g_list_buf, APP_LIST_MAX);
    if (n <= 0) {
        g_ready = 1;
        kprintf("CRUGOS: app registry ready (0 app(s), 0 rejected)\n");
        return 0;
    }
    if ((usize)n >= APP_LIST_MAX) {
        n = (int)APP_LIST_MAX;
        truncated = 1;
    }
    while (i < (usize)n) {
        char entry[APP_NAME_MAX + 8u];
        usize start = i;
        usize len;

        while (i < (usize)n && g_list_buf[i] != 10) {
            i++;
        }
        len = i - start;
        if (i < (usize)n) {
            i++;
        } else if (truncated) {
            break;
        }
        if (len == 0u) {
            continue;
        }
        if (len >= sizeof(entry)) {
            g_rejected++;
            kprintf("CRUGOS: app rejected %.40s (bad-entry)\n",
                    g_list_buf + start);
            continue;
        }
        memcpy(entry, g_list_buf + start, len);
        entry[len] = 0;
        if (!app_ends_with_app(entry)) {
            continue;
        }
        (void)app_load_bundle(fs, entry);
    }
    g_ready = 1;
    kprintf("CRUGOS: app registry ready (%u app(s), %u rejected)\n",
            (unsigned)g_count, (unsigned)g_rejected);
    return 0;
}

u32 app_count(void)
{
    return g_count;
}

u32 app_rejected(void)
{
    return g_rejected;
}

const struct app *app_at(u32 index)
{
    if (index >= g_count) {
        return 0;
    }
    return &g_apps[index];
}

const struct app *app_find_id(const char *id)
{
    if (!id || !id[0]) {
        return 0;
    }
    return app_by_id(id);
}

int app_launch(const struct app *a, u32 parent_id, u32 *out_tid)
{
    struct elf_exec_result result;
    const char *argv[2];
    struct thread *t;

    if (!a || a->prog[0] == 0 || a->dir[0] == 0) {
        return -1;
    }
    argv[0] = a->dir;
    argv[1] = 0;
    memset(&result, 0, sizeof(result));
    /* Phase 7B: the granted set is the manifest's requirement intersected
       with the policy, so neither alone can widen what the bundle gets. */
    t = elf_spawn_user_caps(a->prog, 1u, argv, parent_id,
                            cap_intersect(a->m.cap_mask,
                                          perms_granted(perms_current(),
                                                        a->m.id)),
                            &result);
    if (!t) {
        return -1;
    }
    if (out_tid) {
        *out_tid = result.tid;
    }
    return 0;
}

int app_info(const struct app *a, char *out, usize cap)
{
    if (!a) {
        return -1;
    }
    return app_format_info(&a->m, a->prog, out, cap);
}