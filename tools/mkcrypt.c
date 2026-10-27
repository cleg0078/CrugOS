/* mkcrypt: create and open CrugOS crypt.device volumes from the host.
 *
 *   mkcrypt format IMAGE --plain FSIMAGE --password PASS
 *                  [--recovery PASS] [--m KIB] [--t N] [--p N]
 *   mkcrypt extract IMAGE OUT --password PASS
 *
 * format writes the 16 KiB crypt header at the start of IMAGE, copies the plain
 * ext4 filesystem image FSIMAGE (size = data area) into the data area and
 * encrypts it with AES-256-XTS. extract decrypts the data area back into OUT so
 * host checks (e2fsck/debugfs) can inspect and replay encrypted volumes.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include <crug/argon2.h>
#include <crug/crypt.h>
#include <crug/xts.h>

/* The kernel page allocator is not part of the host tool. */
u64 pmm_alloc_pages(u64 count)
{
    (void)count;
    return 0;
}

void pmm_free_pages(u64 pa, u64 count)
{
    (void)pa;
    (void)count;
}

#define DATA_OFFSET ((u64)CRUG_CRYPT_HEADER_SECTORS * 512u)

static void fail(const char *what)
{
    fprintf(stderr, "mkcrypt: %s\n", what);
    exit(1);
}

static void fail_errno(const char *what)
{
    fprintf(stderr, "mkcrypt: %s: %s\n", what, strerror(errno));
    exit(1);
}

static void usage(void)
{
    fprintf(stderr,
            "usage: mkcrypt format IMAGE --plain FSIMAGE --password PASS\n"
            "                 [--recovery PASS] [--m KIB] [--t N] [--p N]\n"
            "       mkcrypt extract IMAGE OUT --password PASS\n");
    exit(2);
}

static void random_bytes(u8 *out, usize len)
{
    int fd = open("/dev/urandom", O_RDONLY);
    usize done = 0;

    if (fd < 0) {
        fail_errno("open /dev/urandom");
    }
    while (done < len) {
        ssize_t n = read(fd, out + done, len - done);

        if (n <= 0) {
            fail_errno("read /dev/urandom");
        }
        done += (usize)n;
    }
    close(fd);
}

static void read_at(int fd, void *buf, usize len, u64 off, const char *what)
{
    u8 *p = (u8 *)buf;

    while (len > 0) {
        ssize_t n = pread(fd, p, len, (off_t)off);

        if (n < 0) {
            fail_errno(what);
        }
        if (n == 0) {
            fail(what);
        }
        p += n;
        off += (u64)n;
        len -= (usize)n;
    }
}

static void write_at(int fd, const void *buf, usize len, u64 off, const char *what)
{
    const u8 *p = (const u8 *)buf;

    while (len > 0) {
        ssize_t n = pwrite(fd, p, len, (off_t)off);

        if (n <= 0) {
            fail_errno(what);
        }
        p += n;
        off += (u64)n;
        len -= (usize)n;
    }
}

static int parse_option(int argc, char **argv, int *i, const char *name, const char **out)
{
    if (strcmp(argv[*i], name) != 0) {
        return 0;
    }
    if (*i + 1 >= argc) {
        fail("missing option value");
    }
    *out = argv[++*i];
    return 1;
}

static int parse_u32_option(int argc, char **argv, int *i, const char *name, u32 *out)
{
    const char *value = 0;

    if (!parse_option(argc, argv, i, name, &value)) {
        return 0;
    }
    *out = (u32)strtoul(value, 0, 0);
    return 1;
}

struct options {
    const char *image;
    const char *plain;
    const char *out;
    const char *password;
    const char *recovery;
    u32 m_cost;
    u32 t_cost;
    u32 lanes;
};

static void parse_common(int argc, char **argv, int start, struct options *o)
{
    int i;

    for (i = start; i < argc; i++) {
        if (parse_option(argc, argv, &i, "--plain", &o->plain)) {
            continue;
        }
        if (parse_option(argc, argv, &i, "--password", &o->password)) {
            continue;
        }
        if (parse_option(argc, argv, &i, "--recovery", &o->recovery)) {
            continue;
        }
        if (parse_u32_option(argc, argv, &i, "--m", &o->m_cost)) {
            continue;
        }
        if (parse_u32_option(argc, argv, &i, "--t", &o->t_cost)) {
            continue;
        }
        if (parse_u32_option(argc, argv, &i, "--p", &o->lanes)) {
            continue;
        }
        fail("unknown option");
    }
    if (o->password == 0 || o->password[0] == 0) {
        fail("--password is required");
    }
    if (strlen(o->password) > CRUG_CRYPT_MAX_PASSWORD) {
        fail("password is too long");
    }
    if (o->recovery != 0 && strlen(o->recovery) > CRUG_CRYPT_MAX_PASSWORD) {
        fail("recovery password is too long");
    }
}

static u64 image_sectors(int fd)
{
    struct stat st;
    u64 sectors;

    if (fstat(fd, &st) != 0) {
        fail_errno("stat image");
    }
    if (st.st_size < 0 || (u64)st.st_size % 512u != 0) {
        fail("image size must be a multiple of 512 bytes");
    }
    sectors = (u64)st.st_size / 512u;
    if (sectors <= CRUG_CRYPT_HEADER_SECTORS + 1) {
        fail("image is too small for a crypt volume");
    }
    return sectors;
}

static void format_volume(const struct options *o)
{
    int image_fd;
    int plain_fd;
    struct stat st;
    u64 sectors;
    u64 data_sectors;
    u64 plain_bytes;
    u8 salt[32];
    u8 volume_id[16];
    u8 master[CRUG_CRYPT_MASTER_BYTES];
    u8 nonce[32];
    u8 header[CRUG_CRYPT_HEADER_BYTES];
    u8 sector[512];
    void *scratch;
    u64 scratch_bytes;
    struct aes256_xts_key key;
    u32 slots = 1;
    u64 i;

    if (o->plain == 0) {
        fail("format needs --plain FSIMAGE");
    }
    image_fd = open(o->image, O_RDWR);
    if (image_fd < 0) {
        fail_errno("open image");
    }
    plain_fd = open(o->plain, O_RDONLY);
    if (plain_fd < 0) {
        fail_errno("open plain image");
    }
    if (fstat(plain_fd, &st) != 0) {
        fail_errno("stat plain image");
    }
    sectors = image_sectors(image_fd);
    data_sectors = sectors - CRUG_CRYPT_HEADER_SECTORS;
    plain_bytes = (u64)st.st_size;
    if (plain_bytes == 0 || plain_bytes > data_sectors * 512u) {
        fail("plain image does not fit the crypt data area");
    }

    random_bytes(salt, sizeof(salt));
    random_bytes(volume_id, sizeof(volume_id));
    random_bytes(master, sizeof(master));
    memset(header, 0, sizeof(header));
    if (crypt_meta_format_header(header, sectors, o->m_cost, o->t_cost, o->lanes, salt,
                                 volume_id) != 0) {
        fail("invalid KDF parameters");
    }
    scratch_bytes = crypt_meta_scratch_bytes((const struct crug_crypt_header *)header);
    scratch = malloc((size_t)scratch_bytes);
    if (scratch == 0) {
        fail("out of memory for the KDF workspace");
    }
    random_bytes(nonce, sizeof(nonce));
    if (crypt_meta_seal_slot(header, 0, CRUG_CRYPT_SLOT_FLAG_PASSWORD, nonce, 
                             (const u8 *)o->password, strlen(o->password), master, scratch,
                             (usize)scratch_bytes) != 0) {
        fail("cannot seal the password keyslot");
    }
    if (o->recovery != 0) {
        random_bytes(nonce, sizeof(nonce));
        if (crypt_meta_seal_slot(header, 1, CRUG_CRYPT_SLOT_FLAG_RECOVERY, nonce,
                                 (const u8 *)o->recovery, strlen(o->recovery), master, scratch,
                                 (usize)scratch_bytes) != 0) {
            fail("cannot seal the recovery keyslot");
        }
        slots = 2;
    }
    memset(scratch, 0, (usize)scratch_bytes);
    free(scratch);

    aes256_xts_set_key(&key, master);
    memset(master, 0, sizeof(master));
    for (i = 0; i < data_sectors; i++) {
        u8 tweak[16];
        u64 off = i * 512u;

        if (off + 512u <= plain_bytes) {
            read_at(plain_fd, sector, sizeof(sector), off, "read plain image");
        } else {
            memset(sector, 0, sizeof(sector));
            if (off < plain_bytes) {
                u64 part = plain_bytes - off;

                read_at(plain_fd, sector, (usize)part, off, "read plain image");
            }
        }
        aes256_xts_sector_tweak(tweak, i);
        aes256_xts_encrypt(&key, tweak, sector, sizeof(sector));
        write_at(image_fd, sector, sizeof(sector), DATA_OFFSET + i * 512u, "write data");
    }
    aes256_xts_wipe(&key);
    write_at(image_fd, header, sizeof(header), 0, "write header");
    if (fsync(image_fd) != 0) {
        fail_errno("fsync image");
    }
    close(plain_fd);
    close(image_fd);
    memset(header, 0, sizeof(header));
    printf("mkcrypt: formatted %s: %llu data sectors, %u keyslot(s), Argon2id m=%u KiB t=%u p=%u\n",
           o->image, data_sectors, slots, o->m_cost, o->t_cost, o->lanes);
}

static void extract_volume(const struct options *o)
{
    int image_fd;
    int out_fd;
    u64 sectors;
    u64 data_sectors;
    u8 header[CRUG_CRYPT_HEADER_BYTES];
    u8 master[CRUG_CRYPT_MASTER_BYTES];
    struct crug_crypt_header *h = (struct crug_crypt_header *)header;
    void *scratch;
    u64 scratch_bytes;
    struct aes256_xts_key key;
    u32 slot;
    int unlocked = 0;
    u8 sector[512];
    u64 i;

    if (o->out == 0) {
        fail("extract needs an output file");
    }
    image_fd = open(o->image, O_RDONLY);
    if (image_fd < 0) {
        fail_errno("open image");
    }
    sectors = image_sectors(image_fd);
    read_at(image_fd, header, sizeof(header), 0, "read header");
    if (crypt_meta_validate(h) != 0) {
        fail("not a CrugOS crypt volume");
    }
    scratch_bytes = crypt_meta_scratch_bytes(h);
    scratch = malloc((size_t)scratch_bytes);
    if (scratch == 0) {
        fail("out of memory for the KDF workspace");
    }
    for (slot = 0; slot < h->slot_count; slot++) {
        if (crypt_meta_unseal_slot(header, slot, (const u8 *)o->password, strlen(o->password),
                                   master, scratch, (usize)scratch_bytes) == 0) {
            unlocked = 1;
            break;
        }
    }
    memset(scratch, 0, (usize)scratch_bytes);
    free(scratch);
    if (!unlocked) {
        fail("wrong password or damaged volume");
    }
    data_sectors = sectors - CRUG_CRYPT_HEADER_SECTORS;
    out_fd = open(o->out, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out_fd < 0) {
        fail_errno("open output");
    }
    aes256_xts_set_key(&key, master);
    memset(master, 0, sizeof(master));
    for (i = 0; i < data_sectors; i++) {
        u8 tweak[16];

        read_at(image_fd, sector, sizeof(sector), DATA_OFFSET + i * 512u, "read data");
        aes256_xts_sector_tweak(tweak, i);
        aes256_xts_decrypt(&key, tweak, sector, sizeof(sector));
        write_at(out_fd, sector, sizeof(sector), i * 512u, "write output");
    }
    aes256_xts_wipe(&key);
    if (fsync(out_fd) != 0) {
        fail_errno("fsync output");
    }
    close(out_fd);
    close(image_fd);
    printf("mkcrypt: extracted %llu data sectors from %s to %s\n", data_sectors, o->image,
           o->out);
}

int main(int argc, char **argv)
{
    struct options o;

    memset(&o, 0, sizeof(o));
    o.m_cost = 4096;
    o.t_cost = 3;
    o.lanes = 1;
    if (argc < 3) {
        usage();
    }
    if (strcmp(argv[1], "format") == 0) {
        o.image = argv[2];
        parse_common(argc, argv, 3, &o);
        format_volume(&o);
        return 0;
    }
    if (strcmp(argv[1], "extract") == 0) {
        if (argc < 4) {
            usage();
        }
        o.image = argv[2];
        o.out = argv[3];
        parse_common(argc, argv, 4, &o);
        extract_volume(&o);
        return 0;
    }
    usage();
    return 2;
}
