/* Phase 7B: capability names, masks and their text form.
   Pure functions: kernel/app/perms.c, the manifest parser, the syscall layer
   and the Workbench all share these tables.  See include/crug/caps.h. */
#include <crug/caps.h>
#include <crug/string.h>

struct cap_def {
    const char *name;
    u32 bit;
};

static const struct cap_def cap_table[CAP_COUNT] = {
    { "fs.read", CAP_FS_READ },
    { "fs.write", CAP_FS_WRITE },
    { "app.launch", CAP_APP_LAUNCH },
    { "net", CAP_NET },
    { "audio", CAP_AUDIO },
    { "pkg.install", CAP_PKG_INSTALL }
};

static int cap_lower(char c)
{
    return c >= 'a' && c <= 'z';
}

static int cap_dot_or_lower(char c)
{
    return cap_lower(c) || c == '.';
}

const char *cap_name(u32 bit)
{
    u32 i;

    for (i = 0u; i < CAP_COUNT; i++) {
        if (cap_table[i].bit == bit) {
            return cap_table[i].name;
        }
    }
    return 0;
}

u32 cap_from_name(const char *name)
{
    u32 i;

    if (!name) {
        return 0u;
    }
    for (i = 0u; i < CAP_COUNT; i++) {
        if (strcmp(cap_table[i].name, name) == 0) {
            return cap_table[i].bit;
        }
    }
    return 0u;
}

static int cap_token(const char *s, usize n, u32 *out)
{
    char tmp[CAP_NAME_MAX];
    usize i;

    if (n == 0u || n >= sizeof(tmp)) {
        return -1;
    }
    for (i = 0u; i < n; i++) {
        if (!cap_dot_or_lower(s[i])) {
            return -1;
        }
        tmp[i] = s[i];
    }
    tmp[n] = 0;
    *out = cap_from_name(tmp);
    return *out == 0u ? -1 : 0;
}

int cap_parse_list(const char *list, usize len, u32 *out, char *bad,
                   usize bad_cap)
{
    usize i = 0u;
    u32 mask = 0u;

    if (!out) {
        return -1;
    }
    while (i < len) {
        usize s;
        usize e;
        u32 bit = 0u;

        while (i < len && (list[i] == ',' || list[i] == ' ' ||
                           list[i] == 9)) {
            i++;
        }
        if (i >= len) {
            break;
        }
        s = i;
        while (i < len && list[i] != ',' && list[i] != ' ' && list[i] != 9) {
            i++;
        }
        e = i;
        if (cap_token(list + s, e - s, &bit) != 0) {
            if (bad && bad_cap != 0u) {
                usize n = e - s;

                if (n >= bad_cap) {
                    n = bad_cap - 1u;
                }
                memcpy(bad, list + s, n);
                bad[n] = 0;
            }
            return -1;
        }
        mask |= bit;
    }
    *out = mask;
    return 0;
}

int cap_format(u32 mask, char *out, usize cap)
{
    usize n = 0u;
    u32 i;

    if (!out || cap == 0u) {
        return -1;
    }
    if ((mask & CAP_ALL) == 0u) {
        const char *none = "none";

        if (cap < 5u) {
            return -1;
        }
        memcpy(out, none, 4u);
        out[4] = 0;
        return 4;
    }
    for (i = 0u; i < CAP_COUNT; i++) {
        const char *name;
        usize len;

        if ((mask & cap_table[i].bit) == 0u) {
            continue;
        }
        name = cap_table[i].name;
        len = strlen(name);
        if (n != 0u) {
            if (n + 1u >= cap) {
                return -1;
            }
            out[n++] = ',';
        }
        if (n + len >= cap) {
            return -1;
        }
        memcpy(out + n, name, len);
        n += len;
    }
    out[n] = 0;
    return (int)n;
}

u32 cap_intersect(u32 a, u32 b)
{
    return a & b & CAP_ALL;
}
