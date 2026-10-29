/* Phase 7B: signed software packages (.crpkg).

   A package is one Ed25519-signed blob: a fixed 512-byte header followed by
   a small container of files.  Nothing in the header is trusted until the
   signature over the header (with the signature field zeroed) verifies
   against a key listed in /System/trusted/keys.conf, and the payload hash
   matches SHA-512 of the bytes that follow the header.

     offset  size  field
     0x000      8  magic "CRUGPKG1"
     0x008      4  header_size (512)
     0x00C      4  flags (reserved, must be 0)
     0x010     32  id
     0x030     32  name
     0x050     16  version (N.N.N)
     0x060     32  publisher
     0x080      4  cap_mask required by the application
     0x084      4  payload_size
     0x088     64  payload_sha512
     0x0C8     32  key_id (first 16 bytes of the public key, hex)
     0x0E8     32  public key
     0x108      8  timestamp (unix seconds, informational)
     0x110     64  signature
     0x150        reserved, zero

   The payload is a container: u32 file_count, then per file u32 path_len,
   u32 data_len, the path, then the data.  Paths are bundle relative and
   must not be absolute or contain '..'; the installer writes them under
   /Applications/<id>.app.  kernel/app/pkg.c verifies, stages, renames and
   records the install in /System/pkgdb/<id>.txt.  The parser, the trust
   list reader, the signature check and the container walker are pure and
   host tested.  See docs/packages.md. */
#ifndef CRUG_PKG_H
#define CRUG_PKG_H

#include <crug/types.h>

#define PKG_MAGIC "CRUGPKG1"
#define PKG_MAGIC_LEN 8u
#define PKG_HEADER_SIZE 512u
#define PKG_SIG_OFFSET 0x110u
#define PKG_SIG_SIZE 64u
#define PKG_PUBKEY_OFFSET 0xE8u
#define PKG_PUBKEY_SIZE 32u

#define PKG_ID_MAX 32u
#define PKG_NAME_MAX 32u
#define PKG_VERSION_MAX 16u
#define PKG_PUBLISHER_MAX 32u
#define PKG_KEY_ID_MAX 32u
#define PKG_PATH_MAX 96u

/* One package file is at most 128 KiB and at most 32 files: everything an
   installer can do to the heap is bounded before a byte is verified. */
#define PKG_MAX_BYTES (128u * 1024u)
#define PKG_MAX_FILES 32u
#define PKG_MAX_FILE_BYTES (96u * 1024u)
#define PKG_MAX_KEYS 4u
#define PKG_INFO_MAX 192u

#define PKG_DIR "/System/packages"
#define PKG_TRUST_PATH "/System/trusted/keys.conf"
#define PKG_DB_DIR "/System/pkgdb"
#define PKG_STAGE_DIR "/Applications/.stage"
#define PKG_LIST_MAX 1024u

struct pkg_header {
    char id[PKG_ID_MAX];
    char name[PKG_NAME_MAX];
    char version[PKG_VERSION_MAX];
    char publisher[PKG_PUBLISHER_MAX];
    char key_id[PKG_KEY_ID_MAX];
    u32 cap_mask;
    u32 payload_size;
    u8 payload_sha512[64];
    u8 pubkey[PKG_PUBKEY_SIZE];
    u64 timestamp;
};

struct pkg_key {
    u8 pubkey[PKG_PUBKEY_SIZE];
    char publisher[PKG_PUBLISHER_MAX];
};

/* Parse one file in the trust list: lines of the form
     pub <64 hex chars> <publisher text>
   blank lines and '#' comments are skipped.  Returns 0 and fills keys[],
   or -1 with *reason set ('trust-key', 'trust-hex', 'trust-line',
   'trust-full'). */
int pkg_trust_parse(const char *text, usize len, struct pkg_key *keys,
                    usize cap, usize *count, const char **reason);

/* The index of the key that matches pk, or -1. */
int pkg_find_key(const struct pkg_key *keys, usize count, const u8 *pk);

/* Validate the fixed header fields.  Returns 0 and fills *out, or -1 with
   *reason set ('short', 'magic', 'header-size', 'flags', 'id', 'name',
   'version', 'publisher', 'payload-size'), never touching out on failure
   except for the fields it managed to copy. */
int pkg_header_parse(const u8 *raw, usize len, struct pkg_header *out,
                     const char **reason);

/* Verify the Ed25519 signature over the 512 header bytes with the signature
   field taken as zero.  Returns 0 when it verifies, -1 otherwise. */
int pkg_signature_ok(const u8 *raw, usize len, const u8 *pubkey,
                     const u8 *sig);

/* Compare SHA-512(payload) with the header.  Returns 0 or -1. */
int pkg_payload_ok(const struct pkg_header *h, const u8 *payload, usize len);

/* Walk the payload container.  On the first call set *off to 8 (just past
   the file count).  Returns 1 with PATH, DATA and DATA_LEN set, 0 when the
   container is exhausted, or -1 with *reason set ('container', 'path',
   'file-size', 'too-many-files', 'trailing'). */
int pkg_payload_next(const u8 *payload, usize len, usize *off,
                     char *path, usize path_cap, const u8 **data,
                     u32 *data_len, const char **reason);

/* The installed package registry (kernel/app/pkg.c).  pkg_init() scans
   /System/pkgdb after the filesystem is mounted and prints
   'CRUGOS: package registry ready (%u package(s))'. */
void pkg_reset(void);
int pkg_init(void);
u32 pkg_count(void);
const struct pkg *pkg_at(u32 index);
const struct pkg *pkg_find_id(const char *id);

struct pkg {
    char id[PKG_ID_MAX];
    char name[PKG_NAME_MAX];
    char version[PKG_VERSION_MAX];
    char publisher[PKG_PUBLISHER_MAX];
    char prog[PKG_PATH_MAX];
    u32 cap_mask;
};

/* One record for SYS_PKG_INFO: id|name|version|publisher|caps|prog.
   Returns the byte count, or -1 when it does not fit. */
int pkg_format_info(const struct pkg *p, char *out, usize cap);

/* Read and validate a package without installing it: fills *out and sets
   *out_trusted to 1 when the signing key is in the trust list.  Returns 0
   or -1 with *reason set. */
int pkg_probe(const char *path, struct pkg_header *out, int *out_trusted,
              const char **reason);

/* Install PATH: verify, stage under /Applications/.stage-<id>, write the
   files, rename to /Applications/<id>.app and record the install.  A
   failure before the rename removes the stage directory, so a bad package
   never leaves a half-application behind.  Returns 0 or -1 with *reason. */
int pkg_install(const char *path, const char **reason);

/* Human readable name of a reason string, for the installer window. */
const char *pkg_reason_text(const char *reason);

/* Phase 7B boot selftests (kernel/core/pkg_test.c):
   pkg.test=1 -- install path, pkg.test=2 -- capability wall. */
struct boot_info;
void pkg_test_cmdline(const struct boot_info *bi);
int pkg_selftest(void);

#endif /* CRUG_PKG_H */
