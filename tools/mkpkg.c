/* Phase 7B: host-side signer for .crpkg application bundles.

   A package is a 512-byte header followed by a container payload:

     header   magic 'CRUGPKG1', header_size 512, flags 0, then fixed fields
              (id, name, version, publisher, capabilities, payload size and
              SHA-512), the signing key, a timestamp and a 64-byte Ed25519
              signature over the 512 header bytes with the signature field
              taken as zero.  See include/crug/pkg.h for every offset.
     payload  u32 file count, u32 reserved, then per file a u32 path length,
              a u32 data length, the relative path and the data.

   The tool also emits the trust line the kernel reads from
   /System/trusted/keys.conf, so one invocation can seed both the package and
   the key list of a test disk.  The signing seed is fixed on the command
   line so a disk image is reproducible. */
#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

/* The kernel crypto is freestanding C with its own typedefs, so declare the
   two entry points with plain C types instead of including <crug/types.h>.
   usize is unsigned long on x86_64. */
extern void sha512_hash(unsigned char out[64], const void *data,
                        unsigned long len);
extern int ed25519_keypair(unsigned char pk[32], unsigned char sk[64],
                           const unsigned char seed[32]);
extern int ed25519_sign(unsigned char sig[64], const unsigned char sk[64],
                        const void *msg, unsigned long len);

#define PKG_HEADER_SIZE 512u
#define PKG_MAX_BYTES (128u * 1024u)
#define PKG_MAX_FILES 32u
#define PKG_MAX_FILE_BYTES (96u * 1024u)
#define PKG_PATH_MAX 96u
#define PKG_ID_MAX 32u
#define PKG_NAME_MAX 32u
#define PKG_VERSION_MAX 16u
#define PKG_PUBLISHER_MAX 32u
#define PKG_KEY_ID_MAX 32u
#define PKG_MAX_KEYS 4u

#define HDR_MAGIC 0x00u
#define HDR_SIZE 0x08u
#define HDR_FLAGS 0x0Cu
#define HDR_ID 0x10u
#define HDR_NAME 0x30u
#define HDR_VERSION 0x50u
#define HDR_PUBLISHER 0x60u
#define HDR_CAP_MASK 0x80u
#define HDR_PAYLOAD_SIZE 0x84u
#define HDR_PAYLOAD_SHA 0x88u
#define HDR_KEY_ID 0xC8u
#define HDR_PUBKEY 0xE8u
#define HDR_TIMESTAMP 0x108u
#define HDR_SIGNATURE 0x110u

#define CAP_FS_READ (1u << 0)
#define CAP_FS_WRITE (1u << 1)
#define CAP_APP_LAUNCH (1u << 2)
#define CAP_NET (1u << 3)
#define CAP_AUDIO (1u << 4)
#define CAP_PKG_INSTALL (1u << 5)
#define CAP_ALL 0x3Fu

#define BUNDLE_MAX_FILES 64u
#define BUNDLE_DEPTH_MAX 4u

struct entry {
    char path[PKG_PATH_MAX];
    unsigned long offset;
    unsigned long size;
};

struct bundle {
    struct entry entries[BUNDLE_MAX_FILES];
    unsigned long count;
    unsigned char *payload;
    unsigned long payload_len;
};

static void die(const char *message)
{
    fprintf(stderr, "mkpkg: %s\n", message);
    exit(1);
}

static void usage(void)
{
    fprintf(stderr,
            "usage: mkpkg --seed HEX --out FILE --bundle DIR --id ID"
            " --name NAME --version V --publisher P [--caps LIST]"
            " [--key-id ID] [--timestamp N] [--trust-out FILE]"
            " [--print-key]\n");
    exit(2);
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static int hex_decode(const char *text, unsigned char *out, unsigned long n)
{
    unsigned long i;

    if (strlen(text) != n * 2u)
        return -1;
    for (i = 0; i < n; i++) {
        int hi = hex_value(text[i * 2u]);
        int lo = hex_value(text[i * 2u + 1u]);

        if (hi < 0 || lo < 0)
            return -1;
        out[i] = (unsigned char)((hi << 4) | lo);
    }
    return 0;
}

static void hex_encode(const unsigned char *in, unsigned long n, char *out)
{
    static const char digits[] = "0123456789abcdef";
    unsigned long i;

    for (i = 0; i < n; i++) {
        out[i * 2u] = digits[in[i] >> 4];
        out[i * 2u + 1u] = digits[in[i] & 15u];
    }
    out[n * 2u] = 0;
}

static void put32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v & 0xFFu);
    p[1] = (unsigned char)((v >> 8) & 0xFFu);
    p[2] = (unsigned char)((v >> 16) & 0xFFu);
    p[3] = (unsigned char)((v >> 24) & 0xFFu);
}

static void put64(unsigned char *p, uint64_t v)
{
    unsigned i;

    for (i = 0; i < 8u; i++)
        p[i] = (unsigned char)((v >> (8u * i)) & 0xFFu);
}

static void put_field(unsigned char *raw, unsigned long off, unsigned long cap,
                      const char *value, int allow_space)
{
    unsigned long i;

    if (strlen(value) >= cap)
        die("a header field is too long for its slot");
    for (i = 0; value[i]; i++) {
        unsigned char c = (unsigned char)value[i];

        if (c < 0x21u && !(allow_space && c == ' '))
            die("a header field has a control character");
        if (c > 0x7Eu)
            die("a header field is not ASCII");
    }
    memcpy(raw + off, value, strlen(value));
}

static unsigned long read_file(const char *path, unsigned char *out,
                               unsigned long cap)
{
    FILE *f = fopen(path, "rb");
    unsigned long got;

    if (!f) {
        fprintf(stderr, "mkpkg: cannot open %s: %s\n", path, strerror(errno));
        exit(1);
    }
    got = (unsigned long)fread(out, 1, cap + 1u, f);
    if (ferror(f))
        die("read error");
    fclose(f);
    if (got > cap)
        die("a payload file is larger than 96 KiB");
    return got;
}

static void walk(struct bundle *b, const char *dir, const char *rel,
                 unsigned depth)
{
    DIR *d;
    struct dirent *de;

    if (depth > BUNDLE_DEPTH_MAX)
        die("the bundle is nested too deeply");
    d = opendir(dir);
    if (!d) {
        fprintf(stderr, "mkpkg: cannot read %s: %s\n", dir, strerror(errno));
        exit(1);
    }
    while ((de = readdir(d)) != 0) {
        char full[4096];
        char child[512];
        struct stat st;

        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        if (snprintf(full, sizeof(full), "%s/%s", dir, de->d_name) >=
            (int)sizeof(full))
            die("a bundle path is too long");
        if (rel[0])
            snprintf(child, sizeof(child), "%s/%s", rel, de->d_name);
        else
            snprintf(child, sizeof(child), "%s", de->d_name);
        if (strlen(child) >= PKG_PATH_MAX)
            die("a payload path is longer than 95 characters");
        if (lstat(full, &st) != 0) {
            fprintf(stderr, "mkpkg: cannot stat %s: %s\n", full,
                    strerror(errno));
            exit(1);
        }
        if (S_ISDIR(st.st_mode)) {
            walk(b, full, child, depth + 1u);
        } else if (S_ISREG(st.st_mode)) {
            struct entry *e;
            unsigned long size;

            if (b->count >= PKG_MAX_FILES)
                die("a package holds at most 32 files");
            e = &b->entries[b->count++];
            memset(e, 0, sizeof(*e));
            strcpy(e->path, child);
            e->offset = b->payload_len;
            size = read_file(full, b->payload + b->payload_len,
                             PKG_MAX_FILE_BYTES);
            e->size = size;
            b->payload_len += size;
            if (b->payload_len + 512u > PKG_MAX_BYTES)
                die("the payload exceeds the 128 KiB package limit");
        } else {
            die("a bundle holds regular files and directories only");
        }
    }
    closedir(d);
}

static int entry_cmp(const void *a, const void *b)
{
    return strcmp(((const struct entry *)a)->path,
                  ((const struct entry *)b)->path);
}

static unsigned long build_payload(struct bundle *b, unsigned char **container)
{
    /* The file bytes live in b->payload, so the container is built in a
       second buffer; writing it in place used to clobber the payload. */
    unsigned char *out = calloc(1, PKG_MAX_BYTES);
    unsigned long off = 8u;
    unsigned long i;

    if (!out)
        die("out of memory");

    put32(out, (uint32_t)b->count);
    put32(out + 4u, 0u);
    for (i = 0; i < b->count; i++) {
        const struct entry *e = &b->entries[i];
        unsigned long plen = strlen(e->path);

        if (off + 8u + plen + e->size > PKG_MAX_BYTES)
            die("the payload exceeds the 128 KiB package limit");
        put32(out + off, (uint32_t)plen);
        put32(out + off + 4u, (uint32_t)e->size);
        memcpy(out + off + 8u, e->path, plen);
        memcpy(out + off + 8u + plen, b->payload + e->offset, e->size);
        off += 8u + plen + e->size;
    }
    *container = out;
    return off;
}

static int parse_caps(const char *list, uint32_t *out)
{
    char buf[128];
    char *p;
    uint32_t mask = 0u;

    if (strlen(list) >= sizeof(buf))
        return -1;
    strcpy(buf, list);
    for (p = strtok(buf, ","); p; p = strtok(0, ",")) {
        while (*p == ' ')
            p++;
        if (strcmp(p, "fs.read") == 0)
            mask |= CAP_FS_READ;
        else if (strcmp(p, "fs.write") == 0)
            mask |= CAP_FS_WRITE;
        else if (strcmp(p, "app.launch") == 0)
            mask |= CAP_APP_LAUNCH;
        else if (strcmp(p, "net") == 0)
            mask |= CAP_NET;
        else if (strcmp(p, "audio") == 0)
            mask |= CAP_AUDIO;
        else if (strcmp(p, "pkg.install") == 0)
            mask |= CAP_PKG_INSTALL;
        else if (strcmp(p, "all") == 0)
            mask |= CAP_ALL;
        else
            return -1;
    }
    *out = mask;
    return 0;
}

static int has_manifest(struct bundle *b)
{
    unsigned long i;

    for (i = 0; i < b->count; i++) {
        if (strcmp(b->entries[i].path, "manifest.toml") == 0)
            return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *seed_hex = 0;
    const char *out_path = 0;
    const char *bundle_dir = 0;
    const char *id = 0;
    const char *name = 0;
    const char *version = 0;
    const char *publisher = 0;
    const char *caps_list = 0;
    const char *key_id = "crugos-test";
    const char *trust_out = 0;
    unsigned long long timestamp = 0u;
    unsigned char seed[32];
    unsigned char pk[32];
    unsigned char sk[64];
    unsigned char sig[64];
    unsigned char raw[PKG_HEADER_SIZE];
    unsigned char digest[64];
    uint32_t cap_mask = 0u;
    struct bundle b;
    unsigned char *container = 0;
    unsigned long payload_len;
    FILE *f;
    char pub_hex[65];
    char trust_line[256];
    int i;

    memset(&b, 0, sizeof(b));
    for (i = 1; i < argc; i++) {
        const char *opt = argv[i];
        const char *val = (i + 1 < argc) ? argv[i + 1] : 0;

        if (!val)
            usage();
        if (strcmp(opt, "--seed") == 0)
            seed_hex = val;
        else if (strcmp(opt, "--out") == 0)
            out_path = val;
        else if (strcmp(opt, "--bundle") == 0)
            bundle_dir = val;
        else if (strcmp(opt, "--id") == 0)
            id = val;
        else if (strcmp(opt, "--name") == 0)
            name = val;
        else if (strcmp(opt, "--version") == 0)
            version = val;
        else if (strcmp(opt, "--publisher") == 0)
            publisher = val;
        else if (strcmp(opt, "--caps") == 0)
            caps_list = val;
        else if (strcmp(opt, "--key-id") == 0)
            key_id = val;
        else if (strcmp(opt, "--timestamp") == 0)
            timestamp = strtoull(val, 0, 10);
        else if (strcmp(opt, "--trust-out") == 0)
            trust_out = val;
        else
            usage();
        i++;
    }
    if (!seed_hex || !id || !name || !version || !publisher)
        usage();
    if (hex_decode(seed_hex, seed, 32u) != 0)
        die("--seed must be 64 hex characters");
    if (ed25519_keypair(pk, sk, seed) != 0)
        die("the low level cannot expand the seed");
    if (caps_list && parse_caps(caps_list, &cap_mask) != 0)
        die("unknown capability name (fs.read, fs.write, app.launch, net,"
            " audio, pkg.install, all)");
    if (!out_path && !trust_out)
        usage();
    if (out_path) {
        if (!bundle_dir)
            usage();
        b.payload = calloc(1, PKG_MAX_BYTES);
        if (!b.payload)
            die("out of memory");
        walk(&b, bundle_dir, "", 0u);
        if (b.count == 0u)
            die("the bundle is empty");
        if (!has_manifest(&b))
            die("the bundle has no manifest.toml");
        qsort(b.entries, b.count, sizeof(b.entries[0]), entry_cmp);
        payload_len = build_payload(&b, &container);
        memset(raw, 0, sizeof(raw));
        memcpy(raw + HDR_MAGIC, "CRUGPKG1", 8u);
        put32(raw + HDR_SIZE, (uint32_t)PKG_HEADER_SIZE);
        put32(raw + HDR_FLAGS, 0u);
        put_field(raw, HDR_ID, PKG_ID_MAX, id, 0);
        put_field(raw, HDR_NAME, PKG_NAME_MAX, name, 1);
        put_field(raw, HDR_VERSION, PKG_VERSION_MAX, version, 0);
        put_field(raw, HDR_PUBLISHER, PKG_PUBLISHER_MAX, publisher, 1);
        put_field(raw, HDR_KEY_ID, PKG_KEY_ID_MAX, key_id, 0);
        put32(raw + HDR_CAP_MASK, cap_mask);
        put32(raw + HDR_PAYLOAD_SIZE, (uint32_t)payload_len);
        sha512_hash(digest, container, payload_len);
        memcpy(raw + HDR_PAYLOAD_SHA, digest, 64u);
        memcpy(raw + HDR_PUBKEY, pk, 32u);
        put64(raw + HDR_TIMESTAMP, (uint64_t)timestamp);
        if (ed25519_sign(sig, sk, raw, PKG_HEADER_SIZE) != 0)
            die("signing failed");
        memcpy(raw + HDR_SIGNATURE, sig, 64u);
        f = fopen(out_path, "wb");
        if (!f) {
            fprintf(stderr, "mkpkg: cannot create %s: %s\n", out_path,
                    strerror(errno));
            return 1;
        }
        if (fwrite(raw, 1, sizeof(raw), f) != sizeof(raw) ||
            fwrite(container, 1, payload_len, f) != payload_len) {
            fclose(f);
            die("write error");
        }
        if (fclose(f) != 0)
            die("close error");
        printf("mkpkg: %s %s -> %s (%lu file(s), %lu payload bytes, caps 0x%02x)\n",
               id, version, out_path, b.count, payload_len,
               (unsigned)cap_mask);
    }
    hex_encode(pk, 32u, pub_hex);
    if (trust_out) {
        f = fopen(trust_out, "ab");
        if (!f) {
            fprintf(stderr, "mkpkg: cannot append %s: %s\n", trust_out,
                    strerror(errno));
            return 1;
        }
        snprintf(trust_line, sizeof(trust_line), "pub %s %s\n", pub_hex,
                 publisher);
        fputs(trust_line, f);
        fclose(f);
        printf("mkpkg: trust line appended to %s\n", trust_out);
    }
    printf("mkpkg: public key %s\n", pub_hex);
    free(container);
    free(b.payload);
    return 0;
}