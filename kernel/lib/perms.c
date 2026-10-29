/* Phase 7B: the /System/perms.conf capability policy. */
#include <crug/perms.h>

#include <crug/caps.h>
#include <crug/ext4.h>
#include <crug/kernel.h>
#include <crug/storage.h>
#include <crug/string.h>

#define PERMS_BUF_MAX 1024u

static struct perms_policy g_policy;
static char g_buf[PERMS_BUF_MAX];
static int g_loaded;

static int perms_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static int perms_id_char_ok(char c)
{
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9')) {
        return 1;
    }
    return c == '.' || c == '-' || c == '_';
}

int perms_parse(const char *text, usize len, struct perms_policy *out,
                const char **reason)
{
    usize i = 0u;

    if (!text || !out) {
        if (reason) {
            *reason = "perms-line";
        }
        return -1;
    }
    memset(out, 0, sizeof(*out));
    while (i < len) {
        usize start = i;
        usize end;
        usize j;
        usize id_len;
        u32 mask = 0u;

        while (i < len && text[i] != 10) {
            i++;
        }
        end = i;
        if (i < len) {
            i++;
        }
        while (start < end && perms_space(text[start])) {
            start++;
        }
        while (end > start && perms_space(text[end - 1u])) {
            end--;
        }
        if (start == end || text[start] == '#') {
            continue;
        }
        j = start;
        while (j < end && !perms_space(text[j])) {
            j++;
        }
        id_len = j - start;
        if (id_len == 0u || id_len >= PERMS_ID_MAX) {
            if (reason) {
                *reason = "perms-id";
            }
            return -1;
        }
        {
            usize k;

            for (k = start; k < j; k++) {
                if (!perms_id_char_ok(text[k])) {
                    if (reason) {
                        *reason = "perms-id";
                    }
                    return -1;
                }
            }
        }
        while (j < end && perms_space(text[j])) {
            j++;
        }
        {
            char bad[CAP_NAME_MAX];

            if (cap_parse_list(text + j, end - j, &mask, bad,
                               sizeof(bad)) != 0) {
                if (reason) {
                    *reason = "perms-cap";
                }
                return -1;
            }
        }
        if (id_len == 7u &&
            strncmp(text + start, PERMS_DEFAULT_ID, 7u) == 0) {
            if (out->has_default) {
                if (reason) {
                    *reason = "perms-dup";
                }
                return -1;
            }
            out->default_mask = mask;
            out->has_default = 1;
            continue;
        }
        if (out->count >= PERMS_MAX_RULES) {
            if (reason) {
                *reason = "perms-full";
            }
            return -1;
        }
        memcpy(out->rules[out->count].id, text + start, id_len);
        out->rules[out->count].id[id_len] = 0;
        out->rules[out->count].mask = mask;
        out->count++;
    }
    return 0;
}

u32 perms_granted(const struct perms_policy *pol, const char *id)
{
    u32 i;

    if (!pol) {
        return CAP_NONE;
    }
    if (id && id[0]) {
        for (i = 0u; i < pol->count; i++) {
            if (strcmp(pol->rules[i].id, id) == 0) {
                return pol->rules[i].mask;
            }
        }
    }
    return pol->has_default ? pol->default_mask : CAP_NONE;
}

int perms_load(struct perms_policy *out)
{
    struct ext4_fs *fs = storage_present() ? storage_root() : 0;
    const char *reason = 0;
    u64 size = 0u;

    if (!out) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    out->default_mask = CAP_ALL;
    out->has_default = 1;
    if (!fs) {
        return 0;
    }
    if (ext4_read_file(fs, PERMS_PATH, g_buf, PERMS_BUF_MAX - 1u, &size) != 0) {
        return 0;
    }
    if (size >= PERMS_BUF_MAX) {
        kprintf("CRUGOS: perms policy error (perms-line)\n");
        out->default_mask = CAP_NONE;
        return -1;
    }
    g_buf[size] = 0;
    if (perms_parse(g_buf, (usize)size, out, &reason) != 0) {
        kprintf("CRUGOS: perms policy error (%s)\n",
                reason ? reason : "perms-line");
        memset(out, 0, sizeof(*out));
        out->has_default = 1;
        return -1;
    }
    return 0;
}

void perms_init(void)
{
    char caps[64];

    (void)perms_load(&g_policy);
    g_loaded = 1;
    if (cap_format(g_policy.default_mask, caps, sizeof(caps)) < 0) {
        caps[0] = 0;
    }
    kprintf("CRUGOS: perms policy ready (default %s, %u rule(s))\n", caps,
            (unsigned)g_policy.count);
}

const struct perms_policy *perms_current(void)
{
    if (!g_loaded) {
        return 0;
    }
    return &g_policy;
}
