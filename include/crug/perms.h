/* Phase 7B: the capability policy for bundled applications.

   /System/perms.conf is a flat text file:

     # bundle id            capabilities
     default                fs.read,app.launch
     com.crugos.orbit       fs.read,net

   The `default` line applies to every bundle without its own line; a bundle
   line replaces the default for that id (it is not additive).  The granted
   set is then intersected with the capabilities the bundle's manifest
   requires, so a policy that grants more than a manifest asks for can never
   widen what the application gets.

   When the file is missing the whole policy is the historical, fully
   privileged default (CAP_ALL) so a system installed before phase 7B keeps
   working; a file that exists but does not parse is treated as denying
   everything.  See docs/packages.md. */
#ifndef CRUG_PERMS_H
#define CRUG_PERMS_H

#include <crug/types.h>

#define PERMS_PATH "/System/perms.conf"
#define PERMS_MAX_RULES 8u
#define PERMS_ID_MAX 32u
#define PERMS_DEFAULT_ID "default"

struct perms_rule {
    char id[PERMS_ID_MAX];
    u32 mask;
};

struct perms_policy {
    u32 default_mask;
    int has_default;
    u32 count;
    struct perms_rule rules[PERMS_MAX_RULES];
};

/* Parse the policy text.  Returns 0 and fills *out, or -1 with *reason set
   ('perms-line', 'perms-id', 'perms-cap', 'perms-dup', 'perms-full'). */
int perms_parse(const char *text, usize len, struct perms_policy *out,
                const char **reason);

/* The mask granted to bundle ID: its own rule, else the default, else
   CAP_NONE.  NULL or an empty id takes the default. */
u32 perms_granted(const struct perms_policy *pol, const char *id);

/* Kernel side: read /System/perms.conf into OUT.  A missing file yields the
   privileged default; a damaged file yields CAP_NONE and prints the reason.
   Returns 0, or -1 when the file exists but does not parse. */
int perms_load(struct perms_policy *out);

/* Loaded once after the application registry is scanned and prints
   'CRUGOS: perms policy ready (default <caps>, N rule(s))'. */
void perms_init(void);
const struct perms_policy *perms_current(void);

#endif /* CRUG_PERMS_H */
