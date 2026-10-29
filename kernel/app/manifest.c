/* Phase 7A: the bounded .app manifest parser.

   Pure functions only -- no storage, no heap, no globals -- so the host test
   can feed it hostile text without booting.  The grammar is one construct per
   line: [section], key = "value", '#' comments and blank lines.  Unknown
   sections, unknown keys and trailing junk are refused instead of ignored,
   because a manifest that silently loses a tool type would launch an
   application with the wrong configuration.  See include/crug/app.h. */
#include <crug/app.h>
#include <crug/string.h>
#include <crug/version.h>

/* Sections of the manifest. */
#define SEC_NONE 0
#define SEC_APP 1
#define SEC_EXEC 2
#define SEC_TOOLTYPES 3
#define SEC_ICON 4

static int app_space(char c)
{
    return c == ' ' || c == 9 || c == 13;
}

static int app_lower(char c)
{
    return c >= 'a' && c <= 'z';
}

static int app_digit(char c)
{
    return c >= '0' && c <= '9';
}

static int app_upper(char c)
{
    return c >= 'A' && c <= 'Z';
}

static int app_key_char(char c)
{
    return app_lower(c) || app_upper(c) || app_digit(c) || c == '_' || c == '-' ||
           c == '.';
}

static int app_id_char(char c)
{
    return app_lower(c) || app_digit(c) || c == '-' || c == '_' || c == '.';
}

/* Printable ASCII minus the quote that would end a value. */
static int app_text_char(char c)
{
    return c >= 32 && c <= 126 && c != '"';
}

static int app_hex_digit(char c, u32 *value)
{
    if (app_digit(c)) {
        *value = (u32)(c - '0');
        return 0;
    }
    if (c >= 'a' && c <= 'f') {
        *value = (u32)(c - 'a') + 10u;
        return 0;
    }
    if (c >= 'A' && c <= 'F') {
        *value = (u32)(c - 'A') + 10u;
        return 0;
    }
    return -1;
}

static int app_slice_eq(const char *s, usize n, const char *word)
{
    usize i = 0;

    while (i < n && word[i] && s[i] == word[i]) {
        i++;
    }
    return i == n && word[i] == 0;
}

static int app_copy(char *dst, usize cap, const char *src, usize n)
{
    if (n >= cap) {
        return -1;
    }
    memcpy(dst, src, n);
    dst[n] = 0;
    return 0;
}

static int app_all(const char *s, usize n, int (*ok)(char))
{
    usize i;

    if (n == 0u) {
        return 0;
    }
    for (i = 0u; i < n; i++) {
        if (!ok(s[i])) {
            return 0;
        }
    }
    return 1;
}

static int app_reason(const char **reason, const char *text)
{
    if (reason) {
        *reason = text;
    }
    return -1;
}

static int app_version_parse(const char *s, u32 out[3])
{
    int part = 0;

    while (part < 3) {
        u32 value = 0u;
        int digits = 0;

        while (app_digit(*s) && digits < 6) {
            value = value * 10u + (u32)(*s - '0');
            s++;
            digits++;
        }
        if (digits == 0) {
            return -1;
        }
        out[part++] = value;
        if (*s == '.') {
            s++;
            continue;
        }
        break;
    }
    if (*s != 0 || part != 3) {
        return -1;
    }
    return 0;
}

int app_version_compare(const char *a, const char *b)
{
    u32 va[3];
    u32 vb[3];
    int i;

    if (!a || !b || app_version_parse(a, va) != 0 ||
        app_version_parse(b, vb) != 0) {
        return -2;
    }
    for (i = 0; i < 3; i++) {
        if (va[i] < vb[i]) {
            return -1;
        }
        if (va[i] > vb[i]) {
            return 1;
        }
    }
    return 0;
}

int app_version_ge(const char *a, const char *b)
{
    int c = app_version_compare(a, b);

    return c == -2 ? -2 : (c >= 0 ? 1 : 0);
}

const struct app_tooltype *app_tooltype_find(const struct app_manifest *m,
                                             const char *key)
{
    u32 i;

    if (!m || !key) {
        return 0;
    }
    for (i = 0u; i < (u32)m->tooltype_count && i < APP_TOOLTYPE_MAX; i++) {
        if (strcmp(m->tooltypes[i].key, key) == 0) {
            return &m->tooltypes[i];
        }
    }
    return 0;
}

int app_path_join(char *out, usize cap, const char *base, const char *rel)
{
    usize bl;
    usize rl;
    usize n;

    if (!out || cap == 0u || !base) {
        return -1;
    }
    bl = strlen(base);
    if (bl == 0u || bl >= cap) {
        return -1;
    }
    memcpy(out, base, bl);
    n = bl;
    if (!rel || rel[0] == 0) {
        out[n] = 0;
        return 0;
    }
    if (rel[0] == '/' || strstr(rel, "..") != 0) {
        return -1;
    }
    rl = strlen(rel);
    if (n + 1u + rl >= cap) {
        return -1;
    }
    out[n++] = '/';
    memcpy(out + n, rel, rl);
    n += rl;
    out[n] = 0;
    return 0;
}

int app_manifest_parse(const char *text, usize len, struct app_manifest *out,
                       const char **reason)
{
    int section = SEC_NONE;
    int seen_id = 0;
    int seen_name = 0;
    int seen_version = 0;
    int seen_min_os = 0;
    int seen_prog = 0;
    usize i = 0;

    if (reason) {
        *reason = "bad-entry";
    }
    if (!text || !out || len == 0u) {
        return app_reason(reason, "bad-entry");
    }
    if (len > APP_MANIFEST_MAX) {
        return app_reason(reason, "manifest-too-large");
    }
    memset(out, 0, sizeof(*out));
    out->icon_rgb = APP_DEFAULT_ICON_RGB;
    while (i < len) {
        usize s = i;
        usize e;
        usize k;
        usize ke;
        usize v;
        usize vs;
        usize ve;
        usize t;
        usize j;

        while (i < len && text[i] != 10) {
            i++;
        }
        e = i;
        if (i < len) {
            i++;
        }
        while (s < e && app_space(text[s])) {
            s++;
        }
        while (e > s && app_space(text[e - 1u])) {
            e--;
        }
        if (s == e || text[s] == '#') {
            continue;
        }
        if (text[s] == '[') {
            usize name = s + 1u;
            usize close = name;

            while (close < e && text[close] != ']') {
                close++;
            }
            if (close >= e || close + 1u != e) {
                return app_reason(reason, "bad-entry");
            }
            if (app_slice_eq(text + name, close - name, "app")) {
                section = SEC_APP;
            } else if (app_slice_eq(text + name, close - name, "exec")) {
                section = SEC_EXEC;
            } else if (app_slice_eq(text + name, close - name,
                                    "tooltypes")) {
                section = SEC_TOOLTYPES;
            } else if (app_slice_eq(text + name, close - name, "icon")) {
                section = SEC_ICON;
            } else {
                return app_reason(reason, "bad-entry");
            }
            continue;
        }
        k = s;
        while (k < e && text[k] != '=') {
            k++;
        }
        if (k == e || k == s) {
            return app_reason(reason, "bad-entry");
        }
        /* Trim the padding between the key and its '='. */
        ke = k;
        while (ke > s && app_space(text[ke - 1u])) {
            ke--;
        }
        if (ke == s) {
            return app_reason(reason, "bad-entry");
        }
        for (j = s; j < ke; j++) {
            if (!app_key_char(text[j])) {
                return app_reason(reason, "bad-entry");
            }
        }
        v = k + 1u;
        while (v < e && app_space(text[v])) {
            v++;
        }
        if (v >= e || text[v] != '"') {
            return app_reason(reason, "bad-entry");
        }
        v++;
        vs = v;
        while (v < e && text[v] != '"') {
            v++;
        }
        if (v >= e) {
            return app_reason(reason, "bad-entry");
        }
        ve = v;
        t = v + 1u;
        while (t < e && app_space(text[t])) {
            t++;
        }
        if (t != e || section == SEC_NONE) {
            return app_reason(reason, "bad-entry");
        }
        if (section == SEC_APP) {
            if (app_slice_eq(text + s, ke - s, "id")) {
                if (seen_id) {
                    return app_reason(reason, "dup-key");
                }
                if (!app_all(text + vs, ve - vs, app_id_char)) {
                    return app_reason(reason, "bad-id");
                }
                if (app_copy(out->id, APP_ID_MAX, text + vs, ve - vs) != 0) {
                    return app_reason(reason, "bad-id");
                }
                seen_id = 1;
            } else if (app_slice_eq(text + s, ke - s, "name")) {
                if (seen_name) {
                    return app_reason(reason, "dup-key");
                }
                if (!app_all(text + vs, ve - vs, app_text_char)) {
                    return app_reason(reason, "bad-name");
                }
                if (app_copy(out->name, APP_NAME_MAX, text + vs,
                             ve - vs) != 0) {
                    return app_reason(reason, "bad-name");
                }
                seen_name = 1;
            } else if (app_slice_eq(text + s, ke - s, "version")) {
                char tmp[APP_VERSION_MAX];

                if (seen_version) {
                    return app_reason(reason, "dup-key");
                }
                if (app_copy(tmp, sizeof(tmp), text + vs, ve - vs) != 0 ||
                    app_version_compare(tmp, tmp) != 0) {
                    return app_reason(reason, "bad-version");
                }
                memcpy(out->version, tmp, sizeof(tmp));
                seen_version = 1;
            } else if (app_slice_eq(text + s, ke - s, "min_os")) {
                char tmp[APP_VERSION_MAX];

                if (seen_min_os) {
                    return app_reason(reason, "dup-key");
                }
                if (app_copy(tmp, sizeof(tmp), text + vs, ve - vs) != 0 ||
                    app_version_compare(tmp, tmp) != 0) {
                    return app_reason(reason, "bad-version");
                }
                memcpy(out->min_os, tmp, sizeof(tmp));
                seen_min_os = 1;
            } else {
                return app_reason(reason, "bad-entry");
            }
        } else if (section == SEC_EXEC) {
            if (!app_slice_eq(text + s, ke - s, "prog")) {
                return app_reason(reason, "bad-entry");
            }
            if (seen_prog) {
                return app_reason(reason, "dup-key");
            }
            if (ve == vs || text[vs] == '/' ||
                !app_all(text + vs, ve - vs, app_text_char) ||
                app_copy(out->prog, APP_PATH_MAX, text + vs, ve - vs) != 0 ||
                strstr(out->prog, "..") != 0) {
                return app_reason(reason, "bad-prog");
            }
            seen_prog = 1;
        } else if (section == SEC_ICON) {
            u32 rgb = 0u;
            usize h;

            if (!app_slice_eq(text + s, ke - s, "rgb")) {
                return app_reason(reason, "bad-entry");
            }
            if (out->has_icon) {
                return app_reason(reason, "dup-key");
            }
            if (ve - vs != 6u) {
                return app_reason(reason, "bad-rgb");
            }
            for (h = vs; h < ve; h++) {
                u32 nibble = 0u;

                if (app_hex_digit(text[h], &nibble) != 0) {
                    return app_reason(reason, "bad-rgb");
                }
                rgb = (rgb << 4) | nibble;
            }
            out->icon_rgb = rgb;
            out->has_icon = 1;
        } else {
            u32 slen = (u32)(ke - s);
            u32 vlen = (u32)(ve - vs);

            if (out->tooltype_count >= APP_TOOLTYPE_MAX) {
                return app_reason(reason, "too-many-tooltypes");
            }
            if (slen >= APP_TOOLTYPE_KEY_MAX) {
                return app_reason(reason, "bad-tooltype");
            }
            if (vlen >= APP_TOOLTYPE_VALUE_MAX) {
                return app_reason(reason, "bad-tooltype");
            }
            for (j = vs; j < ve; j++) {
                if (!app_text_char(text[j])) {
                    return app_reason(reason, "bad-tooltype");
                }
            }
            if (app_copy(out->tooltypes[out->tooltype_count].key,
                         APP_TOOLTYPE_KEY_MAX, text + s, ke - s) != 0) {
                return app_reason(reason, "bad-tooltype");
            }
            if (app_copy(out->tooltypes[out->tooltype_count].value,
                         APP_TOOLTYPE_VALUE_MAX, text + vs, ve - vs) != 0) {
                return app_reason(reason, "bad-tooltype");
            }
            out->tooltype_count++;
        }
    }
    if (!seen_id) {
        return app_reason(reason, "no-id");
    }
    if (!seen_name) {
        return app_reason(reason, "no-name");
    }
    if (!seen_version) {
        return app_reason(reason, "no-version");
    }
    if (!seen_prog) {
        return app_reason(reason, "no-prog");
    }
    if (seen_min_os) {
        int ge = app_version_ge(CRUG_VERSION_STR, out->min_os);

        if (ge != 1) {
            return app_reason(reason, "min-os");
        }
    }
    if (reason) {
        *reason = 0;
    }
    return 0;
}

/* ---- the SYS_APP_INFO record --------------------------------------- */

struct app_wbuf {
    char *out;
    usize cap;
    usize n;
    int failed;
};

static void app_put(struct app_wbuf *b, const char *s, usize len)
{
    if (b->failed || b->n + len > b->cap) {
        b->failed = 1;
        return;
    }
    memcpy(b->out + b->n, s, len);
    b->n += len;
}

static void app_put_str(struct app_wbuf *b, const char *s)
{
    app_put(b, s, strlen(s));
}

static void app_put_char(struct app_wbuf *b, char c)
{
    app_put(b, &c, 1u);
}

static void app_put_hex(struct app_wbuf *b, u32 rgb)
{
    static const char digits[] = "0123456789ABCDEF";
    int shift;

    for (shift = 20; shift >= 0; shift -= 4) {
        char c = digits[(rgb >> (u32)shift) & 0xFu];

        app_put_char(b, c);
    }
}

int app_format_info(const struct app_manifest *m, const char *prog,
                    char *out, usize cap)
{
    struct app_wbuf b;
    u32 i;

    if (!m || !prog || !out || cap == 0u) {
        return -1;
    }
    b.out = out;
    b.cap = cap;
    b.n = 0u;
    b.failed = 0;
    app_put_str(&b, m->id);
    app_put_char(&b, '|');
    app_put_str(&b, m->name);
    app_put_char(&b, '|');
    app_put_str(&b, m->version);
    app_put_char(&b, '|');
    app_put_str(&b, prog);
    app_put_char(&b, '|');
    app_put_hex(&b, m->icon_rgb);
    app_put_char(&b, '|');
    for (i = 0u; i < (u32)m->tooltype_count && i < APP_TOOLTYPE_MAX; i++) {
        if (i != 0u) {
            app_put_char(&b, ';');
        }
        app_put_str(&b, m->tooltypes[i].key);
        app_put_char(&b, '=');
        app_put_str(&b, m->tooltypes[i].value);
    }
    if (b.failed) {
        return -1;
    }
    if (b.n < b.cap) {
        b.out[b.n] = 0;
    }
    return (int)b.n;
}