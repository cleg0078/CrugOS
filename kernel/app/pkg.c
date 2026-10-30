/* Phase 7B: signed software packages (.crpkg).

   Two halves that never touch each other's state:

     - the pure helpers (header parse, trust list, signature, payload hash,
       container walker) which are host tested and only ever look at bytes;
     - the installer, which needs the mounted root and therefore runs in the
       kernel: it verifies before it writes anything, stages the bundle under
       /Applications/.stage-<id>, renames it into place and only then records
       /System/pkgdb/<id>.txt.  A package whose signature, payload hash or
       container is bad leaves no trace on the disk.

   Every bound here is a constant from include/crug/pkg.h: 128 KiB of image,
   32 files, four trusted keys.  See docs/packages.md. */
#include <crug/pkg.h>

#include <crug/app.h>
#include <crug/caps.h>
#include <crug/ed25519.h>
#include <crug/ext4.h>
#include <crug/heap.h>
#include <crug/kernel.h>
#include <crug/sha512.h>
#include <crug/storage.h>
#include <crug/string.h>

#define PKG_REG_MAX 8u
#define PKG_RECORD_MAX 192u
#define PKG_PATH_BUF_MAX 160u
#define PKG_TRUST_BUF_MAX 2048u
#define PKG_DEPTH_MAX 3u
#define PKG_RECORD_FIELDS 6u

static struct pkg g_pkgs[PKG_REG_MAX];
static u32 g_count;
static char g_list_buf[PKG_LIST_MAX];
static char g_trust_buf[PKG_TRUST_BUF_MAX];
static char g_record_buf[PKG_RECORD_MAX];
static struct pkg_key g_keys[PKG_MAX_KEYS];
static usize g_key_count;

static u32 pkg_rd32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) |
           ((u32)p[3] << 24);
}

static u64 pkg_rd64(const u8 *p)
{
    return (u64)pkg_rd32(p) | ((u64)pkg_rd32(p + 4) << 32);
}

static int pkg_hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static int pkg_hex_decode(u8 *out, usize n, const char *s, usize len)
{
    usize i;

    if (!out || !s || len != n * 2u) {
        return -1;
    }
    for (i = 0; i < n; i++) {
        int hi = pkg_hex_value(s[2u * i]);
        int lo = pkg_hex_value(s[2u * i + 1u]);

        if (hi < 0 || lo < 0) {
            return -1;
        }
        out[i] = (u8)((hi << 4) | lo);
    }
    return 0;
}

static int pkg_field_char_ok(char c, int allow_space)
{
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9')) {
        return 1;
    }
    if (c == '.' || c == '-' || c == '_') {
        return 1;
    }
    if (allow_space && c == ' ') {
        return 1;
    }
    return 0;
}

/* Copy a NUL terminated bounded field into dst.  The source window must hold
   a terminator, at least one character, and only legal characters. */
static int pkg_copy_field(char *dst, usize cap, const u8 *src, usize srclen,
                          int allow_space, const char **reason)
{
    usize len = 0u;
    usize i;

    if (!dst || cap == 0u) {
        if (reason) {
            *reason = "id";
        }
        return -1;
    }
    while (len < srclen && src[len] != 0) {
        len++;
    }
    if (len == 0u || len >= cap || len >= srclen) {
        if (reason) {
            *reason = "id";
        }
        return -1;
    }
    for (i = 0; i < len; i++) {
        if (!pkg_field_char_ok((char)src[i], allow_space)) {
            if (reason) {
                *reason = "id";
            }
            return -1;
        }
    }
    memcpy(dst, src, len);
    dst[len] = 0;
    return 0;
}

int pkg_header_parse(const u8 *raw, usize len, struct pkg_header *out,
                     const char **reason)
{
    u32 payload_size;
    u32 flags;
    u32 header_size;

    if (!raw || !out) {
        if (reason) {
            *reason = "short";
        }
        return -1;
    }
    if (len < PKG_HEADER_SIZE) {
        if (reason) {
            *reason = "short";
        }
        return -1;
    }
    if (memcmp(raw, PKG_MAGIC, PKG_MAGIC_LEN) != 0) {
        if (reason) {
            *reason = "magic";
        }
        return -1;
    }
    header_size = pkg_rd32(raw + 0x08u);
    if (header_size != PKG_HEADER_SIZE) {
        if (reason) {
            *reason = "header-size";
        }
        return -1;
    }
    flags = pkg_rd32(raw + 0x0Cu);
    if (flags != 0u) {
        if (reason) {
            *reason = "flags";
        }
        return -1;
    }
    memset(out, 0, sizeof(*out));
    if (pkg_copy_field(out->id, sizeof(out->id), raw + 0x10u, PKG_ID_MAX, 0,
                       reason) != 0) {
        if (reason) {
            *reason = "id";
        }
        return -1;
    }
    if (pkg_copy_field(out->name, sizeof(out->name), raw + 0x30u,
                       PKG_NAME_MAX, 1, reason) != 0) {
        if (reason) {
            *reason = "name";
        }
        return -1;
    }
    if (pkg_copy_field(out->version, sizeof(out->version), raw + 0x50u,
                       PKG_VERSION_MAX, 0, reason) != 0 ||
        app_version_compare(out->version, "0.0.0") == -2) {
        if (reason) {
            *reason = "version";
        }
        return -1;
    }
    if (pkg_copy_field(out->publisher, sizeof(out->publisher), raw + 0x60u,
                       PKG_PUBLISHER_MAX, 1, reason) != 0) {
        if (reason) {
            *reason = "publisher";
        }
        return -1;
    }
    out->cap_mask = pkg_rd32(raw + 0x80u);
    if ((out->cap_mask & ~CAP_ALL) != 0u) {
        if (reason) {
            *reason = "cap";
        }
        return -1;
    }
    payload_size = pkg_rd32(raw + 0x84u);
    if (payload_size < 4u || payload_size > PKG_MAX_BYTES) {
        if (reason) {
            *reason = "payload-size";
        }
        return -1;
    }
    out->payload_size = payload_size;
    memcpy(out->payload_sha512, raw + 0x88u, 64u);
    if (pkg_copy_field(out->key_id, sizeof(out->key_id), raw + 0xC8u,
                       PKG_KEY_ID_MAX, 0, reason) != 0) {
        if (reason) {
            *reason = "key";
        }
        return -1;
    }
    memcpy(out->pubkey, raw + PKG_PUBKEY_OFFSET, PKG_PUBKEY_SIZE);
    out->timestamp = pkg_rd64(raw + 0x108u);
    return 0;
}

int pkg_trust_parse(const char *text, usize len, struct pkg_key *keys,
                    usize cap, usize *count, const char **reason)
{
    usize i = 0u;

    if (!text || !keys || !count) {
        if (reason) {
            *reason = "trust-line";
        }
        return -1;
    }
    *count = 0u;
    while (i < len) {
        usize start = i;
        usize end;
        usize j;

        while (i < len && text[i] != 10) {
            i++;
        }
        end = i;
        if (i < len) {
            i++;
        }
        while (start < end && (text[start] == ' ' || text[start] == 9 ||
                               text[start] == 13)) {
            start++;
        }
        while (end > start && (text[end - 1u] == ' ' || text[end - 1u] == 9 ||
                               text[end - 1u] == 13)) {
            end--;
        }
        if (start == end || text[start] == '#') {
            continue;
        }
        if (end - start < 5u || text[start] != 'p' || text[start + 1u] != 'u' ||
            text[start + 2u] != 'b' ||
            (text[start + 3u] != ' ' && text[start + 3u] != 9)) {
            if (reason) {
                *reason = "trust-line";
            }
            return -1;
        }
        {
            const char *line = text + start;
            usize n = end - start;
            usize hstart;
            usize hend;
            usize pstart;
            struct pkg_key key;

            j = 3u;
            while (j < n && (line[j] == ' ' || line[j] == 9)) {
                j++;
            }
            hstart = j;
            while (j < n && line[j] != ' ' && line[j] != 9) {
                j++;
            }
            hend = j;
            memset(&key, 0, sizeof(key));
            if (hend - hstart != 64u ||
                pkg_hex_decode(key.pubkey, 32u, line + hstart, 64u) != 0) {
                if (reason) {
                    *reason = "trust-hex";
                }
                return -1;
            }
            while (j < n && (line[j] == ' ' || line[j] == 9)) {
                j++;
            }
            pstart = j;
            if (n - pstart >= PKG_PUBLISHER_MAX) {
                if (reason) {
                    *reason = "trust-line";
                }
                return -1;
            }
            if (n > pstart) {
                usize k;

                for (k = pstart; k < n; k++) {
                    if (!pkg_field_char_ok(line[k], 1)) {
                        if (reason) {
                            *reason = "trust-line";
                        }
                        return -1;
                    }
                }
                memcpy(key.publisher, line + pstart, n - pstart);
            }
            if (*count >= cap) {
                if (reason) {
                    *reason = "trust-full";
                }
                return -1;
            }
            keys[*count] = key;
            (*count)++;
        }
    }
    return 0;
}

int pkg_find_key(const struct pkg_key *keys, usize count, const u8 *pk)
{
    usize i;

    if (!keys || !pk) {
        return -1;
    }
    for (i = 0; i < count; i++) {
        if (memcmp(keys[i].pubkey, pk, PKG_PUBKEY_SIZE) == 0) {
            return (int)i;
        }
    }
    return -1;
}

int pkg_signature_ok(const u8 *raw, usize len, const u8 *pubkey,
                     const u8 *sig)
{
    u8 buf[PKG_HEADER_SIZE];

    if (!raw || len < PKG_HEADER_SIZE || !pubkey || !sig) {
        return -1;
    }
    memcpy(buf, raw, PKG_HEADER_SIZE);
    memset(buf + PKG_SIG_OFFSET, 0, PKG_SIG_SIZE);
    return ed25519_verify(pubkey, buf, PKG_HEADER_SIZE, sig) == 0 ? 0 : -1;
}

int pkg_payload_ok(const struct pkg_header *h, const u8 *payload, usize len)
{
    u8 digest[SHA512_DIGEST_SIZE];

    if (!h || (!payload && len)) {
        return -1;
    }
    sha512_hash(digest, payload, len);
    return memcmp(digest, h->payload_sha512, SHA512_DIGEST_SIZE) == 0 ? 0 : -1;
}

int pkg_payload_next(const u8 *payload, usize len, usize *off, char *path,
                     usize path_cap, const u8 **data, u32 *data_len,
                     const char **reason)
{
    u32 plen;
    u32 dlen;
    usize i;

    if (!payload || !off || !path || !data || !data_len) {
        if (reason) {
            *reason = "container";
        }
        return -1;
    }
    if (*off < 8u) {
        *off = 8u;
    }
    if (*off == len) {
        return 0;
    }
    if (*off + 8u > len) {
        if (reason) {
            *reason = "container";
        }
        return -1;
    }
    plen = pkg_rd32(payload + *off);
    dlen = pkg_rd32(payload + *off + 4u);
    *off += 8u;
    if (plen == 0u || plen >= path_cap || plen > PKG_PATH_MAX ||
        dlen > PKG_MAX_FILE_BYTES || *off + plen + dlen > len ||
        *off + plen > len) {
        if (reason) {
            *reason = dlen > PKG_MAX_FILE_BYTES ? "file-size" : "path";
        }
        return -1;
    }
    if (path_cap == 0u) {
        if (reason) {
            *reason = "path";
        }
        return -1;
    }
    for (i = 0; i < plen; i++) {
        char ch = (char)payload[*off + i];

        if ((unsigned char)ch < 0x20u || ch == 127 ||
            ch == '\\' || ch == ':' || ch == '*' || ch == '?') {
            if (reason) {
                *reason = "path";
            }
            return -1;
        }
    }
    if (payload[*off] == '/' || payload[*off + plen - 1u] == '/') {
        if (reason) {
            *reason = "path";
        }
        return -1;
    }
    for (i = 0; i + 1u < plen; i++) {
        if (payload[*off + i] == '/' && payload[*off + i + 1u] == '/') {
            if (reason) {
                *reason = "path";
            }
            return -1;
        }
    }
    for (i = 0; i + 2u < plen; i++) {
        if (payload[*off + i] == '.' && payload[*off + i + 1u] == '.' &&
            (payload[*off + i + 2u] == '/' ||
             (i + 3u == plen && i > 0u &&
              payload[*off + i - 1u] == '/'))) {
            if (reason) {
                *reason = "path";
            }
            return -1;
        }
    }
    memcpy(path, payload + *off, plen);
    path[plen] = 0;
    *off += plen;
    *data = payload + *off;
    *data_len = dlen;
    *off += dlen;
    return 1;
}

const char *pkg_reason_text(const char *reason)
{
    if (!reason) {
        return "unknown";
    }
    if (strcmp(reason, "short") == 0) {
        return "truncated header";
    }
    if (strcmp(reason, "magic") == 0) {
        return "not a CrugOS package";
    }
    if (strcmp(reason, "header-size") == 0) {
        return "bad header size";
    }
    if (strcmp(reason, "flags") == 0) {
        return "unsupported flags";
    }
    if (strcmp(reason, "id") == 0) {
        return "bad package id";
    }
    if (strcmp(reason, "name") == 0) {
        return "bad package name";
    }
    if (strcmp(reason, "version") == 0) {
        return "bad version";
    }
    if (strcmp(reason, "publisher") == 0) {
        return "bad publisher";
    }
    if (strcmp(reason, "cap") == 0) {
        return "bad capability set";
    }
    if (strcmp(reason, "payload-size") == 0) {
        return "bad payload size";
    }
    if (strcmp(reason, "key") == 0) {
        return "bad key id";
    }
    if (strcmp(reason, "trust-line") == 0 ||
        strcmp(reason, "trust-hex") == 0 ||
        strcmp(reason, "trust-full") == 0) {
        return "bad trust list";
    }
    if (strcmp(reason, "untrusted-key") == 0) {
        return "untrusted signing key";
    }
    if (strcmp(reason, "signature") == 0) {
        return "bad signature";
    }
    if (strcmp(reason, "payload-hash") == 0) {
        return "payload does not match its hash";
    }
    if (strcmp(reason, "container") == 0) {
        return "damaged payload";
    }
    if (strcmp(reason, "path") == 0) {
        return "illegal file path";
    }
    if (strcmp(reason, "file-size") == 0) {
        return "file too large";
    }
    if (strcmp(reason, "too-many-files") == 0) {
        return "too many files";
    }
    if (strcmp(reason, "already-installed") == 0) {
        return "already installed";
    }
    if (strcmp(reason, "downgrade") == 0) {
        return "newer version installed";
    }
    if (strcmp(reason, "not-installed") == 0) {
        return "package not installed";
    }
    if (strcmp(reason, "io") == 0) {
        return "filesystem error";
    }
    if (strcmp(reason, "no-package") == 0) {
        return "no such package";
    }
    if (strcmp(reason, "too-large") == 0) {
        return "package too large";
    }
    if (strcmp(reason, "record") == 0) {
        return "cannot record install";
    }
    if (strcmp(reason, "read-only") == 0) {
        return "the root is read-only";
    }
    return reason;
}

/* ---- the kernel side: trust list, registry and installer ---- */

static struct ext4_fs *pkg_fs(void)
{
    if (!storage_present()) {
        return 0;
    }
    return storage_root();
}

static int pkg_load_trust(void)
{
    struct ext4_fs *fs = pkg_fs();
    const char *reason = 0;
    u64 size = 0;

    g_key_count = 0u;
    if (!fs) {
        return 0;
    }
    if (ext4_read_file(fs, PKG_TRUST_PATH, g_trust_buf,
                       PKG_TRUST_BUF_MAX - 1u, &size) != 0) {
        return 0;
    }
    if (size >= PKG_TRUST_BUF_MAX) {
        return -1;
    }
    g_trust_buf[size] = 0;
    if (pkg_trust_parse(g_trust_buf, (usize)size, g_keys, PKG_MAX_KEYS,
                        &g_key_count, &reason) != 0) {
        g_key_count = 0u;
        return -1;
    }
    return 0;
}

static int pkg_join(char *out, usize cap, const char *a, const char *b)
{
    usize alen;
    usize blen;

    if (!out || !a || !b) {
        return -1;
    }
    alen = strlen(a);
    blen = strlen(b);
    if (alen + 1u + blen + 1u > cap) {
        return -1;
    }
    memcpy(out, a, alen);
    out[alen] = '/';
    memcpy(out + alen + 1u, b, blen + 1u);
    return 0;
}

static int pkg_read_record(const char *path, struct pkg *out)
{
    struct ext4_fs *fs = pkg_fs();
    const char *p;
    usize used = 0u;
    u64 size = 0u;
    u32 field = 0u;

    if (!fs || ext4_read_file(fs, path, g_record_buf, PKG_RECORD_MAX,
                              &size) != 0) {
        return -1;
    }
    if (size == 0u || size >= PKG_RECORD_MAX) {
        return -1;
    }
    g_record_buf[size] = 0;
    memset(out, 0, sizeof(*out));
    p = g_record_buf;
    while (*p) {
        usize len = 0u;
        char *dst = 0;
        usize cap = 0u;

        while (p[len] && p[len] != '|') {
            len++;
        }
        switch (field) {
        case 0u:
            dst = out->id;
            cap = sizeof(out->id);
            break;
        case 1u:
            dst = out->name;
            cap = sizeof(out->name);
            break;
        case 2u:
            dst = out->version;
            cap = sizeof(out->version);
            break;
        case 3u:
            dst = out->publisher;
            cap = sizeof(out->publisher);
            break;
        case 4u:
            if (len) {
                u32 mask = 0u;
                char bad[CAP_NAME_MAX];

                if (cap_parse_list(p, len, &mask, bad, sizeof(bad)) == 0) {
                    out->cap_mask = mask;
                }
            }
            dst = 0;
            break;
        default:
            dst = out->prog;
            cap = sizeof(out->prog);
            break;
        }
        if (dst) {
            if (len == 0u || len >= cap) {
                return -1;
            }
            memcpy(dst, p, len);
            dst[len] = 0;
        }
        used = len;
        p += used;
        if (*p == '|') {
            p++;
        }
        field++;
        if (field > PKG_RECORD_FIELDS) {
            break;
        }
    }
    if (field < 2u || out->id[0] == 0) {
        return -1;
    }
    return 0;
}

static int pkg_record_format(const struct pkg_header *h, const char *prog,
                             char *out, usize cap)
{
    char caps[64];
    int n;

    if (cap_format(h->cap_mask, caps, sizeof(caps)) < 0) {
        return -1;
    }
    n = ksnprintf(out, cap, "%s|%s|%s|%s|%s|%s", h->id, h->name, h->version,
                  h->publisher, caps, prog);
    return n > 0 && (usize)n < cap ? n : -1;
}

static int pkg_registry_add(const struct pkg *candidate)
{
    u32 pos = 0u;
    u32 i;

    if (!candidate || g_count >= PKG_REG_MAX) {
        return -1;
    }
    while (pos < g_count && strcmp(g_pkgs[pos].id, candidate->id) < 0) {
        pos++;
    }
    if (pos < g_count && strcmp(g_pkgs[pos].id, candidate->id) == 0) {
        memcpy(&g_pkgs[pos], candidate, sizeof(g_pkgs[pos]));
        return 0;
    }
    for (i = g_count; i > pos; i--) {
        memcpy(&g_pkgs[i], &g_pkgs[i - 1u], sizeof(g_pkgs[i]));
    }
    memcpy(&g_pkgs[pos], candidate, sizeof(g_pkgs[pos]));
    g_count++;
    return 0;
}

static int pkg_registry_remove(const char *id)
{
    u32 i;

    if (!id) {
        return -1;
    }
    for (i = 0u; i < g_count; i++) {
        if (strcmp(g_pkgs[i].id, id) == 0) {
            u32 j;

            for (j = i + 1u; j < g_count; j++) {
                memcpy(&g_pkgs[j - 1u], &g_pkgs[j], sizeof(g_pkgs[j]));
            }
            g_count--;
            memset(&g_pkgs[g_count], 0, sizeof(g_pkgs[g_count]));
            return 0;
        }
    }
    return -1;
}

void pkg_reset(void)
{
    memset(g_pkgs, 0, sizeof(g_pkgs));
    g_count = 0u;
    g_key_count = 0u;
}

int pkg_init(void)
{
    struct ext4_fs *fs = pkg_fs();
    int n;
    usize i = 0u;

    pkg_reset();
    if (!fs) {
        kprintf("CRUGOS: package registry ready (0 package(s))\n");
        return 0;
    }
    (void)pkg_load_trust();
    n = ext4_list(fs, PKG_DB_DIR, g_list_buf, PKG_LIST_MAX);
    if (n > 0 && (usize)n < PKG_LIST_MAX) {
        while (i < (usize)n) {
            usize start = i;
            usize len;
            char name[64];
            char path[PKG_PATH_BUF_MAX];
            struct pkg record;

            while (i < (usize)n && g_list_buf[i] != 10) {
                i++;
            }
            len = i - start;
            if (i < (usize)n) {
                i++;
            }
            if (len == 0u || len >= sizeof(name)) {
                continue;
            }
            memcpy(name, g_list_buf + start, len);
            name[len] = 0;
            if (len < 4u || strcmp(name + len - 4u, ".txt") != 0) {
                continue;
            }
            if (pkg_join(path, sizeof(path), PKG_DB_DIR, name) != 0) {
                continue;
            }
            if (pkg_read_record(path, &record) == 0) {
                (void)pkg_registry_add(&record);
            }
        }
    }
    kprintf("CRUGOS: package registry ready (%u package(s))\n",
            (unsigned)g_count);
    return 0;
}

u32 pkg_count(void)
{
    return g_count;
}

const struct pkg *pkg_at(u32 index)
{
    if (index >= g_count) {
        return 0;
    }
    return &g_pkgs[index];
}

const struct pkg *pkg_find_id(const char *id)
{
    u32 i;

    if (!id || !id[0]) {
        return 0;
    }
    for (i = 0u; i < g_count; i++) {
        if (strcmp(g_pkgs[i].id, id) == 0) {
            return &g_pkgs[i];
        }
    }
    return 0;
}

int pkg_format_info(const struct pkg *p, char *out, usize cap)
{
    char caps[64];
    int n;

    if (!p || !out || cap == 0u) {
        return -1;
    }
    if (cap_format(p->cap_mask, caps, sizeof(caps)) < 0) {
        return -1;
    }
    n = ksnprintf(out, cap, "%s|%s|%s|%s|%s|%s", p->id, p->name, p->version,
                  p->publisher, caps, p->prog);
    return n > 0 && (usize)n < cap ? n : -1;
}

int pkg_probe(const char *path, struct pkg_header *out, int *out_trusted,
              const char **reason)
{
    struct ext4_fs *fs = pkg_fs();
    const char *r = 0;
    u8 raw[PKG_HEADER_SIZE];
    u64 size = 0u;
    int idx;

    if (out_trusted) {
        *out_trusted = 0;
    }
    if (!fs || !path) {
        if (reason) {
            *reason = "no-package";
        }
        return -1;
    }
    if (ext4_read_file(fs, path, raw, sizeof(raw), &size) != 0 ||
        size < PKG_HEADER_SIZE) {
        if (reason) {
            *reason = "short";
        }
        return -1;
    }
    if (pkg_header_parse(raw, sizeof(raw), out, &r) != 0) {
        if (reason) {
            *reason = r;
        }
        return -1;
    }
    (void)pkg_load_trust();
    idx = pkg_find_key(g_keys, g_key_count, out->pubkey);
    if (idx < 0) {
        if (reason) {
            *reason = "untrusted-key";
        }
        return -1;
    }
    if (out_trusted) {
        *out_trusted = 1;
    }
    if (pkg_signature_ok(raw, sizeof(raw), out->pubkey,
                         raw + PKG_SIG_OFFSET) != 0) {
        if (reason) {
            *reason = "signature";
        }
        if (out_trusted) {
            *out_trusted = 0;
        }
        return -1;
    }
    return 0;
}

static int pkg_rm_tree(const char *path, u32 depth)
{
    struct ext4_fs *fs = pkg_fs();
    int n;
    usize i = 0u;
    char buf[PKG_LIST_MAX];
    char child[PKG_PATH_BUF_MAX];

    if (!fs || !path || depth > PKG_DEPTH_MAX) {
        return -1;
    }
    n = ext4_list(fs, path, buf, sizeof(buf));
    if (n < 0) {
        return -1;
    }
    if ((usize)n >= sizeof(buf)) {
        return -1;
    }
    while (i < (usize)n) {
        usize start = i;
        usize len;

        while (i < (usize)n && buf[i] != 10) {
            i++;
        }
        len = i - start;
        if (i < (usize)n) {
            i++;
        }
        if (len == 0u || len >= 64u) {
            continue;
        }
        {
            char name[64];

            memcpy(name, buf + start, len);
            name[len] = 0;
            if (name[0] == '.' &&
                (len == 1u || (len == 2u && name[1] == '.'))) {
                continue;
            }
            if (pkg_join(child, sizeof(child), path, name) != 0) {
                continue;
            }
        }
        if (ext4_list(fs, child, buf, 1u) >= 0) {
            (void)pkg_rm_tree(child, depth + 1u);
            (void)ext4_rmdir(fs, child);
        } else {
            (void)ext4_unlink(fs, child);
        }
    }
    return 0;
}

static int pkg_mkdir_p(struct ext4_fs *fs, const char *path, u16 mode)
{
    char tmp[PKG_PATH_BUF_MAX];
    usize i;
    usize len;
    u32 ino;

    if (ext4_lookup(fs, path, &ino) == 0) {
        return 0;
    }
    len = strlen(path);
    if (len == 0u || len >= sizeof(tmp)) {
        return -1;
    }
    memcpy(tmp, path, len + 1u);
    for (i = 1u; i < len; i++) {
        if (tmp[i] != '/') {
            continue;
        }
        tmp[i] = 0;
        if (tmp[0] && ext4_lookup(fs, tmp, &ino) != 0) {
            (void)ext4_mkdir(fs, tmp, mode);
        }
        tmp[i] = '/';
    }
    return ext4_mkdir(fs, path, mode);
}

static int pkg_create_file(struct ext4_fs *fs, const char *path, u16 mode)
{
    u32 ino;

    if (ext4_lookup(fs, path, &ino) == 0) {
        return 0;
    }
    return ext4_create(fs, path, mode);
}

int pkg_install(const char *path, const char **reason)
{
    struct ext4_fs *fs = pkg_fs();
    struct pkg_header h;
    struct pkg entry;
    const char *r = 0;
    const char *basename = path;
    u8 *image = 0;
    u64 size = 0u;
    usize off = 8u;
    const u8 *data = 0;
    u32 dlen = 0u;
    u32 files = 0u;
    u32 ino = 0u;
    int idx;
    int has_manifest = 0;
    char stage[PKG_PATH_BUF_MAX];
    char target[PKG_PATH_BUF_MAX];
    char fpath[PKG_PATH_BUF_MAX];
    char rel[PKG_PATH_MAX];
    char record[PKG_RECORD_MAX];
    char recpath[PKG_PATH_BUF_MAX];
    char recname[PKG_ID_MAX + 8u];
    char backup[PKG_PATH_BUF_MAX];
    char oldver[PKG_VERSION_MAX];
    int remounted = 0;
    int is_update = 0;

    if (!reason) {
        return -1;
    }
    *reason = 0;
    if (!fs || !path) {
        *reason = "no-package";
        kprintf("CRUGOS: pkg rejected %s (no-package)\n", path ? path : "-");
        return -1;
    }
    if (basename) {
        const char *p = basename;

        while (*p) {
            if (*p == '/') {
                basename = p + 1;
            }
            p++;
        }
    }
    image = (u8 *)kmalloc(PKG_MAX_BYTES);
    if (!image) {
        *reason = "io";
        kprintf("CRUGOS: pkg rejected %s (io)\n", basename);
        return -1;
    }
    if (ext4_read_file(fs, path, image, PKG_MAX_BYTES, &size) != 0) {
        *reason = "too-large";
        goto reject;
    }
    if (size <= PKG_HEADER_SIZE) {
        *reason = "short";
        goto reject;
    }
    if (pkg_header_parse(image, (usize)size, &h, &r) != 0) {
        *reason = r;
        goto reject;
    }
    if (size - PKG_HEADER_SIZE != (u64)h.payload_size) {
        *reason = "payload-size";
        goto reject;
    }
    (void)pkg_load_trust();
    idx = pkg_find_key(g_keys, g_key_count, h.pubkey);
    if (idx < 0) {
        *reason = "untrusted-key";
        goto reject;
    }
    if (pkg_signature_ok(image, PKG_HEADER_SIZE, h.pubkey,
                         image + PKG_SIG_OFFSET) != 0) {
        *reason = "signature";
        goto reject;
    }
    if (pkg_payload_ok(&h, image + PKG_HEADER_SIZE, h.payload_size) != 0) {
        *reason = "payload-hash";
        goto reject;
    }
    {
        u32 count = pkg_rd32(image + PKG_HEADER_SIZE);

        if (count == 0u || count > PKG_MAX_FILES) {
            *reason = count > PKG_MAX_FILES ? "too-many-files" : "container";
            goto reject;
        }
    }
    if (!pkg_id_valid(h.id)) {
        *reason = "id";
        goto reject;
    }
    if (pkg_join(target, sizeof(target), APP_DIR, h.id) != 0) {
        *reason = "id";
        goto reject;
    }
    {
        usize tlen = strlen(target);

        if (tlen + 5u >= sizeof(target)) {
            *reason = "id";
            goto reject;
        }
        memcpy(target + tlen, ".app", 5u);
    }
    {
        usize idlen = strlen(h.id);

        /* pkg_join inserts a separator, so the suffix is appended by hand. */
        if (idlen + 5u > sizeof(recname)) {
            *reason = "id";
            goto reject;
        }
        memcpy(recname, h.id, idlen);
        memcpy(recname + idlen, ".txt", 5u);
    }
    if (pkg_join(recpath, sizeof(recpath), PKG_DB_DIR, recname) != 0) {
        *reason = "id";
        goto reject;
    }
    if (ext4_lookup(fs, target, &ino) == 0) {
        struct pkg existing;
        int cmp;

        /* Phase 7C: a bundle already at this path is an update when the
           incoming version is newer and a refusal otherwise. */
        if (pkg_read_record(recpath, &existing) != 0) {
            *reason = "record";
            goto reject;
        }
        cmp = app_version_compare(h.version, existing.version);
        if (cmp == 0) {
            *reason = "already-installed";
            goto reject;
        }
        if (cmp < 0) {
            *reason = "downgrade";
            goto reject;
        }
        memcpy(oldver, existing.version, sizeof(oldver));
        is_update = 1;
    }
    /* The boot mounts the root read-only; an install needs a short
       read-write window, exactly like the shell's write command. */
    if (!(fs->flags & EXT4_MOUNT_RW)) {
        if (storage_remount_rw() != 0) {
            *reason = "read-only";
            goto reject;
        }
        remounted = 1;
    }
    if (pkg_join(stage, sizeof(stage), PKG_STAGE_DIR, h.id) != 0) {
        *reason = "id";
        goto reject;
    }
    (void)pkg_mkdir_p(fs, PKG_STAGE_DIR, 0x41EDu);
    (void)pkg_rm_tree(stage, 0u);
    if (pkg_mkdir_p(fs, stage, 0x41EDu) != 0) {
        *reason = "io";
        goto reject;
    }
    {
        usize slen = strlen(stage);

        if (slen + 5u > sizeof(backup)) {
            *reason = "id";
            goto rollback;
        }
        memcpy(backup, stage, slen);
        memcpy(backup + slen, ".old", 5u);
    }
    for (;;) {
        int n = pkg_payload_next(image + PKG_HEADER_SIZE, h.payload_size, &off,
                                 rel, sizeof(rel), &data, &dlen, &r);

        if (n == 0) {
            break;
        }
        if (n < 0) {
            *reason = r;
            goto rollback;
        }
        files++;
        if (pkg_join(fpath, sizeof(fpath), stage, rel) != 0) {
            *reason = "path";
            goto rollback;
        }
        {
            char dir[PKG_PATH_BUF_MAX];
            usize k;
            usize last = 0u;
            usize flen = strlen(fpath);

            memcpy(dir, fpath, flen + 1u);
            for (k = 0u; k < flen; k++) {
                if (dir[k] == '/') {
                    last = k;
                }
            }
            if (last > 0u) {
                dir[last] = 0;
                if (pkg_mkdir_p(fs, dir, 0x41EDu) != 0) {
                    *reason = "io";
                    goto rollback;
                }
            }
        }
        if (pkg_create_file(fs, fpath, 0x81A4u) != 0 ||
            ext4_write_file(fs, fpath, data, (usize)dlen) != 0) {
            *reason = "io";
            goto rollback;
        }
        if (strcmp(rel, "manifest.toml") == 0) {
            has_manifest = 1;
        }
    }
    if (!has_manifest) {
        *reason = "container";
        goto rollback;
    }
    (void)ext4_fsync(fs);
    if (is_update) {
        /* Park the old bundle next to the stage directory: the rename below
           still fails if the record cannot be written, and rollback puts the
           old version back. */
        (void)pkg_rm_tree(backup, 0u);
        (void)ext4_rmdir(fs, backup);
        if (ext4_rename(fs, target, backup) != 0) {
            *reason = "io";
            goto rollback;
        }
    }
    if (ext4_rename(fs, stage, target) != 0) {
        *reason = "io";
        if (is_update) {
            (void)ext4_rename(fs, backup, target);
        }
        goto rollback;
    }
    if (pkg_record_format(&h, target, record, sizeof(record)) < 0) {
        *reason = "record";
        goto undo;
    }
    (void)pkg_mkdir_p(fs, PKG_DB_DIR, 0x41EDu);
    if (pkg_create_file(fs, recpath, 0x81A4u) != 0 ||
        ext4_write_file(fs, recpath, record, strlen(record)) != 0) {
        (void)ext4_unlink(fs, recpath);
        *reason = "record";
        goto undo;
    }
    (void)ext4_fsync(fs);
    memset(&entry, 0, sizeof(entry));
    memcpy(entry.id, h.id, sizeof(entry.id));
    memcpy(entry.name, h.name, sizeof(entry.name));
    memcpy(entry.version, h.version, sizeof(entry.version));
    memcpy(entry.publisher, h.publisher, sizeof(entry.publisher));
    memcpy(entry.prog, target, sizeof(entry.prog));
    entry.cap_mask = h.cap_mask;
    (void)pkg_registry_add(&entry);
    if (is_update) {
        (void)pkg_rm_tree(backup, 0u);
        (void)ext4_rmdir(fs, backup);
        kprintf("CRUGOS: pkg updated %s %s -> %s (%u file(s), %u cap(s))\n",
                h.id, oldver, h.version, (unsigned)files,
                (unsigned)h.cap_mask);
    } else {
        kprintf("CRUGOS: pkg installed %s %s (%u file(s), %u cap(s))\n", h.id,
                h.version, (unsigned)files, (unsigned)h.cap_mask);
    }
    goto done;

undo:
    (void)ext4_rename(fs, target, stage);
    if (is_update) {
        (void)ext4_rename(fs, backup, target);
    }
rollback:
    (void)pkg_rm_tree(stage, 0u);
    (void)ext4_rmdir(fs, stage);
reject:
    kprintf("CRUGOS: pkg rejected %s (%s)\n", basename ? basename : "-",
            *reason ? *reason : "io");
done:
    if (remounted) {
        (void)storage_remount_ro();
    }
    kfree(image);
    return *reason ? -1 : 0;
}

/* Validate a package id before it becomes a path component.  The id is
   signed, so a hostile package can choose any value the signer allowed;
   '.' or '..' would make /Applications/<id>.app address an existing
   directory, so a legal id is exactly one ordinary name: letters, digits,
   '.', '_' and '-', not starting or ending with a dot or a dash, and never
   containing an empty component. */
int pkg_id_valid(const char *id)
{
    usize i;
    usize len;

    if (!id || !id[0]) {
        return 0;
    }
    len = strlen(id);
    if (len >= PKG_ID_MAX) {
        return 0;
    }
    if (id[0] == '.' || id[0] == '-' || id[len - 1u] == '.' ||
        id[len - 1u] == '-') {
        return 0;
    }
    for (i = 0u; i < len; i++) {
        char c = id[i];

        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-') {
            continue;
        }
        return 0;
    }
    for (i = 0u; i + 1u < len; i++) {
        if (id[i] == '.' && id[i + 1u] == '.') {
            return 0;
        }
    }
    return 1;
}

int pkg_remove(const char *id, const char **reason)
{
    struct ext4_fs *fs = pkg_fs();
    const struct pkg *installed;
    char target[PKG_PATH_BUF_MAX];
    char recname[PKG_ID_MAX + 8u];
    char recpath[PKG_PATH_BUF_MAX];
    u32 ino = 0u;
    usize idlen;
    int present;
    int remounted = 0;

    if (!reason) {
        return -1;
    }
    *reason = 0;
    if (!fs || !id || !pkg_id_valid(id)) {
        *reason = "id";
        goto reject;
    }
    if (pkg_join(target, sizeof(target), APP_DIR, id) != 0) {
        *reason = "id";
        goto reject;
    }
    {
        usize tlen = strlen(target);

        if (tlen + 5u >= sizeof(target)) {
            *reason = "id";
            goto reject;
        }
        memcpy(target + tlen, ".app", 5u);
    }
    installed = pkg_find_id(id);
    present = ext4_lookup(fs, target, &ino) == 0;
    if (!installed && !present) {
        *reason = "not-installed";
        goto reject;
    }
    if (!(fs->flags & EXT4_MOUNT_RW)) {
        if (storage_remount_rw() != 0) {
            *reason = "read-only";
            goto reject;
        }
        remounted = 1;
    }
    if (present) {
        (void)pkg_rm_tree(target, 0u);
        (void)ext4_rmdir(fs, target);
    }
    idlen = strlen(id);
    if (idlen + 5u <= sizeof(recname)) {
        memcpy(recname, id, idlen);
        memcpy(recname + idlen, ".txt", 5u);
        if (pkg_join(recpath, sizeof(recpath), PKG_DB_DIR, recname) == 0) {
            (void)ext4_unlink(fs, recpath);
        }
    }
    (void)ext4_fsync(fs);
    (void)pkg_registry_remove(id);
    (void)app_init();
    kprintf("CRUGOS: pkg removed %s (%u package(s))\n", id,
            (unsigned)pkg_count());
    goto done;

reject:
    kprintf("CRUGOS: pkg remove rejected %s (%s)\n", id ? id : "-",
            *reason ? *reason : "io");
done:
    if (remounted) {
        (void)storage_remount_ro();
    }
    return *reason ? -1 : 0;
}
