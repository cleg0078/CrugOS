/* Host tests for the phase 7B signed package container, the capability model
 * and the permission policy. Every parser here reads bytes that arrive from
 * a package file, so each is checked at its boundary and with a tampered
 * input. The installer itself needs a mounted root and is gated in QEMU by
 * make test-pkg and make test-caps. */
#include <crug/caps.h>
#include <crug/ed25519.h>
#include <crug/ext4.h>
#include <crug/perms.h>
#include <crug/pkg.h>
#include <crug/sha512.h>
#include <crug/storage.h>
#include <string.h>
#include "test.h"

/* ---- storage stubs: the pure helpers never touch a mounted root -------- */

int ext4_lookup(struct ext4_fs *fs, const char *path, u32 *ino)
{
    return -1;
}

int ext4_read_file(struct ext4_fs *fs, const char *path, void *buf, usize cap,
                   usize *out)
{
    return -1;
}

int ext4_list(struct ext4_fs *fs, const char *path, char *out, usize cap)
{
    return -1;
}

int ext4_create(struct ext4_fs *fs, const char *path, u16 mode)
{
    return -1;
}

int ext4_mkdir(struct ext4_fs *fs, const char *path, u16 mode)
{
    return -1;
}

int ext4_write_file(struct ext4_fs *fs, const char *path, const void *buf,
                    usize len)
{
    return -1;
}

int ext4_unlink(struct ext4_fs *fs, const char *path)
{
    return -1;
}

int ext4_rmdir(struct ext4_fs *fs, const char *path)
{
    return -1;
}

int ext4_rename(struct ext4_fs *fs, const char *oldpath, const char *newpath)
{
    return -1;
}

int ext4_fsync(struct ext4_fs *fs)
{
    return -1;
}

int storage_present(void)
{
    return 0;
}

struct ext4_fs *storage_root(void)
{
    return 0;
}

int storage_remount_rw(void)
{
    return -1;
}

int storage_remount_ro(void)
{
    return 0;
}

/* ---- byte helpers ------------------------------------------------------ */

static void put32(u8 *p, u32 v)
{
    p[0] = (u8)(v & 0xFFu);
    p[1] = (u8)((v >> 8) & 0xFFu);
    p[2] = (u8)((v >> 16) & 0xFFu);
    p[3] = (u8)((v >> 24) & 0xFFu);
}

static void put64(u8 *p, u64 v)
{
    put32(p, (u32)(v & 0xFFFFFFFFu));
    put32(p + 4, (u32)(v >> 32));
}

static int hexval(char c)
{
    if (c >= 48 && c <= 57) {
        return c - 48;
    }
    if (c >= 97 && c <= 102) {
        return c - 87;
    }
    if (c >= 65 && c <= 70) {
        return c - 55;
    }
    return -1;
}

/* Decode exactly n bytes from a 2n character hex string. */
static int unhex(const char *s, u8 *out, usize n)
{
    usize i;

    if (strlen(s) != n * 2u) {
        return -1;
    }
    for (i = 0; i < n; i++) {
        int hi = hexval(s[2u * i]);
        int lo = hexval(s[2u * i + 1u]);

        if (hi < 0 || lo < 0) {
            return -1;
        }
        out[i] = (u8)((hi << 4) | lo);
    }
    return 0;
}

/* Encode n bytes as 2n lowercase hex characters (no terminator). */
static void tohex(char *out, const u8 *in, usize n)
{
    static const char digits[] = "0123456789abcdef";
    usize i;

    for (i = 0; i < n; i++) {
        out[2u * i] = digits[in[i] >> 4];
        out[2u * i + 1u] = digits[in[i] & 0x0Fu];
    }
}

/* Copy a NUL terminated field into a fixed header slot, zero padded. */
static void put_field(u8 *dst, usize cap, const char *s)
{
    usize i;

    for (i = 0; i < cap; i++) {
        dst[i] = 0;
    }
    for (i = 0; i + 1u < cap && s[i] != 0; i++) {
        dst[i] = (u8)s[i];
    }
}

/* Build a 512 byte header for PAYLOAD. When SEED is not NULL the header is
   signed with the matching key and PUB_OUT receives the public key. */
static void build_header(u8 *raw, const char *id, const char *version,
                         const u8 *payload, usize plen, const u8 *seed,
                         u8 *pub_out)
{
    memcpy(raw, PKG_MAGIC, PKG_MAGIC_LEN);
    put32(raw + 0x08u, PKG_HEADER_SIZE);
    put32(raw + 0x0Cu, 0u);
    put_field(raw + 0x10u, PKG_ID_MAX, id);
    put_field(raw + 0x30u, PKG_NAME_MAX, "CapTest");
    put_field(raw + 0x50u, PKG_VERSION_MAX, version);
    put_field(raw + 0x60u, PKG_PUBLISHER_MAX, "Crug Labs");
    put32(raw + 0x80u, CAP_FS_READ);
    put32(raw + 0x84u, (u32)plen);
    sha512_hash(raw + 0x88u, payload, plen);
    put_field(raw + 0xC8u, PKG_KEY_ID_MAX, "crug-test");
    put64(raw + 0x108u, 0u);
    if (seed != 0) {
        u8 sk[ED25519_SK_SIZE];
        u8 pk[ED25519_PK_SIZE];
        char kid[PKG_KEY_ID_MAX];

        if (ed25519_keypair(pk, sk, seed) == 0) {
            memcpy(raw + PKG_PUBKEY_OFFSET, pk, PKG_PUBKEY_SIZE);
            tohex(kid, pk, 16u);
            memcpy(raw + 0xC8u, kid, PKG_KEY_ID_MAX);
            memset(raw + PKG_SIG_OFFSET, 0, PKG_SIG_SIZE);
            (void)ed25519_sign(raw + PKG_SIG_OFFSET, sk, raw,
                               PKG_HEADER_SIZE);
            if (pub_out != 0) {
                memcpy(pub_out, pk, PKG_PUBKEY_SIZE);
            }
        }
    }
}

/* Append one container entry (path and data) and return the new offset. */
static usize put_entry(u8 *out, usize off, const char *path, const char *data)
{
    usize plen = strlen(path);
    usize dlen = strlen(data);

    put32(out + off, (u32)plen);
    put32(out + off + 4u, (u32)dlen);
    off += 8u;
    memcpy(out + off, path, plen);
    off += plen;
    memcpy(out + off, data, dlen);
    return off + dlen;
}

/* Build a two file payload container; returns its byte count. */
static usize build_payload(u8 *out)
{
    usize off;

    put32(out, 2u);
    put32(out + 4u, 0u);
    off = 8u;
    off = put_entry(out, off, "manifest.toml", "[app]");
    off = put_entry(out, off, "bin/x86_64/captest", "MZ fake elf");
    return off;
}

static const char SEED_HEX[] =
    "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60";
static const char PUB_HEX[] =
    "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a";
static const char SIG_HEX[] =
    "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155"
    "5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b";

/* ---- SHA-512 ----------------------------------------------------------- */

static void check_digest(const char *text, const char *want_hex)
{
    u8 got[SHA512_DIGEST_SIZE];
    u8 want[SHA512_DIGEST_SIZE];

    CHECK(unhex(want_hex, want, SHA512_DIGEST_SIZE) == 0);
    sha512_hash(got, text, strlen(text));
    CHECK(memcmp(got, want, SHA512_DIGEST_SIZE) == 0);
}

static void test_pkg_sha512(void)
{
    struct sha512_ctx ctx;
    u8 a[SHA512_DIGEST_SIZE];
    u8 b[SHA512_DIGEST_SIZE];

    check_digest("",
        "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce"
        "47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e");
    check_digest("abc",
        "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
        "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f");
    check_digest("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
        "204a8fc6dda82f0a0ced7beb8e08a41657c16ef468b228a8279be331a703c335"
        "96fd15c13b1b07f9aa1d3bea57789ca031ad85c7a71dd70354ec631238ca3445");
    /* A streaming update in odd sized pieces must equal the one shot hash. */
    sha512_init(&ctx);
    sha512_update(&ctx, "abc", 1u);
    sha512_update(&ctx, "bc", 2u);
    sha512_final(&ctx, a);
    sha512_hash(b, "abc", 3u);
    CHECK(memcmp(a, b, SHA512_DIGEST_SIZE) == 0);
}

/* ---- Ed25519 (RFC 8032 test 1 and test 2) ----------------------------- */

static void test_ed25519_vector(const char *seed_hex, const char *pub_hex,
                               const u8 *msg, usize len,
                               const char *sig_hex)
{
    u8 seed[ED25519_SEED_SIZE];
    u8 want_pk[ED25519_PK_SIZE];
    u8 want_sig[ED25519_SIG_SIZE];
    u8 pk[ED25519_PK_SIZE];
    u8 sk[ED25519_SK_SIZE];
    u8 sig[ED25519_SIG_SIZE];

    CHECK(unhex(seed_hex, seed, ED25519_SEED_SIZE) == 0);
    CHECK(unhex(pub_hex, want_pk, ED25519_PK_SIZE) == 0);
    CHECK(unhex(sig_hex, want_sig, ED25519_SIG_SIZE) == 0);
    CHECK(ed25519_keypair(pk, sk, seed) == 0);
    CHECK(memcmp(pk, want_pk, ED25519_PK_SIZE) == 0);
    CHECK(memcmp(sk, seed, ED25519_SEED_SIZE) == 0);
    CHECK(memcmp(sk + ED25519_SEED_SIZE, pk, ED25519_PK_SIZE) == 0);
    CHECK(ed25519_sign(sig, sk, msg, len) == 0);
    CHECK(memcmp(sig, want_sig, ED25519_SIG_SIZE) == 0);
    CHECK(ed25519_verify(pk, msg, len, sig) == 0);
}

static void test_pkg_ed25519(void)
{
    static const u8 one[] = { 0x72u };
    u8 seed[ED25519_SEED_SIZE];
    u8 pk[ED25519_PK_SIZE];
    u8 sk[ED25519_SK_SIZE];
    u8 sig[ED25519_SIG_SIZE];
    u8 other_pk[ED25519_PK_SIZE];
    u8 other_sk[ED25519_SK_SIZE];
    u8 other_seed[ED25519_SEED_SIZE];

    test_ed25519_vector(SEED_HEX, PUB_HEX, 0, 0u, SIG_HEX);
    test_ed25519_vector(
        "4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb",
        "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c",
        one, 1u,
        "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da"
        "085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00");
    CHECK(unhex(SEED_HEX, seed, ED25519_SEED_SIZE) == 0);
    CHECK(ed25519_keypair(pk, sk, seed) == 0);
    CHECK(ed25519_sign(sig, sk, "crug", 4u) == 0);
    /* A changed message, a flipped signature bit and a foreign key all fail. */
    CHECK(ed25519_verify(pk, "crux", 4u, sig) == -1);
    CHECK(ed25519_verify(pk, "crug", 3u, sig) == -1);
    sig[0] ^= 0x01u;
    CHECK(ed25519_verify(pk, "crug", 4u, sig) == -1);
    sig[0] ^= 0x01u;
    CHECK(ed25519_verify(pk, "crug", 4u, sig) == 0);
    memset(other_seed, 0x5Au, sizeof(other_seed));
    CHECK(ed25519_keypair(other_pk, other_sk, other_seed) == 0);
    CHECK(memcmp(other_pk, pk, ED25519_PK_SIZE) != 0);
    CHECK(ed25519_verify(other_pk, "crug", 4u, sig) == -1);
    /* A non-canonical point is rejected, never accepted as identity. */
    memset(other_pk, 0xFF, sizeof(other_pk));
    CHECK(ed25519_verify(other_pk, "crug", 4u, sig) == -1);
}

/* ---- capability names -------------------------------------------------- */

static void test_pkg_caps(void)
{
    char buf[64];
    char bad[CAP_NAME_MAX];
    u32 mask = 0u;

    CHECK_STR(cap_name(CAP_FS_READ), "fs.read");
    CHECK_STR(cap_name(CAP_FS_WRITE), "fs.write");
    CHECK_STR(cap_name(CAP_APP_LAUNCH), "app.launch");
    CHECK_STR(cap_name(CAP_NET), "net");
    CHECK_STR(cap_name(CAP_AUDIO), "audio");
    CHECK_STR(cap_name(CAP_PKG_INSTALL), "pkg.install");
    CHECK(cap_name(CAP_ALL) == 0);
    CHECK(cap_name(1u << 20) == 0);
    CHECK(cap_from_name("fs.read") == CAP_FS_READ);
    CHECK(cap_from_name("pkg.install") == CAP_PKG_INSTALL);
    CHECK(cap_from_name("bogus") == 0u);
    CHECK(cap_from_name(0) == 0u);
    CHECK(cap_parse_list("", 0u, &mask, bad, sizeof(bad)) == 0);
    CHECK(mask == CAP_NONE);
    CHECK(cap_parse_list("fs.read,net", 11u, &mask, bad, sizeof(bad)) == 0);
    CHECK(mask == (CAP_FS_READ | CAP_NET));
    CHECK(cap_parse_list(" fs.read , net ", strlen(" fs.read , net "),
                         &mask, bad, sizeof(bad)) == 0);
    CHECK(mask == (CAP_FS_READ | CAP_NET));
    CHECK(cap_parse_list("fs.read,zzz", 11u, &mask, bad,
                         sizeof(bad)) == -1);
    CHECK_STR(bad, "zzz");
    CHECK(cap_parse_list("fs.read,,net", 12u, &mask, bad,
                         sizeof(bad)) == 0);
    CHECK(mask == (CAP_FS_READ | CAP_NET));
    CHECK(cap_format(CAP_NONE, buf, sizeof(buf)) == 4);
    CHECK_STR(buf, "none");
    CHECK(cap_format(CAP_FS_READ | CAP_NET, buf, sizeof(buf)) == 11);
    CHECK_STR(buf, "fs.read,net");
    CHECK(cap_format(CAP_ALL, buf, sizeof(buf)) == 49);
    CHECK_STR(buf, "fs.read,fs.write,app.launch,net,audio,pkg.install");
    CHECK(cap_format(CAP_ALL, buf, 4u) == -1);
    CHECK(cap_intersect(CAP_ALL, CAP_NET) == CAP_NET);
    CHECK(cap_intersect(CAP_FS_READ | CAP_NET, CAP_NET | CAP_AUDIO) ==
          CAP_NET);
    CHECK(cap_intersect(CAP_NONE, CAP_ALL) == CAP_NONE);
}

/* ---- permission policy ------------------------------------------------- */

static void test_pkg_perms(void)
{
    struct perms_policy pol;
    const char *why = 0;
    char text[512];

    sprintf(text, "# policy%cdefault fs.read,app.launch%ccaptest fs.read%c",
            10, 10, 10);
    CHECK(perms_parse(text, strlen(text), &pol, &why) == 0);
    CHECK(pol.has_default == 1);
    CHECK(pol.default_mask == (CAP_FS_READ | CAP_APP_LAUNCH));
    CHECK(pol.count == 1u);
    CHECK_STR(pol.rules[0].id, "captest");
    CHECK(perms_granted(&pol, "captest") == CAP_FS_READ);
    CHECK(perms_granted(&pol, "other.app") ==
          (CAP_FS_READ | CAP_APP_LAUNCH));
    CHECK(perms_granted(&pol, 0) == (CAP_FS_READ | CAP_APP_LAUNCH));
    CHECK(perms_granted(&pol, "") == (CAP_FS_READ | CAP_APP_LAUNCH));
    /* An empty policy grants nothing at all. */
    CHECK(perms_parse("", 0u, &pol, &why) == 0);
    CHECK(pol.has_default == 0);
    CHECK(pol.count == 0u);
    CHECK(perms_granted(&pol, "captest") == CAP_NONE);
    CHECK(perms_granted(0, "captest") == CAP_NONE);
    /* A rule with no capabilities parses as an explicit denial. */
    why = 0;
    CHECK(perms_parse("captest", 7u, &pol, &why) == 0);
    CHECK(pol.count == 1u);
    CHECK(perms_granted(&pol, "captest") == CAP_NONE);
    /* Bad capability, duplicate default, bad id and a full table. */
    why = 0;
    sprintf(text, "captest zzz%c", 10);
    CHECK(perms_parse(text, strlen(text), &pol, &why) == -1);
    CHECK_STR(why, "perms-cap");
    why = 0;
    sprintf(text, "default net%cdefault net%c", 10, 10);
    CHECK(perms_parse(text, strlen(text), &pol, &why) == -1);
    CHECK_STR(why, "perms-dup");
    why = 0;
    sprintf(text, "bad/id net%c", 10);
    CHECK(perms_parse(text, strlen(text), &pol, &why) == -1);
    CHECK_STR(why, "perms-id");
    why = 0;
    CHECK(perms_parse(0, 0u, &pol, &why) == -1);
    CHECK_STR(why, "perms-line");
    why = 0;
    sprintf(text,
            "a net%cb net%cc net%cd net%ce net%cf net%cg net%ch net%ci net%c",
            10, 10, 10, 10, 10, 10, 10, 10, 10);
    CHECK(perms_parse(text, strlen(text), &pol, &why) == -1);
    CHECK_STR(why, "perms-full");
}

/* ---- the trust list ---------------------------------------------------- */

static void test_pkg_trust(void)
{
    struct pkg_key keys[PKG_MAX_KEYS];
    usize count = 0u;
    const char *why = 0;
    u8 pk[PKG_PUBKEY_SIZE];
    u8 other[PKG_PUBKEY_SIZE];
    char text[512];
    char pubhex[65];
    char otherhex[65];

    CHECK(unhex(PUB_HEX, pk, PKG_PUBKEY_SIZE) == 0);
    memset(other, 0x33, sizeof(other));
    tohex(pubhex, pk, PKG_PUBKEY_SIZE);
    pubhex[64] = 0;
    tohex(otherhex, other, PKG_PUBKEY_SIZE);
    otherhex[64] = 0;
    sprintf(text, "# trusted keys%cpub %s Crug Labs%cpub %s Rogue%c", 10,
            pubhex, 10, otherhex, 10);
    CHECK(pkg_trust_parse(text, strlen(text), keys, PKG_MAX_KEYS, &count,
                          &why) == 0);
    CHECK(count == 2u);
    CHECK(pkg_find_key(keys, count, pk) == 0);
    CHECK(pkg_find_key(keys, count, other) == 1);
    CHECK_STR(keys[0].publisher, "Crug Labs");
    memset(other, 0x44, sizeof(other));
    CHECK(pkg_find_key(keys, count, other) == -1);
    CHECK(pkg_find_key(keys, 0u, pk) == -1);
    CHECK(pkg_find_key(0, 1u, pk) == -1);
    /* A truncated key, a non hex key and a missing publisher all fail. */
    why = 0;
    sprintf(text, "pub 1234 Crug Labs%c", 10);
    CHECK(pkg_trust_parse(text, strlen(text), keys, PKG_MAX_KEYS, &count,
                          &why) == -1);
    CHECK_STR(why, "trust-hex");
    why = 0;
    CHECK(pkg_trust_parse("pub zz", 6u, keys, PKG_MAX_KEYS, &count,
                          &why) == -1);
    CHECK_STR(why, "trust-hex");
    why = 0;
    CHECK(pkg_trust_parse("key abc", 7u, keys, PKG_MAX_KEYS, &count,
                          &why) == -1);
    CHECK_STR(why, "trust-line");
    /* A full table rejects one more key instead of overwriting. */
    why = 0;
    sprintf(text, "pub %s A%cpub %s B%cpub %s C%cpub %s D%cpub %s E%c",
            pubhex, 10, pubhex, 10, pubhex, 10, pubhex, 10, pubhex, 10);
    CHECK(pkg_trust_parse(text, strlen(text), keys, 4u, &count, &why) == -1);
    CHECK_STR(why, "trust-full");
}

/* ---- header parse, payload hash and container walker ------------------- */

static void test_pkg_header(void)
{
    u8 payload[256];
    u8 raw[PKG_HEADER_SIZE];
    u8 pub[PKG_PUBKEY_SIZE];
    usize plen = build_payload(payload);
    struct pkg_header h;
    const char *why = 0;

    build_header(raw, "captest", "1.0.0", payload, plen, 0, 0);
    CHECK(pkg_header_parse(raw, PKG_HEADER_SIZE, &h, &why) == 0);
    CHECK_STR(h.id, "captest");
    CHECK_STR(h.name, "CapTest");
    CHECK_STR(h.version, "1.0.0");
    CHECK_STR(h.publisher, "Crug Labs");
    CHECK(h.cap_mask == CAP_FS_READ);
    CHECK(h.payload_size == (u32)plen);
    CHECK(h.timestamp == 0u);
    CHECK(pkg_payload_ok(&h, payload, plen) == 0);
    CHECK(pkg_payload_ok(&h, payload, plen - 1u) == -1);
    CHECK(pkg_payload_ok(0, payload, plen) == -1);
    payload[0] ^= 0x10u;
    CHECK(pkg_payload_ok(&h, payload, plen) == -1);
    payload[0] ^= 0x10u;
    CHECK(pkg_payload_ok(&h, payload, plen) == 0);
    /* Every fixed field is validated before any of it is believed. */
    why = 0;
    CHECK(pkg_header_parse(raw, PKG_HEADER_SIZE - 1u, &h, &why) == -1);
    CHECK_STR(why, "short");
    raw[0] = (u8)'X';
    why = 0;
    CHECK(pkg_header_parse(raw, PKG_HEADER_SIZE, &h, &why) == -1);
    CHECK_STR(why, "magic");
    raw[0] = (u8)'C';
    put32(raw + 0x08u, 1024u);
    why = 0;
    CHECK(pkg_header_parse(raw, PKG_HEADER_SIZE, &h, &why) == -1);
    CHECK_STR(why, "header-size");
    put32(raw + 0x08u, PKG_HEADER_SIZE);
    put32(raw + 0x0Cu, 1u);
    why = 0;
    CHECK(pkg_header_parse(raw, PKG_HEADER_SIZE, &h, &why) == -1);
    CHECK_STR(why, "flags");
    put32(raw + 0x0Cu, 0u);
    raw[0x10u] = 0u;
    why = 0;
    CHECK(pkg_header_parse(raw, PKG_HEADER_SIZE, &h, &why) == -1);
    CHECK_STR(why, "id");
    put_field(raw + 0x10u, PKG_ID_MAX, "captest");
    raw[0x10u] = (u8)'/';
    why = 0;
    CHECK(pkg_header_parse(raw, PKG_HEADER_SIZE, &h, &why) == -1);
    CHECK_STR(why, "id");
    put_field(raw + 0x10u, PKG_ID_MAX, "captest");
    put_field(raw + 0x50u, PKG_VERSION_MAX, "v1");
    why = 0;
    CHECK(pkg_header_parse(raw, PKG_HEADER_SIZE, &h, &why) == -1);
    CHECK_STR(why, "version");
    put_field(raw + 0x50u, PKG_VERSION_MAX, "1.0.0");
    put_field(raw + 0x30u, PKG_NAME_MAX, "Bad/Name");
    why = 0;
    CHECK(pkg_header_parse(raw, PKG_HEADER_SIZE, &h, &why) == -1);
    CHECK_STR(why, "name");
    put_field(raw + 0x30u, PKG_NAME_MAX, "CapTest");
    put_field(raw + 0x60u, PKG_PUBLISHER_MAX, "Bad:Publisher");
    why = 0;
    CHECK(pkg_header_parse(raw, PKG_HEADER_SIZE, &h, &why) == -1);
    CHECK_STR(why, "publisher");
    put_field(raw + 0x60u, PKG_PUBLISHER_MAX, "Crug Labs");
    /* A signed header verifies, and one changed byte anywhere breaks it. */
    build_header(raw, "captest", "1.0.0", payload, plen, 0, 0);
    {
        u8 seed[ED25519_SEED_SIZE];

        CHECK(unhex(SEED_HEX, seed, ED25519_SEED_SIZE) == 0);
        CHECK(unhex(PUB_HEX, pub, PKG_PUBKEY_SIZE) == 0);
        build_header(raw, "captest", "1.0.0", payload, plen, seed, 0);
    }
    CHECK(pkg_signature_ok(raw, PKG_HEADER_SIZE, pub, raw + PKG_SIG_OFFSET)
          == 0);
    raw[0x20u] ^= 0x01u;
    CHECK(pkg_signature_ok(raw, PKG_HEADER_SIZE, pub, raw + PKG_SIG_OFFSET)
          == -1);
    raw[0x20u] ^= 0x01u;
    CHECK(pkg_signature_ok(raw, PKG_HEADER_SIZE, pub, raw + PKG_SIG_OFFSET)
          == 0);
    raw[PKG_SIG_OFFSET] ^= 0x01u;
    CHECK(pkg_signature_ok(raw, PKG_HEADER_SIZE, pub, raw + PKG_SIG_OFFSET)
          == -1);
}

static void test_pkg_container(void)
{
    u8 payload[256];
    usize plen;
    usize off;
    const u8 *data = 0;
    u32 dlen = 0u;
    char path[PKG_PATH_MAX];
    const char *why = 0;

    plen = build_payload(payload);
    off = 0u;
    CHECK(pkg_payload_next(payload, plen, &off, path, sizeof(path), &data,
                           &dlen, &why) == 1);
    CHECK_STR(path, "manifest.toml");
    CHECK(dlen == 5u);
    CHECK(memcmp(data, "[app]", 5u) == 0);
    CHECK(pkg_payload_next(payload, plen, &off, path, sizeof(path), &data,
                           &dlen, &why) == 1);
    CHECK_STR(path, "bin/x86_64/captest");
    CHECK(dlen == 11u);
    CHECK(memcmp(data, "MZ fake elf", 11u) == 0);
    CHECK(pkg_payload_next(payload, plen, &off, path, sizeof(path), &data,
                           &dlen, &why) == 0);
    /* An offset inside the eight byte container header is clamped up. */
    off = 0u;
    CHECK(pkg_payload_next(payload, 8u, &off, path, sizeof(path), &data,
                           &dlen, &why) == 0);
    CHECK(off == 8u);
    /* A container that ends inside the next entry header is damaged. */
    off = 0u;
    why = 0;
    CHECK(pkg_payload_next(payload, 10u, &off, path, sizeof(path), &data,
                           &dlen, &why) == -1);
    CHECK_STR(why, "container");
    /* A zero length path, a path over the format limit and a path that does
       not fit the caller buffer are all refused, never truncated. */
    put32(payload + 8u, 0u);
    off = 0u;
    why = 0;
    CHECK(pkg_payload_next(payload, plen, &off, path, sizeof(path), &data,
                           &dlen, &why) == -1);
    CHECK_STR(why, "path");
    put32(payload + 8u, PKG_PATH_MAX + 1u);
    off = 0u;
    why = 0;
    CHECK(pkg_payload_next(payload, plen, &off, path, 128u, &data, &dlen,
                           &why) == -1);
    CHECK_STR(why, "path");
    put32(payload + 8u, 13u);
    off = 0u;
    why = 0;
    CHECK(pkg_payload_next(payload, plen, &off, path, 4u, &data, &dlen,
                           &why) == -1);
    CHECK_STR(why, "path");
    /* A single file larger than the format allows is a file-size error. */
    put32(payload + 12u, PKG_MAX_FILE_BYTES + 1u);
    off = 0u;
    why = 0;
    CHECK(pkg_payload_next(payload, plen, &off, path, sizeof(path), &data,
                           &dlen, &why) == -1);
    CHECK_STR(why, "file-size");
    /* A missing payload pointer is a damaged container, not a crash. */
    off = 0u;
    why = 0;
    CHECK(pkg_payload_next(0, plen, &off, path, sizeof(path), &data, &dlen,
                           &why) == -1);
    CHECK_STR(why, "container");
}

/* ---- the registry record ---------------------------------------------- */

static void test_pkg_info(void)
{
    struct pkg p;
    char out[PKG_INFO_MAX];

    memset(&p, 0, sizeof(p));
    put_field((u8 *)p.id, sizeof(p.id), "captest");
    put_field((u8 *)p.name, sizeof(p.name), "CapTest");
    put_field((u8 *)p.version, sizeof(p.version), "1.0.0");
    put_field((u8 *)p.publisher, sizeof(p.publisher), "Crug Labs");
    put_field((u8 *)p.prog, sizeof(p.prog), "bin/x86_64/captest");
    p.cap_mask = CAP_FS_READ;
    CHECK(pkg_format_info(&p, out, sizeof(out)) > 0);
    CHECK_STR(out, "captest|CapTest|1.0.0|Crug Labs|fs.read|bin/x86_64/captest");
    CHECK(pkg_format_info(&p, out, 8u) == -1);
    CHECK(pkg_format_info(0, out, sizeof(out)) == -1);
    p.cap_mask = CAP_NONE;
    CHECK(pkg_format_info(&p, out, sizeof(out)) > 0);
    CHECK_STR(out, "captest|CapTest|1.0.0|Crug Labs|none|bin/x86_64/captest");
}

static void test_pkg_reasons(void)
{
    CHECK(pkg_reason_text("magic") != 0);
    CHECK_STR(pkg_reason_text("magic"), "not a CrugOS package");
    CHECK_STR(pkg_reason_text("signature"), "bad signature");
    CHECK_STR(pkg_reason_text("untrusted-key"), "untrusted signing key");
    CHECK_STR(pkg_reason_text("trust-full"), "bad trust list");
    CHECK_STR(pkg_reason_text(0), "unknown");
    /* An unrecognised reason is handed back verbatim, never NULL. */
    CHECK_STR(pkg_reason_text("no-such-reason"), "no-such-reason");
}

void test_pkg_functions(void)
{
    RUN(test_pkg_sha512);
    RUN(test_pkg_ed25519);
    RUN(test_pkg_caps);
    RUN(test_pkg_perms);
    RUN(test_pkg_trust);
    RUN(test_pkg_header);
    RUN(test_pkg_container);
    RUN(test_pkg_info);
    RUN(test_pkg_reasons);
}
