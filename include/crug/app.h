/* Phase 7A: application bundles (.app) and their tool types.

   An application on CrugOS is a directory, not an executable:

     /Applications/Hello.app/manifest.toml
     /Applications/Hello.app/bin/x86_64/hello

   The manifest is a deliberately small subset of TOML -- one section header
   per line, key = "value" pairs, '#' comments and blank lines.  Everything
   the kernel accepts is bounded (2048 bytes, 32-byte strings, eight tool
   types), so parsing an image that arrives from a disk or an installer can
   never run away.  kernel/app/manifest.c is pure and host tested;
   kernel/app/app.c only walks the mounted root and fills the table.
   See docs/apps.md. */
#ifndef CRUG_APP_H
#define CRUG_APP_H

#include <crug/types.h>

#define APP_DIR "/Applications"
#define APP_MAX 8u
#define APP_ID_MAX 32u
#define APP_NAME_MAX 32u
#define APP_VERSION_MAX 16u
#define APP_PATH_MAX 128u
#define APP_MANIFEST_MAX 2048u
#define APP_TOOLTYPE_MAX 8u
#define APP_TOOLTYPE_KEY_MAX 16u
#define APP_TOOLTYPE_VALUE_MAX 32u
#define APP_INFO_MAX 512u

/* The icon colour used when a manifest has no [icon] section. */
#define APP_DEFAULT_ICON_RGB 0x00A0A0A0u

struct app_tooltype {
    char key[APP_TOOLTYPE_KEY_MAX];
    char value[APP_TOOLTYPE_VALUE_MAX];
};

/* A parsed manifest.  prog is the bundle-relative path from [exec]. */
struct app_manifest {
    char id[APP_ID_MAX];
    char name[APP_NAME_MAX];
    char version[APP_VERSION_MAX];
    char min_os[APP_VERSION_MAX];
    char prog[APP_PATH_MAX];
    u32 icon_rgb;
    int has_icon;
    u8 tooltype_count;
    struct app_tooltype tooltypes[APP_TOOLTYPE_MAX];
};

/* A registered bundle: the manifest plus the absolute paths it lives at. */
struct app {
    struct app_manifest m;
    char dir[APP_PATH_MAX];   /* /Applications/Hello.app */
    char prog[APP_PATH_MAX];  /* /Applications/Hello.app/bin/x86_64/hello */
};

/* Pure manifest helpers (kernel/app/manifest.c).
   app_manifest_parse() returns 0 and fills *out, or -1 and sets *reason to a
   stable string from the rejection table: no-id, bad-id, dup-key, no-name,
   bad-name, no-prog, bad-prog, no-version, bad-version, min-os, bad-rgb,
   too-many-tooltypes, bad-tooltype, bad-entry. */
int app_manifest_parse(const char *text, usize len, struct app_manifest *out,
                       const char **reason);

/* base + '/' + rel into out; -1 when rel is absolute, contains '..', or the
   result does not fit.  An empty rel copies base. */
int app_path_join(char *out, usize cap, const char *base, const char *rel);

/* Compare N.N.N version strings: -1, 0, 1, or -2 when either is malformed. */
int app_version_compare(const char *a, const char *b);
/* 1 when a >= b, 0 when not, -2 when either string is malformed. */
int app_version_ge(const char *a, const char *b);

const struct app_tooltype *app_tooltype_find(const struct app_manifest *m,
                                             const char *key);

/* One text record per application for SYS_APP_INFO:
   id|name|version|prog|RRGGBB|KEY=value;KEY=value
   Returns the byte count written (no terminator counted), -1 when it does
   not fit. */
int app_format_info(const struct app_manifest *m, const char *prog,
                    char *out, usize cap);

/* The registry (kernel/app/app.c).  app_init() scans /Applications once,
   right after the root filesystem is mounted, and prints
   'CRUGOS: app registry ready (%u app(s), %u rejected)'.  Entries are kept
   in id order so the Workbench drawer and 'app list' are deterministic. */
void app_reset(void);
int app_init(void);
u32 app_count(void);
u32 app_rejected(void);
const struct app *app_at(u32 index);
const struct app *app_find_id(const char *id);
/* Spawn an application's program with argv[0] = the bundle directory.
   Returns its tid in *out_tid, or -1. */
int app_launch(const struct app *a, u32 parent_id, u32 *out_tid);
/* app_format_info() with the absolute program path, for SYS_APP_INFO. */
int app_info(const struct app *a, char *out, usize cap);

#endif /* CRUG_APP_H */